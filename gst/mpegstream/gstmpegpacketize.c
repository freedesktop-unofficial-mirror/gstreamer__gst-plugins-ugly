/* GStreamer
 * Copyright (C) <1999> Erik Walthinsen <omega@cse.ogi.edu>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */


/*#define GST_DEBUG_ENABLED */
#include <gstmpegpacketize.h>

GstMPEGPacketize*
gst_mpeg_packetize_new (GstPad *pad, GstMPEGPacketizeType type)
{
  GstMPEGPacketize *new;

  g_return_val_if_fail (pad != NULL, NULL);
  g_return_val_if_fail (GST_IS_PAD (pad), NULL);
  
  new = g_malloc (sizeof (GstMPEGPacketize));
  
  gst_object_ref (GST_OBJECT (pad));
  new->id = 0;
  new->pad = pad;
  new->bs = gst_bytestream_new (pad);
  new->MPEG2 = FALSE;
  new->type = type;

  return new;
}

void
gst_mpeg_packetize_destroy (GstMPEGPacketize *packetize)
{
  g_return_if_fail (packetize != NULL);

  gst_bytestream_destroy (packetize->bs);
  gst_object_unref (GST_OBJECT (packetize->pad));

  g_free (packetize);
}

static GstData*
parse_packhead (GstMPEGPacketize * packetize)
{
  gint length = 8 + 4;
  guint8 *buf;
  GstBuffer *outbuf;
  guint32 got_bytes;

  GST_DEBUG (0, "packetize: in parse_packhead");

  got_bytes = gst_bytestream_peek_bytes (packetize->bs, &buf, length);
  if (got_bytes < length) return NULL;
  buf += 4;

  GST_DEBUG (0, "code %02x", *buf);

  /* start parsing the stream */
  if ((*buf & 0xf0) == 0x40) {
    GST_DEBUG (0, "packetize::parse_packhead setting mpeg2");
    packetize->MPEG2 = TRUE;
    length += 2;
    got_bytes = gst_bytestream_peek_bytes (packetize->bs, &buf, length);
    if (got_bytes < length) return NULL;
  }
  else {
    GST_DEBUG (0, "packetize::parse_packhead setting mpeg1");
    packetize->MPEG2 = FALSE;
  }

  got_bytes = gst_bytestream_read (packetize->bs, &outbuf, length);
  if (got_bytes < length) return NULL;

  return GST_DATA (outbuf);
}

static inline GstData*
parse_generic (GstMPEGPacketize *packetize)
{
  guint16 length;
  GstByteStream *bs = packetize->bs;
  guchar *buf;
  GstBuffer *outbuf;
  guint32 got_bytes;

  GST_DEBUG (0, "packetize: in parse_generic");

  got_bytes = gst_bytestream_peek_bytes (bs, (guint8**)&buf, 2 + 4);
  if (got_bytes < 6) return NULL;
  buf += 4;

  length = GUINT16_FROM_BE (*(guint16 *) buf);
  GST_DEBUG (0, "packetize: header_length %d", length);

  got_bytes = gst_bytestream_read (packetize->bs, &outbuf, 2 + length + 4);
  if (got_bytes < 2 + length + 4) return NULL;

  return GST_DATA (outbuf);
}

static inline GstData*
parse_chunk (GstMPEGPacketize *packetize)
{
  GstByteStream *bs = packetize->bs;
  guchar *buf;
  gint offset;
  guint32 code;
  const gint chunksize = 4096;
  GstBuffer *outbuf = NULL;
  guint32 got_bytes;

  got_bytes = gst_bytestream_peek_bytes (bs, (guint8**)&buf, chunksize);
  if (got_bytes < chunksize)
    return FALSE;
  offset = 4;

  code = GUINT32_FROM_BE (*((guint32 *)(buf+offset)));

  GST_DEBUG (0, "code = %08x", code);

  while ((code & 0xffffff00) != 0x100L) {
    code = (code << 8) | buf[offset++];
    
    GST_DEBUG (0, "  code = %08x", code);

    if ((offset % chunksize) == 0) {
      got_bytes = gst_bytestream_peek_bytes (bs, (guint8**)&buf, offset + chunksize);
      if (got_bytes < offset + chunksize)
	return NULL;
    }
  }
  if (offset > 4) {
     got_bytes = gst_bytestream_read (bs, &outbuf, offset-4);
  }
  return GST_DATA (outbuf);
}


/* FIXME mmx-ify me */
static inline gboolean
find_start_code (GstMPEGPacketize *packetize)
{
  GstByteStream *bs = packetize->bs;
  guchar *buf;
  gint offset;
  guint32 code;
  const gint chunksize = 4096;
  guint32 got_bytes;

  got_bytes = gst_bytestream_peek_bytes (bs, (guint8**)&buf, chunksize);
  if (got_bytes < chunksize)
    return FALSE;
  offset = 4;

  code = GUINT32_FROM_BE (*((guint32 *)(buf)));

  GST_DEBUG (0, "code = %08x", code);

  while ((code & 0xffffff00) != 0x100L) {
    code = (code << 8) | buf[offset++];
    
    GST_DEBUG (0, "  code = %08x", code);

    if (offset == chunksize) {
      if (!gst_bytestream_flush (bs, offset))
	return FALSE;
      got_bytes = gst_bytestream_peek_bytes (bs, (guint8**)&buf, chunksize);
      if (got_bytes < chunksize)
	return FALSE;
      offset = 0;
    }
  }
  packetize->id = code & 0xff;
  if (offset > 4) {
    if (!gst_bytestream_flush (bs, offset - 4))
      return FALSE;
  }
  return TRUE;
}

GstData*
gst_mpeg_packetize_read (GstMPEGPacketize *packetize)
{
  gboolean got_event = FALSE;
  GstData *outbuf = NULL;

  while (outbuf == NULL) {
    if (!find_start_code (packetize))
      got_event = TRUE;
    else {
      GST_DEBUG (0, "packetize: have chunk 0x%02X", packetize->id);
      if (packetize->type == GST_MPEG_PACKETIZE_SYSTEM) {
	if (packetize->resync) {
          if (packetize->id != PACK_START_CODE) {
            if (!gst_bytestream_flush (packetize->bs, 4))
              got_event = TRUE;
	    continue;
	  }

	  packetize->resync = FALSE;
	}
        switch (packetize->id) {
          case PACK_START_CODE:
    	    outbuf = parse_packhead (packetize);
	    if (!outbuf)
	      got_event = TRUE;
	    break;
          case SYS_HEADER_START_CODE:
	    outbuf = parse_generic (packetize);
	    if (!outbuf)
	      got_event = TRUE;
	    break;
          default:
	    if (packetize->MPEG2 && ((packetize->id < 0xBD) || (packetize->id > 0xFE))) {
	      g_warning ("packetize: ******** unknown id 0x%02X",packetize->id);
	    }
	    else {
	      outbuf = parse_generic (packetize);
	      if (!outbuf)
	        got_event = TRUE;
	    }
        }
      }
      else if (packetize->type == GST_MPEG_PACKETIZE_VIDEO) {
	outbuf = parse_chunk (packetize);
      }
      else {
	g_assert_not_reached ();
      }
    }

    if (got_event) {
      guint32 remaining;
      GstEvent *event;
      gint etype;

      gst_bytestream_get_status (packetize->bs, &remaining, &event);
      etype = event? GST_EVENT_TYPE (event) : GST_EVENT_EOS;

      g_print ("remaining %d\n", remaining);

      switch (etype) {
        case GST_EVENT_DISCONTINUOUS:
          GST_DEBUG (GST_CAT_EVENT, "packetize: discont\n");
	  gst_bytestream_flush_fast (packetize->bs, remaining);
	  break;
      }

      return GST_DATA (event);
    }
  }

  return outbuf;
}
