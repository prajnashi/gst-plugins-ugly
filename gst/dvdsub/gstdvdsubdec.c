/* GStreamer
 * Copyright (C) <2005> Jan Schmidt <jan@fluendo.com>
 * Copyright (C) <2002> Wim Taymans <wim@fluendo.com>
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

/* TODO: liboil-ise code, esp. use _splat() family of functions */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstdvdsubdec.h"
#include "gstdvdsubparse.h"
#include <string.h>

GST_BOILERPLATE (GstDvdSubDec, gst_dvd_sub_dec, GstElement, GST_TYPE_ELEMENT);

static gboolean gst_dvd_sub_dec_src_event (GstPad * srcpad, GstEvent * event);
static GstFlowReturn gst_dvd_sub_dec_chain (GstPad * pad, GstBuffer * buf);

static gboolean gst_dvd_sub_dec_handle_dvd_event (GstDvdSubDec * dec,
    GstEvent * event);
static void gst_dvd_sub_dec_finalize (GObject * gobject);
static void gst_setup_palette (GstDvdSubDec * dec);
static void gst_dvd_sub_dec_merge_title (GstDvdSubDec * dec, GstBuffer * buf);
static GstClockTime gst_dvd_sub_dec_get_event_delay (GstDvdSubDec * dec);
static gboolean gst_dvd_sub_dec_sink_event (GstPad * pad, GstEvent * event);

static GstFlowReturn gst_send_subtitle_frame (GstDvdSubDec * dec,
    GstClockTime end_ts);

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-raw-yuv, format = (fourcc) AYUV, "
        "width = (int) 720, height = (int) 576, framerate = (fraction) 0/1")
    );

static GstStaticPadTemplate subtitle_template = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-dvd-subpicture")
    );

GST_DEBUG_CATEGORY_STATIC (gst_dvd_sub_dec_debug);
#define GST_CAT_DEFAULT (gst_dvd_sub_dec_debug)

enum
{
  SPU_FORCE_DISPLAY = 0x00,
  SPU_SHOW = 0x01,
  SPU_HIDE = 0x02,
  SPU_SET_PALETTE = 0x03,
  SPU_SET_ALPHA = 0x04,
  SPU_SET_SIZE = 0x05,
  SPU_SET_OFFSETS = 0x06,
  SPU_WIPE = 0x07,
  SPU_END = 0xff
};

static const guint32 default_clut[16] = {
  0xb48080, 0x248080, 0x628080, 0xd78080,
  0x808080, 0x808080, 0x808080, 0x808080,
  0x808080, 0x808080, 0x808080, 0x808080,
  0x808080, 0x808080, 0x808080, 0x808080
};

typedef struct RLE_state
{
  gint id;
  gint aligned;
  gint offset[2];
  gint hl_left;
  gint hl_right;

  guchar *target;

  guchar next;
}
RLE_state;

static void
gst_dvd_sub_dec_base_init (gpointer klass)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&src_template));
  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&subtitle_template));

  gst_element_class_set_details_simple (element_class, "DVD subtitle decoder",
      "Codec/Decoder/Video", "Decodes DVD subtitles into AYUV video frames",
      "Wim Taymans <wim.taymans@gmail.com>, "
      "Jan Schmidt <thaytan@mad.scientist.com>");
}

static void
gst_dvd_sub_dec_class_init (GstDvdSubDecClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;

  gobject_class = (GObjectClass *) klass;
  gstelement_class = (GstElementClass *) klass;

  gobject_class->finalize = gst_dvd_sub_dec_finalize;
}

static void
gst_dvd_sub_dec_init (GstDvdSubDec * dec, GstDvdSubDecClass * klass)
{
  GstPadTemplate *tmpl;

  dec->sinkpad = gst_pad_new_from_static_template (&subtitle_template, "sink");
  gst_pad_set_chain_function (dec->sinkpad,
      GST_DEBUG_FUNCPTR (gst_dvd_sub_dec_chain));
  gst_pad_set_event_function (dec->sinkpad,
      GST_DEBUG_FUNCPTR (gst_dvd_sub_dec_sink_event));
  gst_element_add_pad (GST_ELEMENT (dec), dec->sinkpad);

  tmpl = gst_static_pad_template_get (&src_template);
  dec->srcpad = gst_pad_new_from_template (tmpl, "src");
  gst_pad_set_event_function (dec->srcpad,
      GST_DEBUG_FUNCPTR (gst_dvd_sub_dec_src_event));
  gst_pad_use_fixed_caps (dec->srcpad);
  gst_pad_set_caps (dec->srcpad, gst_pad_template_get_caps (tmpl));
  gst_object_unref (tmpl);
  gst_element_add_pad (GST_ELEMENT (dec), dec->srcpad);

  /* FIXME: aren't there more possible sizes? (tpm) */
  dec->in_width = 720;
  dec->in_height = 576;

  dec->partialbuf = NULL;
  dec->have_title = FALSE;
  dec->parse_pos = NULL;
  dec->forced_display = FALSE;
  dec->visible = FALSE;

  memset (dec->menu_index, 0, sizeof (dec->menu_index));
  memset (dec->menu_alpha, 0, sizeof (dec->menu_alpha));
  memset (dec->subtitle_index, 0, sizeof (dec->subtitle_index));
  memset (dec->subtitle_alpha, 0, sizeof (dec->subtitle_alpha));
  memcpy (dec->current_clut, default_clut, sizeof (guint32) * 16);

  gst_setup_palette (dec);

  dec->next_ts = 0;
  dec->next_event_ts = GST_CLOCK_TIME_NONE;

  dec->out_buffer = NULL;
  dec->buf_dirty = TRUE;
}

static void
gst_dvd_sub_dec_finalize (GObject * gobject)
{
  GstDvdSubDec *dec = GST_DVD_SUB_DEC (gobject);

  if (dec->partialbuf) {
    gst_buffer_unref (dec->partialbuf);
    dec->partialbuf = NULL;
  }

  G_OBJECT_CLASS (parent_class)->finalize (gobject);
}

static gboolean
gst_dvd_sub_dec_src_event (GstPad * pad, GstEvent * event)
{
  GstDvdSubDec *dec = GST_DVD_SUB_DEC (gst_pad_get_parent (pad));
  gboolean res = FALSE;

  switch (GST_EVENT_TYPE (event)) {
    default:
      res = gst_pad_event_default (pad, event);
      break;
  }

  gst_object_unref (dec);
  return res;
}

static GstClockTime
gst_dvd_sub_dec_get_event_delay (GstDvdSubDec * dec)
{
  guchar *start = GST_BUFFER_DATA (dec->partialbuf);
  guchar *buf;
  guint16 ticks;
  GstClockTime event_delay;

  /* If starting a new buffer, follow the first DCSQ ptr */
  if (dec->parse_pos == start) {
    buf = dec->parse_pos + dec->data_size;
  } else {
    buf = dec->parse_pos;
  }

  ticks = GST_READ_UINT16_BE (buf);
  event_delay = gst_util_uint64_scale (ticks, 1024 * GST_SECOND, 90000);

  GST_DEBUG_OBJECT (dec, "returning delay %" GST_TIME_FORMAT " from offset %u",
      GST_TIME_ARGS (event_delay), (guint) (buf - dec->parse_pos));

  return event_delay;
}

/*
 * Parse the next event time in the current subpicture buffer, stopping
 * when time advances to the next state. 
 */
static void
gst_dvd_sub_dec_parse_subpic (GstDvdSubDec * dec)
{
#define PARSE_BYTES_NEEDED(x) if ((buf+(x)) >= end) \
  { GST_WARNING("Subtitle stream broken parsing %c", *buf); \
    broken = TRUE; break; }

  guchar *start = GST_BUFFER_DATA (dec->partialbuf);
  guchar *buf;
  guchar *end;
  gboolean broken = FALSE;
  gboolean last_seq = FALSE;
  guchar *next_seq = NULL;
  GstClockTime event_time;

  /* nothing to do if we finished this buffer already */
  if (dec->parse_pos == NULL)
    return;

  g_return_if_fail (dec->packet_size >= 4);

  end = start + dec->packet_size;
  if (dec->parse_pos == start) {
    buf = dec->parse_pos + dec->data_size;
  } else {
    buf = dec->parse_pos;
  }

  g_assert (buf >= start && buf < end);

  /* If the next control sequence is at the current offset, this is 
   * the last one */
  next_seq = start + GST_READ_UINT16_BE (buf + 2);
  last_seq = (next_seq == buf);
  buf += 4;

  while ((buf < end) && (!broken)) {
    switch (*buf) {
      case SPU_FORCE_DISPLAY:  /* Forced display menu subtitle */
        dec->forced_display = TRUE;
        dec->buf_dirty = TRUE;
        GST_DEBUG_OBJECT (dec, "SPU FORCE_DISPLAY");
        buf++;
        break;
      case SPU_SHOW:           /* Show the subtitle in this packet */
        dec->visible = TRUE;
        dec->buf_dirty = TRUE;
        GST_DEBUG_OBJECT (dec, "SPU SHOW at %" GST_TIME_FORMAT,
            GST_TIME_ARGS (dec->next_event_ts));
        buf++;
        break;
      case SPU_HIDE:
        /* 02 ff (ff) is the end of the packet, hide the subpicture */
        dec->visible = FALSE;
        dec->buf_dirty = TRUE;

        GST_DEBUG_OBJECT (dec, "SPU HIDE at %" GST_TIME_FORMAT,
            GST_TIME_ARGS (dec->next_event_ts));
        buf++;
        break;
      case SPU_SET_PALETTE:    /* palette */
        PARSE_BYTES_NEEDED (3);

        GST_DEBUG_OBJECT (dec, "SPU SET_PALETTE");

        dec->subtitle_index[3] = buf[1] >> 4;
        dec->subtitle_index[2] = buf[1] & 0xf;
        dec->subtitle_index[1] = buf[2] >> 4;
        dec->subtitle_index[0] = buf[2] & 0xf;
        gst_setup_palette (dec);

        dec->buf_dirty = TRUE;
        buf += 3;
        break;
      case SPU_SET_ALPHA:      /* transparency palette */
        PARSE_BYTES_NEEDED (3);

        GST_DEBUG_OBJECT (dec, "SPU SET_ALPHA");

        dec->subtitle_alpha[3] = buf[1] >> 4;
        dec->subtitle_alpha[2] = buf[1] & 0xf;
        dec->subtitle_alpha[1] = buf[2] >> 4;
        dec->subtitle_alpha[0] = buf[2] & 0xf;
        gst_setup_palette (dec);

        dec->buf_dirty = TRUE;
        buf += 3;
        break;
      case SPU_SET_SIZE:       /* image coordinates */
        PARSE_BYTES_NEEDED (7);

        dec->left =
            CLAMP ((((guint) buf[1]) << 4) | (buf[2] >> 4), 0,
            (dec->in_width - 1));
        dec->top =
            CLAMP ((((guint) buf[4]) << 4) | (buf[5] >> 4), 0,
            (dec->in_height - 1));
        dec->right =
            CLAMP ((((buf[2] & 0x0f) << 8) | buf[3]), 0, (dec->in_width - 1));
        dec->bottom =
            CLAMP ((((buf[5] & 0x0f) << 8) | buf[6]), 0, (dec->in_height - 1));

        GST_DEBUG_OBJECT (dec, "SPU SET_SIZE left %d, top %d, right %d, "
            "bottom %d", dec->left, dec->top, dec->right, dec->bottom);

        dec->buf_dirty = TRUE;
        buf += 7;
        break;
      case SPU_SET_OFFSETS:    /* image 1 / image 2 offsets */
        PARSE_BYTES_NEEDED (5);

        dec->offset[0] = (((guint) buf[1]) << 8) | buf[2];
        dec->offset[1] = (((guint) buf[3]) << 8) | buf[4];
        GST_DEBUG_OBJECT (dec, "Offset1 %d, Offset2 %d",
            dec->offset[0], dec->offset[1]);

        dec->buf_dirty = TRUE;
        buf += 5;
        break;
      case SPU_WIPE:
      {
        guint length;

        PARSE_BYTES_NEEDED (3);

        GST_WARNING_OBJECT (dec, "SPU_WIPE not yet implemented");

        length = (buf[1] << 8) | (buf[2]);
        buf += 1 + length;

        dec->buf_dirty = TRUE;
        break;
      }
      case SPU_END:
        buf = (last_seq) ? end : next_seq;

        /* Start a new control sequence */
        if (buf + 4 < end) {
          guint16 ticks = GST_READ_UINT16_BE (buf);

          event_time = gst_util_uint64_scale (ticks, 1024 * GST_SECOND, 90000);

          GST_DEBUG_OBJECT (dec,
              "Next DCSQ at offset %d, delay %g secs (%d ticks)", buf - start,
              gst_util_guint64_to_gdouble (event_time / GST_SECOND), ticks);

          dec->parse_pos = buf;
          if (event_time > 0) {
            dec->next_event_ts += event_time;

            GST_LOG_OBJECT (dec, "Exiting parse loop with time %g",
                gst_guint64_to_gdouble (dec->next_event_ts) /
                gst_guint64_to_gdouble (GST_SECOND));
            return;
          }
        } else {
          dec->parse_pos = NULL;
          dec->next_event_ts = GST_CLOCK_TIME_NONE;
          GST_LOG_OBJECT (dec, "Finished all cmds. Exiting parse loop");
          return;
        }
      default:
        GST_ERROR
            ("Invalid sequence in subtitle packet header (%.2x). Skipping",
            *buf);
        broken = TRUE;
        dec->parse_pos = NULL;
        break;
    }
  }
}

static inline int
gst_get_nibble (guchar * buffer, RLE_state * state)
{
  if (state->aligned) {
    state->next = buffer[state->offset[state->id]++];
    state->aligned = 0;
    return state->next >> 4;
  } else {
    state->aligned = 1;
    return state->next & 0xf;
  }
}

/* Premultiply the current lookup table into the "target" cache */
static void
gst_setup_palette (GstDvdSubDec * dec)
{
  gint i;
  guint32 col;
  YUVA_val *target = dec->palette_cache;
  YUVA_val *target2 = dec->hl_palette_cache;

  for (i = 0; i < 4; i++, target2++, target++) {
    col = dec->current_clut[dec->subtitle_index[i]];
    target->Y = (col >> 16) & 0xff;
    target->V = (col >> 8) & 0xff;
    target->U = col & 0xff;
    target->A = dec->subtitle_alpha[i] * 0xff / 0xf;

    col = dec->current_clut[dec->menu_index[i]];
    target2->Y = (col >> 16) & 0xff;
    target2->V = (col >> 8) & 0xff;
    target2->U = col & 0xff;
    target2->A = dec->menu_alpha[i] * 0xff / 0xf;
  }
}

static inline guint
gst_get_rle_code (guchar * buffer, RLE_state * state)
{
  gint code;

  code = gst_get_nibble (buffer, state);
  if (code < 0x4) {             /* 4 .. f */
    code = (code << 4) | gst_get_nibble (buffer, state);
    if (code < 0x10) {          /* 1x .. 3x */
      code = (code << 4) | gst_get_nibble (buffer, state);
      if (code < 0x40) {        /* 04x .. 0fx */
        code = (code << 4) | gst_get_nibble (buffer, state);
      }
    }
  }
  return code;
}

#define DRAW_RUN(target,len,c)                  \
G_STMT_START {                                  \
  if ((c)->A) {                                 \
    gint i;                                     \
    for (i = 0; i < (len); i++) {               \
      *(target)++ = (c)->A;                     \
      *(target)++ = (c)->Y;                     \
      *(target)++ = (c)->U;                     \
      *(target)++ = (c)->V;                     \
    }                                           \
  } else {                                      \
    (target) += 4 * (len);                      \
  }                                             \
} G_STMT_END

/* 
 * This function steps over each run-length segment, drawing 
 * into the YUVA buffers as it goes. UV are composited and then output
 * at half width/height
 */
static void
gst_draw_rle_line (GstDvdSubDec * dec, guchar * buffer, RLE_state * state)
{
  gint length, colourid;
  guint code;
  gint x, right;
  guchar *target;

  target = state->target;

  x = dec->left;
  right = dec->right + 1;

  while (x < right) {
    gboolean in_hl;
    const YUVA_val *colour_entry;

    code = gst_get_rle_code (buffer, state);
    length = code >> 2;
    colourid = code & 3;
    colour_entry = dec->palette_cache + colourid;

    /* Length = 0 implies fill to the end of the line */
    /* Restrict the colour run to the end of the line */
    if (length == 0 || x + length > right)
      length = right - x;

    /* Check if this run of colour touches the highlight region */
    in_hl = ((x <= state->hl_right) && (x + length) >= state->hl_left);
    if (in_hl) {
      gint run;

      /* Draw to the left of the highlight */
      if (x <= state->hl_left) {
        run = MIN (length, state->hl_left - x + 1);

        DRAW_RUN (target, run, colour_entry);
        length -= run;
        x += run;
      }

      /* Draw across the highlight region */
      if (x <= state->hl_right) {
        const YUVA_val *hl_colour = dec->hl_palette_cache + colourid;

        run = MIN (length, state->hl_right - x + 1);

        DRAW_RUN (target, run, hl_colour);
        length -= run;
        x += run;
      }
    }

    /* Draw the rest of the run */
    if (length > 0) {
      DRAW_RUN (target, length, colour_entry);
      x += length;
    }
  }
}

/*
 * Decode the RLE subtitle image and blend with the current
 * frame buffer.
 */
static void
gst_dvd_sub_dec_merge_title (GstDvdSubDec * dec, GstBuffer * buf)
{
  gint y;
  gint Y_stride = 4 * dec->in_width;
  guchar *buffer = GST_BUFFER_DATA (dec->partialbuf);

  gint hl_top, hl_bottom;
  gint last_y;
  RLE_state state;

  GST_DEBUG_OBJECT (dec, "Merging subtitle on frame at time %" GST_TIME_FORMAT,
      GST_TIME_ARGS (GST_BUFFER_TIMESTAMP (buf)));

  state.id = 0;
  state.aligned = 1;
  state.next = 0;
  state.offset[0] = dec->offset[0];
  state.offset[1] = dec->offset[1];

  if (dec->current_button) {
    hl_top = dec->hl_top;
    hl_bottom = dec->hl_bottom;
  } else {
    hl_top = -1;
    hl_bottom = -1;
  }
  last_y = MIN (dec->bottom, dec->in_height);

  y = dec->top;
  state.target = GST_BUFFER_DATA (buf) + 4 * dec->left + (y * Y_stride);

  /* Now draw scanlines until we hit last_y or end of RLE data */
  for (; ((state.offset[1] < dec->data_size + 2) && (y <= last_y)); y++) {
    /* Set up to draw the highlight if we're in the right scanlines */
    if (y > hl_bottom || y < hl_top) {
      state.hl_left = -1;
      state.hl_right = -1;
    } else {
      state.hl_left = dec->hl_left;
      state.hl_right = dec->hl_right;
    }
    gst_draw_rle_line (dec, buffer, &state);

    state.target += Y_stride;

    /* Realign the RLE state for the next line */
    if (!state.aligned)
      gst_get_nibble (buffer, &state);
    state.id = !state.id;
  }
}

static void
gst_send_empty_fill (GstDvdSubDec * dec, GstClockTime ts)
{
  if (dec->next_ts < ts) {
    GST_LOG_OBJECT (dec, "Sending newsegment update to advance time to %"
        GST_TIME_FORMAT, GST_TIME_ARGS (ts));

    gst_pad_push_event (dec->srcpad,
        gst_event_new_new_segment (TRUE, 1.0, GST_FORMAT_TIME, ts, -1, ts));
  }
  dec->next_ts = ts;
}

static GstFlowReturn
gst_send_subtitle_frame (GstDvdSubDec * dec, GstClockTime end_ts)
{
  GstFlowReturn flow;
  GstBuffer *out_buf;
  gint x, y;

  g_assert (dec->have_title);
  g_assert (dec->next_ts <= end_ts);

  /* Check if we need to redraw the output buffer */
  if (dec->buf_dirty) {
    if (dec->out_buffer) {
      gst_buffer_unref (dec->out_buffer);
      dec->out_buffer = NULL;
    }

    flow = gst_pad_alloc_buffer_and_set_caps (dec->srcpad, 0,
        4 * dec->in_width * dec->in_height, GST_PAD_CAPS (dec->srcpad),
        &out_buf);

    if (flow != GST_FLOW_OK) {
      GST_DEBUG_OBJECT (dec, "alloc buffer failed: flow = %s",
          gst_flow_get_name (flow));
      goto out;
    }

    /* Clear the buffer */
    /* FIXME - move this into the buffer rendering code */
    for (y = 0; y < dec->in_height; y++) {
      guchar *line = GST_BUFFER_DATA (out_buf) + 4 * dec->in_width * y;

      for (x = 0; x < dec->in_width; x++) {
        line[0] = 0;            /* A */
        line[1] = 16;           /* Y */
        line[2] = 128;          /* U */
        line[3] = 128;          /* V */

        line += 4;
      }
    }

    /* FIXME: do we really want to honour the forced_display flag
     * for subtitles streans? */
    if (dec->visible || dec->forced_display) {
      gst_dvd_sub_dec_merge_title (dec, out_buf);
    }

    dec->out_buffer = out_buf;
    dec->buf_dirty = FALSE;
  }

  out_buf = gst_buffer_create_sub (dec->out_buffer, 0,
      GST_BUFFER_SIZE (dec->out_buffer));

  GST_BUFFER_TIMESTAMP (out_buf) = dec->next_ts;
  GST_BUFFER_DURATION (out_buf) = GST_CLOCK_DIFF (dec->next_ts, end_ts);

  GST_DEBUG_OBJECT (dec, "Sending subtitle buffer with ts %"
      GST_TIME_FORMAT ", dur %" G_GINT64_FORMAT,
      GST_TIME_ARGS (GST_BUFFER_TIMESTAMP (out_buf)),
      GST_BUFFER_DURATION (out_buf));

  gst_buffer_set_caps (out_buf, GST_PAD_CAPS (dec->srcpad));

  flow = gst_pad_push (dec->srcpad, out_buf);

out:

  dec->next_ts = end_ts;
  return flow;
}

/* Walk time forward, processing any subtitle events as needed. */
static GstFlowReturn
gst_dvd_sub_dec_advance_time (GstDvdSubDec * dec, GstClockTime new_ts)
{
  GstFlowReturn ret = GST_FLOW_OK;

  GST_LOG_OBJECT (dec, "Advancing time to %" GST_TIME_FORMAT,
      GST_TIME_ARGS (new_ts));

  if (!dec->have_title) {
    gst_send_empty_fill (dec, new_ts);
    return ret;
  }

  while (dec->next_ts < new_ts) {
    GstClockTime next_ts = new_ts;

    if (GST_CLOCK_TIME_IS_VALID (dec->next_event_ts) &&
        dec->next_event_ts < next_ts) {
      /* We might need to process the subtitle cmd queue */
      next_ts = dec->next_event_ts;
    }

    /* 
     * Now, either output a filler or a frame spanning
     * dec->next_ts to next_ts
     */
    if (dec->visible || dec->forced_display) {
      ret = gst_send_subtitle_frame (dec, next_ts);
    } else {
      gst_send_empty_fill (dec, next_ts);
    }

    /*
     * and then process some subtitle cmds if we need
     */
    if (next_ts == dec->next_event_ts)
      gst_dvd_sub_dec_parse_subpic (dec);
  }

  return ret;
}

static GstFlowReturn
gst_dvd_sub_dec_chain (GstPad * pad, GstBuffer * buf)
{
  GstFlowReturn ret = GST_FLOW_OK;
  GstDvdSubDec *dec;
  guint8 *data;
  glong size = 0;

  dec = GST_DVD_SUB_DEC (GST_PAD_PARENT (pad));

  GST_DEBUG_OBJECT (dec, "Have buffer of size %d, ts %"
      GST_TIME_FORMAT ", dur %" G_GINT64_FORMAT, GST_BUFFER_SIZE (buf),
      GST_TIME_ARGS (GST_BUFFER_TIMESTAMP (buf)), GST_BUFFER_DURATION (buf));

  if (GST_BUFFER_TIMESTAMP_IS_VALID (buf)) {
    if (!GST_CLOCK_TIME_IS_VALID (dec->next_ts)) {
      dec->next_ts = GST_BUFFER_TIMESTAMP (buf);
    }

    /* Move time forward to the start of the new buffer */
    ret = gst_dvd_sub_dec_advance_time (dec, GST_BUFFER_TIMESTAMP (buf));
  }

  if (dec->have_title) {
    gst_buffer_unref (dec->partialbuf);
    dec->partialbuf = NULL;
    dec->have_title = FALSE;
  }

  GST_DEBUG_OBJECT (dec, "Got subtitle buffer, pts %" GST_TIME_FORMAT,
      GST_TIME_ARGS (GST_BUFFER_TIMESTAMP (buf)));

  /* deal with partial frame from previous buffer */
  if (dec->partialbuf) {
    GstBuffer *merge;

    merge = gst_buffer_join (dec->partialbuf, buf);
    dec->partialbuf = merge;
  } else {
    dec->partialbuf = buf;
  }

  data = GST_BUFFER_DATA (dec->partialbuf);
  size = GST_BUFFER_SIZE (dec->partialbuf);

  if (size > 4) {
    dec->packet_size = GST_READ_UINT16_BE (data);

    if (dec->packet_size == size) {
      GST_LOG_OBJECT (dec, "Subtitle packet size %d, current size %ld",
          dec->packet_size, size);

      dec->data_size = GST_READ_UINT16_BE (data + 2);

      /* Reset parameters for a new subtitle buffer */
      dec->parse_pos = data;
      dec->forced_display = FALSE;
      dec->visible = FALSE;

      dec->have_title = TRUE;
      dec->next_event_ts = GST_BUFFER_TIMESTAMP (dec->partialbuf);

      if (!GST_CLOCK_TIME_IS_VALID (dec->next_event_ts))
        dec->next_event_ts = dec->next_ts;

      dec->next_event_ts += gst_dvd_sub_dec_get_event_delay (dec);
    }
  }

  return ret;
}

static gboolean
gst_dvd_sub_dec_sink_event (GstPad * pad, GstEvent * event)
{
  GstDvdSubDec *dec = GST_DVD_SUB_DEC (gst_pad_get_parent (pad));
  gboolean ret = FALSE;

  GST_LOG_OBJECT (dec, "%s event", GST_EVENT_TYPE_NAME (event));

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_CUSTOM_DOWNSTREAM:{
      GstClockTime ts = GST_EVENT_TIMESTAMP (event);

      if (event->structure != NULL &&
          gst_structure_has_name (event->structure, "application/x-gst-dvd")) {

        if (GST_CLOCK_TIME_IS_VALID (ts))
          gst_dvd_sub_dec_advance_time (dec, ts);

        if (gst_dvd_sub_dec_handle_dvd_event (dec, event)) {
          /* gst_dvd_sub_dec_advance_time (dec, dec->next_ts + GST_SECOND / 30.0); */
          gst_event_unref (event);
          ret = TRUE;
          break;
        }
      }

      ret = gst_pad_event_default (pad, event);
      break;
    }
    case GST_EVENT_NEWSEGMENT:{
      gboolean update;
      GstFormat format;
      gint64 start, stop, pos;

      gst_event_parse_new_segment (event, &update, NULL, &format, &start,
          &stop, &pos);

      if (update) {
        /* update ... advance time */
        if (GST_CLOCK_TIME_IS_VALID (pos)) {
          GST_DEBUG_OBJECT (dec, "Got segment update, advancing time from %"
              GST_TIME_FORMAT " to %" GST_TIME_FORMAT,
              GST_TIME_ARGS (dec->next_ts), GST_TIME_ARGS (pos));

          gst_dvd_sub_dec_advance_time (dec, pos);
        } else {
          GST_WARNING_OBJECT (dec, "Got segment update with invalid position");
        }
        gst_event_unref (event);
        ret = TRUE;
      } else {
        /* not just an update ... */

        /* Turn off forced highlight display */
        // dec->forced_display = 0;
        // dec->current_button = 0;
        if (dec->partialbuf) {
          gst_buffer_unref (dec->partialbuf);
          dec->partialbuf = NULL;
          dec->have_title = FALSE;
        }

        if (GST_CLOCK_TIME_IS_VALID (pos))
          dec->next_ts = pos;
        else
          dec->next_ts = GST_CLOCK_TIME_NONE;

        GST_DEBUG_OBJECT (dec, "Got newsegment, new time = %"
            GST_TIME_FORMAT, GST_TIME_ARGS (dec->next_ts));

        ret = gst_pad_event_default (pad, event);
      }
      break;
    }
    case GST_EVENT_FLUSH_STOP:{
      /* Turn off forced highlight display */
      dec->forced_display = 0;
      dec->current_button = 0;

      if (dec->partialbuf) {
        gst_buffer_unref (dec->partialbuf);
        dec->partialbuf = NULL;
        dec->have_title = FALSE;
      }

      ret = gst_pad_event_default (pad, event);
      break;
    }
    default:{
      ret = gst_pad_event_default (pad, event);
      break;
    }
  }
  gst_object_unref (dec);
  return ret;
}

static gboolean
gst_dvd_sub_dec_handle_dvd_event (GstDvdSubDec * dec, GstEvent * event)
{
  GstStructure *structure;
  const gchar *event_name;

  structure = (GstStructure *) gst_event_get_structure (event);

  if (structure == NULL)
    goto not_handled;

  event_name = gst_structure_get_string (structure, "event");

  GST_LOG_OBJECT (dec, "DVD event %s with timestamp %lld on sub pad",
      GST_STR_NULL (event_name), GST_EVENT_TIMESTAMP (event));

  if (event_name == NULL)
    goto not_handled;

  if (strcmp (event_name, "dvd-spu-highlight") == 0) {
    gint button;
    gint palette, sx, sy, ex, ey;
    gint i;

    /* Details for the highlight region to display */
    if (!gst_structure_get_int (structure, "button", &button) ||
        !gst_structure_get_int (structure, "palette", &palette) ||
        !gst_structure_get_int (structure, "sx", &sx) ||
        !gst_structure_get_int (structure, "sy", &sy) ||
        !gst_structure_get_int (structure, "ex", &ex) ||
        !gst_structure_get_int (structure, "ey", &ey)) {
      GST_ERROR_OBJECT (dec, "Invalid dvd-spu-highlight event received");
      return TRUE;
    }
    dec->current_button = button;
    dec->hl_left = sx;
    dec->hl_top = sy;
    dec->hl_right = ex;
    dec->hl_bottom = ey;
    for (i = 0; i < 4; i++) {
      dec->menu_alpha[i] = ((guint32) (palette) >> (i * 4)) & 0x0f;
      dec->menu_index[i] = ((guint32) (palette) >> (16 + (i * 4))) & 0x0f;
    }

    GST_DEBUG_OBJECT (dec, "New button activated highlight=(%d,%d) to (%d,%d) "
        "palette 0x%x", sx, sy, ex, ey, palette);
    gst_setup_palette (dec);

    dec->buf_dirty = TRUE;
  } else if (strcmp (event_name, "dvd-spu-clut-change") == 0) {
    /* Take a copy of the colour table */
    gchar name[16];
    int i;
    gint value;

    GST_LOG_OBJECT (dec, "New colour table received");
    for (i = 0; i < 16; i++) {
      g_snprintf (name, sizeof (name), "clut%02d", i);
      if (!gst_structure_get_int (structure, name, &value)) {
        GST_ERROR_OBJECT (dec, "dvd-spu-clut-change event did not "
            "contain %s field", name);
        break;
      }
      dec->current_clut[i] = (guint32) (value);
    }

    gst_setup_palette (dec);

    dec->buf_dirty = TRUE;
  } else if (strcmp (event_name, "dvd-spu-stream-change") == 0
      || strcmp (event_name, "dvd-spu-reset-highlight") == 0) {
    /* Turn off forced highlight display */
    dec->current_button = 0;

    GST_LOG_OBJECT (dec, "Clearing button state");
    dec->buf_dirty = TRUE;
  } else if (strcmp (event_name, "dvd-spu-still-frame") == 0) {
    /* Handle a still frame */
    GST_LOG_OBJECT (dec, "Received still frame notification");
  } else {
    goto not_handled;
  }

  return TRUE;

not_handled:
  {
    /* Ignore all other unknown events */
    GST_LOG_OBJECT (dec, "Ignoring other custom event %" GST_PTR_FORMAT,
        structure);
    return FALSE;
  }
}

static gboolean
plugin_init (GstPlugin * plugin)
{
  if (!gst_element_register (plugin, "dvdsubdec", GST_RANK_NONE,
          GST_TYPE_DVD_SUB_DEC) ||
      !gst_element_register (plugin, "dvdsubparse", GST_RANK_NONE,
          GST_TYPE_DVD_SUB_PARSE)) {
    return FALSE;
  }

  GST_DEBUG_CATEGORY_INIT (gst_dvd_sub_dec_debug, "dvdsubdec", 0,
      "DVD subtitle decoder");

  return TRUE;
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    "dvdsub",
    "DVD subtitle parser and decoder", plugin_init,
    VERSION, GST_LICENSE, GST_PACKAGE_NAME, GST_PACKAGE_ORIGIN);
