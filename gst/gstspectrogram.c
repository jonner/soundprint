/* GStreamer
 * Copyright (C) <1999> Erik Walthinsen <omega@cse.ogi.edu>
 *               <2006,2011> Stefan Kost <ensonic@users.sf.net>
 *               <2007-2009> Sebastian Dröge <sebastian.droege@collabora.co.uk>
 *               <2011> Jonathon Jongsma <jonathon@quotidian.org>
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
/**
 * SECTION:element-spectrogram
 *
 * The Spectrogram element analyzes the frequency spectrum of an audio signal
 * and produces a visualization of that signal in the form of a 'spectrogram'
 *
 * If #GstSpectrogram:multi-channel property is set to true. magnitude and phase
 * fields will be each a nested #GstValueArray. The first dimension are the
 * channels and the second dimension are the values.
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include <math.h>
#include "gstspectrogram.h"
#include <gst/video/video.h>

GST_DEBUG_CATEGORY_STATIC (gst_spectrogram_debug);
#define GST_CAT_DEFAULT gst_spectrogram_debug

/* elementfactory information */

/* Spectrogram properties */
#define DEFAULT_BANDS			128
#define DEFAULT_THRESHOLD		-60
#define DEFAULT_MULTI_CHANNEL		FALSE
#define DEFAULT_WIDTH 320
#define DEFAULT_HEIGHT 240

enum
{
  PROP_0,
  PROP_BANDS,
  PROP_THRESHOLD,
  PROP_MULTI_CHANNEL
};

GST_BOILERPLATE (GstSpectrogram, gst_spectrogram, GstElement,
    GST_TYPE_ELEMENT);

static GstStaticPadTemplate src_template =
    GST_STATIC_PAD_TEMPLATE ("src",
                             GST_PAD_SRC,
                             GST_PAD_ALWAYS,
                             GST_STATIC_CAPS (GST_VIDEO_CAPS_RGBx));

static GstStaticPadTemplate sink_template =
    GST_STATIC_PAD_TEMPLATE ("sink",
                             GST_PAD_SINK,
                             GST_PAD_ALWAYS,
                             GST_STATIC_CAPS ("audio/x-raw-int, "
                                              "endianness = (int) BYTE_ORDER, "
                                              "signed = (boolean) TRUE, "
                                              "width = (int) 16, "
                                              "depth = (int) 16, "
                                              "rate = (int) [ 8000, 96000 ], "
                                              "channels = (int) { 1, 2 }"));

static void gst_spectrogram_finalize (GObject * object);
static void gst_spectrogram_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_spectrogram_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

static void
gst_spectrogram_base_init (gpointer g_class)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS (g_class);

  gst_element_class_set_details_simple (element_class, "Spectrogram",
      "Visualization",
      "Run an FFT on the audio signal, visualize spectrogram data",
      "Erik Walthinsen <omega@cse.ogi.edu>, "
      "Stefan Kost <ensonic@users.sf.net>, "
      "Sebastian Dröge <sebastian.droege@collabora.co.uk>, "
      "Jonathon Jongsma <jonathon@quotidian.org>");
}

static void
gst_spectrogram_class_init (GstSpectrogramClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->set_property = gst_spectrogram_set_property;
  gobject_class->get_property = gst_spectrogram_get_property;
  gobject_class->finalize = gst_spectrogram_finalize;

  g_object_class_install_property (gobject_class, PROP_BANDS,
      g_param_spec_uint ("bands", "Bands", "Number of frequency bands",
          0, G_MAXUINT, DEFAULT_BANDS,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_THRESHOLD,
      g_param_spec_int ("threshold", "Threshold",
          "dB threshold for result. All lower values will be set to this",
          G_MININT, 0, DEFAULT_THRESHOLD,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_MULTI_CHANNEL,
      g_param_spec_boolean ("multi-channel", "Multichannel results",
          "Send separate results for each channel",
          DEFAULT_MULTI_CHANNEL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  GST_DEBUG_CATEGORY_INIT (gst_spectrogram_debug, "spectrogram", 0,
      "spectrogram visualization element");
}

static gboolean
gst_spectrogram_setup (GstSpectrogram * self);

static gboolean
gst_spectrogram_sink_setcaps (GstPad *pad, GstCaps *caps)
{
  GstStructure *structure;
  gboolean res;
  GstSpectrogram *self = GST_SPECTROGRAM (GST_PAD_PARENT (pad));

  structure = gst_caps_get_structure (caps, 0);

  res = gst_structure_get_int (structure, "channels", &self->format_channels);
  res &= gst_structure_get_int (structure, "rate", &self->rate);
  self->bps = self->format_channels * sizeof (gint16);

  res &= gst_spectrogram_setup (self);

  return res;
}

static gboolean
gst_spectrogram_src_setcaps (GstPad *pad, GstCaps *caps)
{
  GstStructure *structure;
  gboolean res;
  GstSpectrogram *self = GST_SPECTROGRAM (GST_PAD_PARENT (pad));

  structure = gst_caps_get_structure (caps, 0);

  res = gst_structure_get_int (structure, "width", &self->width);
  res &= gst_structure_get_int (structure, "height", &self->height);
  res &= gst_structure_get_fraction (structure, "framerate", &self->fps_n,
                                &self->fps_d);
  gchar *caps_str = gst_caps_to_string (caps);
  g_debug ("Got src caps: %s", caps_str);
  g_free (caps_str);

  self->interval = gst_util_uint64_scale_int (GST_SECOND, self->fps_d,
                                              self->fps_n);

  return res;
}

static GstFlowReturn
gst_spectrogram_process_buffer (GstSpectrogram * self, GstBuffer * buffer);

static GstFlowReturn
gst_spectrogram_chain (GstPad *pad, GstBuffer *buffer)
{
  GstSpectrogram *self = GST_SPECTROGRAM (gst_pad_get_parent (pad));
  GstFlowReturn ret = GST_FLOW_OK;

  if (self->bps == 0) {
    ret = GST_FLOW_NOT_NEGOTIATED;
    goto evacuate;
  }

  if (GST_PAD_CAPS (self->srcpad) == NULL)
  {
    GST_DEBUG_OBJECT (self, "Trying to negotiate src pad");

    /* try to negotiate caps with peer */
    GstCaps *target;
    const GstCaps *tmpl = gst_pad_get_pad_template_caps (self->srcpad);
    GstCaps *peercaps = gst_pad_peer_get_caps (self->srcpad);

    if (peercaps) {
      target = gst_caps_intersect (peercaps, tmpl);
      gst_caps_unref (peercaps);

      if (gst_caps_is_empty (target)) {
        gst_caps_unref (target);
        ret = GST_FLOW_NOT_NEGOTIATED;
        goto evacuate;
      }

      gst_caps_truncate (target);
    } else {
      target = gst_caps_ref ((GstCaps*)tmpl);
    }

    GstStructure *structure = gst_caps_get_structure (target, 0);
    gst_structure_fixate_field_nearest_int (structure, "width", DEFAULT_WIDTH);
    gst_structure_fixate_field_nearest_int (structure, "height", DEFAULT_HEIGHT);
    gst_structure_fixate_field_nearest_fraction (structure, "framerate", 25, 1);

    gst_pad_set_caps (self->srcpad, target);
    gst_caps_unref (target);
  }

  ret = gst_spectrogram_process_buffer (self, buffer);

evacuate:
  gst_buffer_unref (buffer);
  return ret;
}

static gboolean
gst_spectrogram_sink_event (GstPad *pad, GstEvent *event)
{
  /* FIXME: implement */
  return TRUE;
}

static gboolean
gst_spectrogram_src_event (GstPad *pad, GstEvent *event)
{
  /* FIXME: implement */
  return TRUE;
}

static void
gst_spectrogram_init (GstSpectrogram *self, GstSpectrogramClass * g_class)
{
  self->bands = DEFAULT_BANDS;
  self->threshold = DEFAULT_THRESHOLD;

  self->sinkpad = gst_pad_new_from_static_template (&sink_template, "sink");
  gst_pad_set_chain_function (self->sinkpad,
                              GST_DEBUG_FUNCPTR (gst_spectrogram_chain));
  gst_pad_set_event_function (self->sinkpad,
                              GST_DEBUG_FUNCPTR (gst_spectrogram_sink_event));
  gst_pad_set_setcaps_function (self->sinkpad,
                                GST_DEBUG_FUNCPTR (gst_spectrogram_sink_setcaps));
  gst_element_add_pad (GST_ELEMENT (self), self->sinkpad);

  self->srcpad = gst_pad_new_from_static_template (&src_template, "src");
  gst_pad_set_event_function (self->srcpad,
                              GST_DEBUG_FUNCPTR (gst_spectrogram_src_event));
  gst_pad_set_setcaps_function (self->srcpad,
                                GST_DEBUG_FUNCPTR (gst_spectrogram_src_setcaps));
  /* FIXME: add latency to query ?? */
  /*gst_pad_set_query_function (self->srcpad,
                              GST_DEBUG_FUNCPTR (gst_spectrogram_src_query)); */
  gst_element_add_pad (GST_ELEMENT (self), self->srcpad);

  self->spectrogram_data = g_queue_new ();
}

static void
gst_spectrogram_alloc_channel_data (GstSpectrogram * self)
{
  gint i;
  GstSpectrumChannel *cd;
  guint bands = self->bands;
  guint nfft = 2 * bands - 2;

  g_assert (self->channel_data == NULL);

  self->num_channels = (self->multi_channel) ?
    self->format_channels : 1;

  GST_DEBUG_OBJECT (self, "allocating data for %d channels",
      self->num_channels);

  self->channel_data = g_new (GstSpectrumChannel, self->num_channels);
  for (i = 0; i < self->num_channels; i++) {
    cd = &self->channel_data[i];
    cd->fft_ctx = gst_fft_f32_new (nfft, FALSE);
    cd->input = g_new0 (gfloat, nfft);
    cd->input_tmp = g_new0 (gfloat, nfft);
    cd->freqdata = g_new0 (GstFFTF32Complex, bands);
    cd->spect_magnitude = g_new0 (gfloat, bands);
    cd->spect_phase = g_new0 (gfloat, bands);
  }
}

static void
gst_spectrogram_free_channel_data (GstSpectrogram * self)
{
  if (self->channel_data) {
    gint i;
    GstSpectrumChannel *cd;

    GST_DEBUG_OBJECT (self, "freeing data for %d channels",
        self->num_channels);

    for (i = 0; i < self->num_channels; i++) {
      cd = &self->channel_data[i];
      if (cd->fft_ctx)
        gst_fft_f32_free (cd->fft_ctx);
      g_free (cd->input);
      g_free (cd->input_tmp);
      g_free (cd->freqdata);
      g_free (cd->spect_magnitude);
      g_free (cd->spect_phase);
    }
    g_free (self->channel_data);
    self->channel_data = NULL;
  }
}

static void
gst_spectrogram_flush (GstSpectrogram * self)
{
  self->num_frames = 0;
  self->num_fft = 0;

  self->accumulated_error = 0;
}

static void
gst_spectrogram_reset_state (GstSpectrogram * self)
{
  GST_DEBUG_OBJECT (self, "resetting state");

  gst_spectrogram_free_channel_data (self);
  gst_spectrogram_flush (self);
}

static void
gst_spectrogram_finalize (GObject * object)
{
  GstSpectrogram *self = GST_SPECTROGRAM (object);

  gst_spectrogram_reset_state (self);
  g_queue_free (self->spectrogram_data);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_spectrogram_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstSpectrogram *filter = GST_SPECTROGRAM (object);

  switch (prop_id) {
    case PROP_BANDS:{
      guint bands = g_value_get_uint (value);
      if (filter->bands != bands) {
        filter->bands = bands;
        gst_spectrogram_reset_state (filter);
      }
    }
      break;
    case PROP_THRESHOLD:
      filter->threshold = g_value_get_int (value);
      break;
    case PROP_MULTI_CHANNEL:{
      gboolean multi_channel = g_value_get_boolean (value);
      if (filter->multi_channel != multi_channel) {
        filter->multi_channel = multi_channel;
        gst_spectrogram_reset_state (filter);
      }
    }
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_spectrogram_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstSpectrogram *filter = GST_SPECTROGRAM (object);

  switch (prop_id) {
    case PROP_BANDS:
      g_value_set_uint (value, filter->bands);
      break;
    case PROP_THRESHOLD:
      g_value_set_int (value, filter->threshold);
      break;
    case PROP_MULTI_CHANNEL:
      g_value_set_boolean (value, filter->multi_channel);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

/* mixing data readers */

static void
input_data_mixed_int16_max (const guint8 * _in, gfloat * out, guint len,
    guint channels, gfloat max_value, guint op, guint nfft)
{
  guint i, j, ip = 0;
  gint16 *in = (gint16 *) _in;
  gfloat v;

  for (j = 0; j < len; j++) {
    v = in[ip++] / max_value;
    for (i = 1; i < channels; i++)
      v += in[ip++] / max_value;
    out[op] = v / channels;
    op = (op + 1) % nfft;
  }
}

/* non mixing data readers */

static void
input_data_int16_max (const guint8 * _in, gfloat * out, guint len,
    guint channels, gfloat max_value, guint op, guint nfft)
{
  guint j, ip;
  gint16 *in = (gint16 *) _in;

  for (j = 0, ip = 0; j < len; j++, ip += channels) {
    out[op] = in[ip] / max_value;
    op = (op + 1) % nfft;
  }
}

static gboolean
gst_spectrogram_setup (GstSpectrogram * self)
{
  self->input_data =
    self->multi_channel ? input_data_int16_max : input_data_mixed_int16_max;

  gst_spectrogram_reset_state (self);
  return TRUE;
}

static void
gst_spectrogram_run_fft (GstSpectrogram * self, GstSpectrumChannel * cd,
    guint input_pos)
{
  guint i;
  guint bands = self->bands;
  guint nfft = 2 * bands - 2;
  gint threshold = self->threshold;
  gfloat *input = cd->input;
  gfloat *input_tmp = cd->input_tmp;
  gfloat *spect_magnitude = cd->spect_magnitude;
  GstFFTF32Complex *freqdata = cd->freqdata;
  GstFFTF32 *fft_ctx = cd->fft_ctx;

  for (i = 0; i < nfft; i++)
    input_tmp[i] = input[(input_pos + i) % nfft];

  gst_fft_f32_window (fft_ctx, input_tmp, GST_FFT_WINDOW_HAMMING);

  gst_fft_f32_fft (fft_ctx, input_tmp, freqdata);

  gdouble val;
  /* Calculate magnitude in db */
  for (i = 0; i < bands; i++) {
    val = freqdata[i].r * freqdata[i].r;
    val += freqdata[i].i * freqdata[i].i;
    val /= nfft * nfft;
    val = 10.0 * log10 (val);
    if (val < threshold)
      val = threshold;
    spect_magnitude[i] += val;
  }
}

static void
gst_spectrogram_reset_message_data (GstSpectrogram * self,
    GstSpectrumChannel * cd)
{
  guint bands = self->bands;
  gfloat *spect_magnitude = cd->spect_magnitude;
  gfloat *spect_phase = cd->spect_phase;

  /* reset spectrum accumulators */
  memset (spect_magnitude, 0, bands * sizeof (gfloat));
  memset (spect_phase, 0, bands * sizeof (gfloat));
}

static void
gst_spectrogram_push_spectrum_data (GstSpectrogram *self)
{
  guchar *slice = g_new0 (guchar, self->height * 4);
  int i;
  for (i = 0; i < self->height; i++) {
    guchar *d = slice + (i * 4);
    gint band = (double)i / (double)self->height * self->bands;
    gdouble level = (self->channel_data[0].spect_magnitude[band] -
                     self->threshold) / abs(self->threshold);
    d[0] = 0xff * level;
    d[1] = 0xff * level;
    d[2] = 0xff * level;
  }

  while (g_queue_get_length (self->spectrogram_data) >= self->width) {
    guchar *old = g_queue_pop_tail (self->spectrogram_data);
    g_free (old);
  }

  g_queue_push_head (self->spectrogram_data, slice);
}

static GstFlowReturn
gst_spectrogram_push_video_frame (GstSpectrogram *self)
{
  GstBuffer *buffer = 0;
  GstFlowReturn ret;
  int buffer_size = self->width * self->height * 4;
  ret = gst_pad_alloc_buffer_and_set_caps (self->srcpad,
                                           GST_BUFFER_OFFSET_NONE,
                                           buffer_size,
                                           GST_PAD_CAPS (self->srcpad),
                                           &buffer);

  if (ret != GST_FLOW_OK)
    return ret;

  static guchar count = 0x1;

  memset (buffer->data, 0x00, buffer_size);
  int slices = g_queue_get_length (self->spectrogram_data);
  int i;
  for (i = 0; i < self->height; i++) {
    int j;
    for (j = 0; j < slices; j++) {
      guchar *src = g_queue_peek_nth (self->spectrogram_data, j) + (i * 4);
      guchar *dest = buffer->data + (self->width - (j + 1)) * 4 + (self->width *
                                                                   4 * i);
      dest[0] = src[0];
      dest[1] = src[1];
      dest[2] = src[2];
    }
  }
  count++;

  ret = gst_pad_push (self->srcpad, buffer);

  return GST_FLOW_OK;
}

static GstFlowReturn
gst_spectrogram_process_buffer (GstSpectrogram * self, GstBuffer * buffer)
{
  GstFlowReturn ret = GST_FLOW_OK;
  guint rate = self->rate;
  guint channels = self->format_channels;
  guint output_channels = self->multi_channel ? channels : 1;
  guint c;
  guint width = 16 / 8;
  gfloat max_value = (1UL << (16 - 1)) - 1;
  guint bands = self->bands;
  guint nfft = 2 * bands - 2;
  guint input_pos;
  gfloat *input;
  const guint8 *data = GST_BUFFER_DATA (buffer);
  guint size = GST_BUFFER_SIZE (buffer);
  guint frame_size = width * channels;
  guint fft_todo, msg_todo, block_size;
  gboolean have_full_interval;
  GstSpectrumChannel *cd;
  GstSpectrumInputData input_data;

  GST_LOG_OBJECT (self, "input size: %d bytes", GST_BUFFER_SIZE (buffer));

  if (GST_BUFFER_IS_DISCONT (buffer)) {
    GST_DEBUG_OBJECT (self, "Discontinuity detected -- flushing");
    gst_spectrogram_flush (self);
  }

  /* If we don't have a FFT context yet (or it was reset due to parameter
   * changes) get one and allocate memory for everything
   */
  if (self->channel_data == NULL) {
    GST_DEBUG_OBJECT (self, "allocating for bands %u", bands);

    gst_spectrogram_alloc_channel_data (self);

    /* number of sample frames we process before posting a message
     * interval is in ns */
    self->frames_per_interval =
        gst_util_uint64_scale (self->interval, rate, GST_SECOND);
    self->frames_todo = self->frames_per_interval;
    /* rounding error for frames_per_interval in ns,
     * aggregated it in accumulated_error */
    self->error_per_interval = (self->interval * rate) % GST_SECOND;
    if (self->frames_per_interval == 0)
      self->frames_per_interval = 1;

    GST_INFO_OBJECT (self, "interval %" GST_TIME_FORMAT ", fpi %"
        G_GUINT64_FORMAT ", error %" GST_TIME_FORMAT,
        GST_TIME_ARGS (self->interval), self->frames_per_interval,
        GST_TIME_ARGS (self->error_per_interval));

    self->input_pos = 0;

    gst_spectrogram_flush (self);
  }

  if (self->num_frames == 0)
    self->message_ts = GST_BUFFER_TIMESTAMP (buffer);

  input_pos = self->input_pos;
  input_data = self->input_data;

  while (size >= frame_size) {
    /* run input_data for a chunk of data */
    fft_todo = nfft - (self->num_frames % nfft);
    msg_todo = self->frames_todo - self->num_frames;
    GST_LOG_OBJECT (self,
        "message frames todo: %u, fft frames todo: %u, input frames %u",
        msg_todo, fft_todo, (size / frame_size));
    block_size = msg_todo;
    if (block_size > (size / frame_size))
      block_size = (size / frame_size);
    if (block_size > fft_todo)
      block_size = fft_todo;

    for (c = 0; c < output_channels; c++) {
      cd = &self->channel_data[c];
      input = cd->input;
      /* Move the current frames into our ringbuffers */
      input_data (data + c * width, input, block_size, channels, max_value,
          input_pos, nfft);
    }
    data += block_size * frame_size;
    size -= block_size * frame_size;
    input_pos = (input_pos + block_size) % nfft;
    self->num_frames += block_size;

    have_full_interval = (self->num_frames == self->frames_todo);

    GST_LOG_OBJECT (self, "size: %u, do-fft = %d, do-message = %d", size,
        (self->num_frames % nfft == 0), have_full_interval);

    /* If we have enough frames for an FFT or we have all frames required for
     * the interval and we haven't run a FFT, then run an FFT */
    if ((self->num_frames % nfft == 0) ||
        (have_full_interval && !self->num_fft)) {
      for (c = 0; c < output_channels; c++) {
        cd = &self->channel_data[c];
        gst_spectrogram_run_fft (self, cd, input_pos);
      }
      self->num_fft++;
    }

    /* Do we have the FFTs for one interval? */
    if (have_full_interval) {
      GST_DEBUG_OBJECT (self, "nfft: %u frames: %" G_GUINT64_FORMAT
          " fpi: %" G_GUINT64_FORMAT " error: %" GST_TIME_FORMAT, nfft,
          self->num_frames, self->frames_per_interval,
          GST_TIME_ARGS (self->accumulated_error));

      self->frames_todo = self->frames_per_interval;
      if (self->accumulated_error >= GST_SECOND) {
        self->accumulated_error -= GST_SECOND;
        self->frames_todo++;
      }
      self->accumulated_error += self->error_per_interval;

      gst_spectrogram_push_spectrum_data (self);
      ret = gst_spectrogram_push_video_frame (self);

      if (GST_CLOCK_TIME_IS_VALID (self->message_ts))
        self->message_ts +=
            gst_util_uint64_scale (self->num_frames, GST_SECOND, rate);

      for (c = 0; c < channels; c++) {
        cd = &self->channel_data[c];
        gst_spectrogram_reset_message_data (self, cd);
      }
      self->num_frames = 0;
      self->num_fft = 0;
    }
  }

  self->input_pos = input_pos;

  g_assert (size == 0);

  return ret;
}

static gboolean
plugin_init (GstPlugin * plugin)
{
  return gst_element_register (plugin, "spectrogram", GST_RANK_NONE,
      GST_TYPE_SPECTROGRAM);
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    "spectrogram",
    "Run an FFT on the audio signal, output spectrum data",
    plugin_init, PACKAGE_VERSION, "GPL", PACKAGE_NAME, "http://www.gnome.org/~jjongsma/");
