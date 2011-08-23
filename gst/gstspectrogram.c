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
 * SECTION:element-spectrum
 *
 * The Spectrogram element analyzes the frequency spectrum of an audio signal
 * and produces a visualization of that signal in the form of a 'spectrogram'
 *
 * If #GstSpectrum:multi-channel property is set to true. magnitude and phase
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

GST_DEBUG_CATEGORY_STATIC (gst_spectrum_debug);
#define GST_CAT_DEFAULT gst_spectrum_debug

/* elementfactory information */

#define ALLOWED_CAPS \
    "audio/x-raw-int, "                                               \
    " width = (int) 16, "                                             \
    " depth = (int) [ 1, 16 ], "                                      \
    " signed = (boolean) true, "                                      \
    " endianness = (int) BYTE_ORDER, "                                \
    " rate = (int) [ 1, MAX ], "                                      \
    " channels = (int) [ 1, MAX ]; "                                  \
    "audio/x-raw-int, "                                               \
    " width = (int) 24, "                                             \
    " depth = (int) [ 1, 24 ], "                                      \
    " signed = (boolean) true, "                                      \
    " endianness = (int) BYTE_ORDER, "                                \
    " rate = (int) [ 1, MAX ], "                                      \
    " channels = (int) [ 1, MAX ]; "                                  \
    "audio/x-raw-int, "                                               \
    " width = (int) 32, "                                             \
    " depth = (int) [ 1, 32 ], "                                      \
    " signed = (boolean) true, "                                      \
    " endianness = (int) BYTE_ORDER, "                                \
    " rate = (int) [ 1, MAX ], "                                      \
    " channels = (int) [ 1, MAX ]; "                                  \
    "audio/x-raw-float, "                                             \
    " width = (int) { 32, 64 }, "                                     \
    " endianness = (int) BYTE_ORDER, "                                \
    " rate = (int) [ 1, MAX ], "                                      \
    " channels = (int) [ 1, MAX ]"

/* Spectrum properties */
#define DEFAULT_INTERVAL		(GST_SECOND / 10)
#define DEFAULT_BANDS			128
#define DEFAULT_THRESHOLD		-60
#define DEFAULT_MULTI_CHANNEL		FALSE

enum
{
  PROP_0,
  PROP_INTERVAL,
  PROP_BANDS,
  PROP_THRESHOLD,
  PROP_MULTI_CHANNEL
};

GST_BOILERPLATE (GstSpectrum, gst_spectrum, GstAudioFilter,
    GST_TYPE_AUDIO_FILTER);

static void gst_spectrum_finalize (GObject * object);
static void gst_spectrum_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_spectrum_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);
static gboolean gst_spectrum_start (GstBaseTransform * trans);
static gboolean gst_spectrum_stop (GstBaseTransform * trans);
static GstFlowReturn gst_spectrum_transform_ip (GstBaseTransform * trans,
    GstBuffer * in);
static gboolean gst_spectrum_setup (GstAudioFilter * base,
    GstRingBufferSpec * format);

static void
gst_spectrum_base_init (gpointer g_class)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS (g_class);
  GstCaps *caps;

  gst_element_class_set_details_simple (element_class, "Spectrogram",
      "Visualization",
      "Run an FFT on the audio signal, visualize spectrogram data",
      "Erik Walthinsen <omega@cse.ogi.edu>, "
      "Stefan Kost <ensonic@users.sf.net>, "
      "Sebastian Dröge <sebastian.droege@collabora.co.uk>, "
      "Jonathon Jongsma <jonathon@quotidian.org>");

  caps = gst_caps_from_string (ALLOWED_CAPS);
  gst_audio_filter_class_add_pad_templates (GST_AUDIO_FILTER_CLASS (g_class),
      caps);
  gst_caps_unref (caps);
}

static void
gst_spectrum_class_init (GstSpectrumClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstBaseTransformClass *trans_class = GST_BASE_TRANSFORM_CLASS (klass);
  GstAudioFilterClass *filter_class = GST_AUDIO_FILTER_CLASS (klass);

  gobject_class->set_property = gst_spectrum_set_property;
  gobject_class->get_property = gst_spectrum_get_property;
  gobject_class->finalize = gst_spectrum_finalize;

  trans_class->start = GST_DEBUG_FUNCPTR (gst_spectrum_start);
  trans_class->stop = GST_DEBUG_FUNCPTR (gst_spectrum_stop);
  trans_class->transform_ip = GST_DEBUG_FUNCPTR (gst_spectrum_transform_ip);
  trans_class->passthrough_on_same_caps = TRUE;

  filter_class->setup = GST_DEBUG_FUNCPTR (gst_spectrum_setup);

  g_object_class_install_property (gobject_class, PROP_INTERVAL,
      g_param_spec_uint64 ("interval", "Interval",
          "Interval of time between message posts (in nanoseconds)",
          1, G_MAXUINT64, DEFAULT_INTERVAL,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_BANDS,
      g_param_spec_uint ("bands", "Bands", "Number of frequency bands",
          0, G_MAXUINT, DEFAULT_BANDS,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_THRESHOLD,
      g_param_spec_int ("threshold", "Threshold",
          "dB threshold for result. All lower values will be set to this",
          G_MININT, 0, DEFAULT_THRESHOLD,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
   * GstSpectrum:multi-channel
   *
   * Send separate results for each channel
   *
   * Since: 0.10.29
   */
  g_object_class_install_property (gobject_class, PROP_MULTI_CHANNEL,
      g_param_spec_boolean ("multi-channel", "Multichannel results",
          "Send separate results for each channel",
          DEFAULT_MULTI_CHANNEL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  GST_DEBUG_CATEGORY_INIT (gst_spectrum_debug, "spectrogram", 0,
      "audio spectrum analyser element");
}

static void
gst_spectrum_init (GstSpectrum * spectrum, GstSpectrumClass * g_class)
{
  spectrum->interval = DEFAULT_INTERVAL;
  spectrum->bands = DEFAULT_BANDS;
  spectrum->threshold = DEFAULT_THRESHOLD;
}

static void
gst_spectrum_alloc_channel_data (GstSpectrum * spectrum)
{
  gint i;
  GstSpectrumChannel *cd;
  guint bands = spectrum->bands;
  guint nfft = 2 * bands - 2;

  g_assert (spectrum->channel_data == NULL);

  spectrum->num_channels = (spectrum->multi_channel) ?
      GST_AUDIO_FILTER (spectrum)->format.channels : 1;

  GST_DEBUG_OBJECT (spectrum, "allocating data for %d channels",
      spectrum->num_channels);

  spectrum->channel_data = g_new (GstSpectrumChannel, spectrum->num_channels);
  for (i = 0; i < spectrum->num_channels; i++) {
    cd = &spectrum->channel_data[i];
    //cd->fft_ctx = gst_fft_f32_new (nfft, FALSE);
    cd->input = g_new0 (gfloat, nfft);
    cd->input_tmp = g_new0 (gfloat, nfft);
    //cd->freqdata = g_new0 (GstFFTF32Complex, bands);
    cd->spect_magnitude = g_new0 (gfloat, bands);
    cd->spect_phase = g_new0 (gfloat, bands);
  }
}

static void
gst_spectrum_free_channel_data (GstSpectrum * spectrum)
{
  if (spectrum->channel_data) {
    gint i;
    GstSpectrumChannel *cd;

    GST_DEBUG_OBJECT (spectrum, "freeing data for %d channels",
        spectrum->num_channels);

    for (i = 0; i < spectrum->num_channels; i++) {
      cd = &spectrum->channel_data[i];
      //if (cd->fft_ctx)
        //gst_fft_f32_free (cd->fft_ctx);
      g_free (cd->input);
      g_free (cd->input_tmp);
      //g_free (cd->freqdata);
      g_free (cd->spect_magnitude);
      g_free (cd->spect_phase);
    }
    g_free (spectrum->channel_data);
    spectrum->channel_data = NULL;
  }
}

static void
gst_spectrum_flush (GstSpectrum * spectrum)
{
  spectrum->num_frames = 0;
  spectrum->num_fft = 0;

  spectrum->accumulated_error = 0;
}

static void
gst_spectrum_reset_state (GstSpectrum * spectrum)
{
  GST_DEBUG_OBJECT (spectrum, "resetting state");

  gst_spectrum_free_channel_data (spectrum);
  gst_spectrum_flush (spectrum);
}

static void
gst_spectrum_finalize (GObject * object)
{
  GstSpectrum *spectrum = GST_SPECTRUM (object);

  gst_spectrum_reset_state (spectrum);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_spectrum_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstSpectrum *filter = GST_SPECTRUM (object);

  switch (prop_id) {
    case PROP_INTERVAL:{
      guint64 interval = g_value_get_uint64 (value);
      if (filter->interval != interval) {
        GST_BASE_TRANSFORM_LOCK (filter);
        filter->interval = interval;
        gst_spectrum_reset_state (filter);
        GST_BASE_TRANSFORM_UNLOCK (filter);
      }
    }
      break;
    case PROP_BANDS:{
      guint bands = g_value_get_uint (value);
      if (filter->bands != bands) {
        GST_BASE_TRANSFORM_LOCK (filter);
        filter->bands = bands;
        gst_spectrum_reset_state (filter);
        GST_BASE_TRANSFORM_UNLOCK (filter);
      }
    }
      break;
    case PROP_THRESHOLD:
      filter->threshold = g_value_get_int (value);
      break;
    case PROP_MULTI_CHANNEL:{
      gboolean multi_channel = g_value_get_boolean (value);
      if (filter->multi_channel != multi_channel) {
        GST_BASE_TRANSFORM_LOCK (filter);
        filter->multi_channel = multi_channel;
        gst_spectrum_reset_state (filter);
        GST_BASE_TRANSFORM_UNLOCK (filter);
      }
    }
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_spectrum_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstSpectrum *filter = GST_SPECTRUM (object);

  switch (prop_id) {
    case PROP_INTERVAL:
      g_value_set_uint64 (value, filter->interval);
      break;
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

static gboolean
gst_spectrum_start (GstBaseTransform * trans)
{
  GstSpectrum *spectrum = GST_SPECTRUM (trans);

  gst_spectrum_reset_state (spectrum);

  return TRUE;
}

static gboolean
gst_spectrum_stop (GstBaseTransform * trans)
{
  GstSpectrum *spectrum = GST_SPECTRUM (trans);

  gst_spectrum_reset_state (spectrum);

  return TRUE;
}

/* mixing data readers */

static void
input_data_mixed_float (const guint8 * _in, gfloat * out, guint len,
    guint channels, gfloat max_value, guint op, guint nfft)
{
  guint i, j, ip = 0;
  gfloat v;
  gfloat *in = (gfloat *) _in;

  for (j = 0; j < len; j++) {
    v = in[ip++];
    for (i = 1; i < channels; i++)
      v += in[ip++];
    out[op] = v / channels;
    op = (op + 1) % nfft;
  }
}

static void
input_data_mixed_double (const guint8 * _in, gfloat * out, guint len,
    guint channels, gfloat max_value, guint op, guint nfft)
{
  guint i, j, ip = 0;
  gfloat v;
  gdouble *in = (gdouble *) _in;

  for (j = 0; j < len; j++) {
    v = in[ip++];
    for (i = 1; i < channels; i++)
      v += in[ip++];
    out[op] = v / channels;
    op = (op + 1) % nfft;
  }
}

static void
input_data_mixed_int32 (const guint8 * _in, gfloat * out, guint len,
    guint channels, gfloat max_value, guint op, guint nfft)
{
  guint i, j, ip = 0;
  gint32 *in = (gint32 *) _in;
  gfloat v;

  for (j = 0; j < len; j++) {
    v = in[ip++] * 2 + 1;
    for (i = 1; i < channels; i++)
      v += in[ip++] * 2 + 1;
    out[op] = v / channels;
    op = (op + 1) % nfft;
  }
}

static void
input_data_mixed_int32_max (const guint8 * _in, gfloat * out, guint len,
    guint channels, gfloat max_value, guint op, guint nfft)
{
  guint i, j, ip = 0;
  gint32 *in = (gint32 *) _in;
  gfloat v;

  for (j = 0; j < len; j++) {
    v = in[ip++] / max_value;
    for (i = 1; i < channels; i++)
      v += in[ip++] / max_value;
    out[op] = v / channels;
    op = (op + 1) % nfft;
  }
}

static void
input_data_mixed_int24 (const guint8 * _in, gfloat * out, guint len,
    guint channels, gfloat max_value, guint op, guint nfft)
{
  guint i, j;
  gfloat v = 0.0;

  for (j = 0; j < len; j++) {
    for (i = 0; i < channels; i++) {
#if G_BYTE_ORDER == G_BIG_ENDIAN
      gint32 value = GST_READ_UINT24_BE (_in);
#else
      gint32 value = GST_READ_UINT24_LE (_in);
#endif
      if (value & 0x00800000)
        value |= 0xff000000;
      v += value * 2 + 1;
      _in += 3;
    }
    out[op] = v / channels;
    op = (op + 1) % nfft;
  }
}

static void
input_data_mixed_int24_max (const guint8 * _in, gfloat * out, guint len,
    guint channels, gfloat max_value, guint op, guint nfft)
{
  guint i, j;
  gfloat v = 0.0;

  for (j = 0; j < len; j++) {
    for (i = 0; i < channels; i++) {
#if G_BYTE_ORDER == G_BIG_ENDIAN
      gint32 value = GST_READ_UINT24_BE (_in);
#else
      gint32 value = GST_READ_UINT24_LE (_in);
#endif
      if (value & 0x00800000)
        value |= 0xff000000;
      v += value / max_value;
      _in += 3;
    }
    out[op] = v / channels;
    op = (op + 1) % nfft;
  }
}

static void
input_data_mixed_int16 (const guint8 * _in, gfloat * out, guint len,
    guint channels, gfloat max_value, guint op, guint nfft)
{
  guint i, j, ip = 0;
  gint16 *in = (gint16 *) _in;
  gfloat v;

  for (j = 0; j < len; j++) {
    v = in[ip++] * 2 + 1;
    for (i = 1; i < channels; i++)
      v += in[ip++] * 2 + 1;
    out[op] = v / channels;
    op = (op + 1) % nfft;
  }
}

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
input_data_float (const guint8 * _in, gfloat * out, guint len, guint channels,
    gfloat max_value, guint op, guint nfft)
{
  guint j, ip;
  gfloat *in = (gfloat *) _in;

  for (j = 0, ip = 0; j < len; j++, ip += channels) {
    out[op] = in[ip];
    op = (op + 1) % nfft;
  }
}

static void
input_data_double (const guint8 * _in, gfloat * out, guint len, guint channels,
    gfloat max_value, guint op, guint nfft)
{
  guint j, ip;
  gdouble *in = (gdouble *) _in;

  for (j = 0, ip = 0; j < len; j++, ip += channels) {
    out[op] = in[ip];
    op = (op + 1) % nfft;
  }
}

static void
input_data_int32 (const guint8 * _in, gfloat * out, guint len, guint channels,
    gfloat max_value, guint op, guint nfft)
{
  guint j, ip;
  gint32 *in = (gint32 *) _in;

  for (j = 0, ip = 0; j < len; j++, ip += channels) {
    out[op] = in[ip] * 2 + 1;
    op = (op + 1) % nfft;
  }
}

static void
input_data_int32_max (const guint8 * _in, gfloat * out, guint len,
    guint channels, gfloat max_value, guint op, guint nfft)
{
  guint j, ip;
  gint32 *in = (gint32 *) _in;

  for (j = 0, ip = 0; j < len; j++, ip += channels) {
    out[op] = in[ip] / max_value;
    op = (op + 1) % nfft;
  }
}

static void
input_data_int24 (const guint8 * _in, gfloat * out, guint len, guint channels,
    gfloat max_value, guint op, guint nfft)
{
  guint j;

  for (j = 0; j < len; j++) {
#if G_BYTE_ORDER == G_BIG_ENDIAN
    gint32 v = GST_READ_UINT24_BE (_in);
#else
    gint32 v = GST_READ_UINT24_LE (_in);
#endif
    if (v & 0x00800000)
      v |= 0xff000000;
    _in += 3 * channels;
    out[op] = v * 2 + 1;
    op = (op + 1) % nfft;
  }
}

static void
input_data_int24_max (const guint8 * _in, gfloat * out, guint len,
    guint channels, gfloat max_value, guint op, guint nfft)
{
  guint j;

  for (j = 0; j < len; j++) {
#if G_BYTE_ORDER == G_BIG_ENDIAN
    gint32 v = GST_READ_UINT24_BE (_in);
#else
    gint32 v = GST_READ_UINT24_LE (_in);
#endif
    if (v & 0x00800000)
      v |= 0xff000000;
    _in += 3 * channels;
    out[op] = v / max_value;
    op = (op + 1) % nfft;
  }
}

static void
input_data_int16 (const guint8 * _in, gfloat * out, guint len, guint channels,
    gfloat max_value, guint op, guint nfft)
{
  guint j, ip;
  gint16 *in = (gint16 *) _in;

  for (j = 0, ip = 0; j < len; j++, ip += channels) {
    out[op] = in[ip] * 2 + 1;
    op = (op + 1) % nfft;
  }
}

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
gst_spectrum_setup (GstAudioFilter * base, GstRingBufferSpec * format)
{
  GstSpectrum *spectrum = GST_SPECTRUM (base);
  guint width = format->width / 8;
  gboolean is_float = (format->type == GST_BUFTYPE_FLOAT);
  /* max_value will be 0 when depth is 1,
   * interpret -1 and 0 as -1 and +1 if that's the case. */
  guint max_value = (1UL << (format->depth - 1)) - 1;
  gboolean multi_channel = spectrum->multi_channel;
  GstSpectrumInputData input_data = NULL;

  if (is_float) {
    if (width == 4) {
      input_data = multi_channel ? input_data_float : input_data_mixed_float;
    } else if (width == 8) {
      input_data = multi_channel ? input_data_double : input_data_mixed_double;
    } else {
      g_assert_not_reached ();
    }
  } else {
    if (width == 4) {
      if (max_value) {
        input_data =
            multi_channel ? input_data_int32_max : input_data_mixed_int32_max;
      } else {
        input_data = multi_channel ? input_data_int32 : input_data_mixed_int32;
      }
    } else if (width == 3) {
      if (max_value) {
        input_data =
            multi_channel ? input_data_int24_max : input_data_mixed_int24_max;
      } else {
        input_data = multi_channel ? input_data_int24 : input_data_mixed_int24;
      }
    } else if (width == 2) {
      if (max_value) {
        input_data =
            multi_channel ? input_data_int16_max : input_data_mixed_int16_max;
      } else {
        input_data = multi_channel ? input_data_int16 : input_data_mixed_int16;
      }
    } else {
      g_assert_not_reached ();
    }
  }

  spectrum->input_data = input_data;
  gst_spectrum_reset_state (spectrum);
  return TRUE;
}

static void
gst_spectrum_run_fft (GstSpectrum * spectrum, GstSpectrumChannel * cd,
    guint input_pos)
{
  guint i;
  guint bands = spectrum->bands;
  guint nfft = 2 * bands - 2;
  gfloat *input = cd->input;
  gfloat *input_tmp = cd->input_tmp;
  GstFFTF32Complex *freqdata = cd->freqdata;
  GstFFTF32 *fft_ctx = cd->fft_ctx;

  for (i = 0; i < nfft; i++)
    input_tmp[i] = input[(input_pos + i) % nfft];

  gst_fft_f32_window (fft_ctx, input_tmp, GST_FFT_WINDOW_HAMMING);

  gst_fft_f32_fft (fft_ctx, input_tmp, freqdata);
}

static void
gst_spectrum_reset_message_data (GstSpectrum * spectrum,
    GstSpectrumChannel * cd)
{
  guint bands = spectrum->bands;
  gfloat *spect_magnitude = cd->spect_magnitude;
  gfloat *spect_phase = cd->spect_phase;

  /* reset spectrum accumulators */
  memset (spect_magnitude, 0, bands * sizeof (gfloat));
  memset (spect_phase, 0, bands * sizeof (gfloat));
}

static GstFlowReturn
gst_spectrum_transform_ip (GstBaseTransform * trans, GstBuffer * buffer)
{
  GstSpectrum *spectrum = GST_SPECTRUM (trans);
  GstRingBufferSpec *format = &GST_AUDIO_FILTER (spectrum)->format;
  guint rate = format->rate;
  guint channels = format->channels;
  guint output_channels = spectrum->multi_channel ? channels : 1;
  guint c;
  guint width = format->width / 8;
  gfloat max_value = (1UL << (format->depth - 1)) - 1;
  guint bands = spectrum->bands;
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

  GST_LOG_OBJECT (spectrum, "input size: %d bytes", GST_BUFFER_SIZE (buffer));

  if (GST_BUFFER_IS_DISCONT (buffer)) {
    GST_DEBUG_OBJECT (spectrum, "Discontinuity detected -- flushing");
    gst_spectrum_flush (spectrum);
  }

  /* If we don't have a FFT context yet (or it was reset due to parameter
   * changes) get one and allocate memory for everything
   */
  if (spectrum->channel_data == NULL) {
    GST_DEBUG_OBJECT (spectrum, "allocating for bands %u", bands);

    gst_spectrum_alloc_channel_data (spectrum);

    /* number of sample frames we process before posting a message
     * interval is in ns */
    spectrum->frames_per_interval =
        gst_util_uint64_scale (spectrum->interval, rate, GST_SECOND);
    spectrum->frames_todo = spectrum->frames_per_interval;
    /* rounding error for frames_per_interval in ns,
     * aggregated it in accumulated_error */
    spectrum->error_per_interval = (spectrum->interval * rate) % GST_SECOND;
    if (spectrum->frames_per_interval == 0)
      spectrum->frames_per_interval = 1;

    GST_INFO_OBJECT (spectrum, "interval %" GST_TIME_FORMAT ", fpi %"
        G_GUINT64_FORMAT ", error %" GST_TIME_FORMAT,
        GST_TIME_ARGS (spectrum->interval), spectrum->frames_per_interval,
        GST_TIME_ARGS (spectrum->error_per_interval));

    spectrum->input_pos = 0;

    gst_spectrum_flush (spectrum);
  }

  if (spectrum->num_frames == 0)
    spectrum->message_ts = GST_BUFFER_TIMESTAMP (buffer);

  input_pos = spectrum->input_pos;
  input_data = spectrum->input_data;

  while (size >= frame_size) {
    /* run input_data for a chunk of data */
    fft_todo = nfft - (spectrum->num_frames % nfft);
    msg_todo = spectrum->frames_todo - spectrum->num_frames;
    GST_LOG_OBJECT (spectrum,
        "message frames todo: %u, fft frames todo: %u, input frames %u",
        msg_todo, fft_todo, (size / frame_size));
    block_size = msg_todo;
    if (block_size > (size / frame_size))
      block_size = (size / frame_size);
    if (block_size > fft_todo)
      block_size = fft_todo;

    for (c = 0; c < output_channels; c++) {
      cd = &spectrum->channel_data[c];
      input = cd->input;
      /* Move the current frames into our ringbuffers */
      input_data (data + c * width, input, block_size, channels, max_value,
          input_pos, nfft);
    }
    data += block_size * frame_size;
    size -= block_size * frame_size;
    input_pos = (input_pos + block_size) % nfft;
    spectrum->num_frames += block_size;

    have_full_interval = (spectrum->num_frames == spectrum->frames_todo);

    GST_LOG_OBJECT (spectrum, "size: %u, do-fft = %d, do-message = %d", size,
        (spectrum->num_frames % nfft == 0), have_full_interval);

    /* If we have enough frames for an FFT or we have all frames required for
     * the interval and we haven't run a FFT, then run an FFT */
    if ((spectrum->num_frames % nfft == 0) ||
        (have_full_interval && !spectrum->num_fft)) {
      for (c = 0; c < output_channels; c++) {
        cd = &spectrum->channel_data[c];
        gst_spectrum_run_fft (spectrum, cd, input_pos);
      }
      spectrum->num_fft++;
    }

    /* Do we have the FFTs for one interval? */
    if (have_full_interval) {
      GST_DEBUG_OBJECT (spectrum, "nfft: %u frames: %" G_GUINT64_FORMAT
          " fpi: %" G_GUINT64_FORMAT " error: %" GST_TIME_FORMAT, nfft,
          spectrum->num_frames, spectrum->frames_per_interval,
          GST_TIME_ARGS (spectrum->accumulated_error));

      spectrum->frames_todo = spectrum->frames_per_interval;
      if (spectrum->accumulated_error >= GST_SECOND) {
        spectrum->accumulated_error -= GST_SECOND;
        spectrum->frames_todo++;
      }
      spectrum->accumulated_error += spectrum->error_per_interval;

      if (GST_CLOCK_TIME_IS_VALID (spectrum->message_ts))
        spectrum->message_ts +=
            gst_util_uint64_scale (spectrum->num_frames, GST_SECOND, rate);

      for (c = 0; c < channels; c++) {
        cd = &spectrum->channel_data[c];
        gst_spectrum_reset_message_data (spectrum, cd);
      }
      spectrum->num_frames = 0;
      spectrum->num_fft = 0;
    }
  }

  spectrum->input_pos = input_pos;

  g_assert (size == 0);

  return GST_FLOW_OK;
}

static gboolean
plugin_init (GstPlugin * plugin)
{
  return gst_element_register (plugin, "spectrogram", GST_RANK_NONE,
      GST_TYPE_SPECTRUM);
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    "spectrogram",
    "Run an FFT on the audio signal, output spectrum data",
    plugin_init, PACKAGE_VERSION, "GPL", PACKAGE_NAME, "http://www.gnome.org/~jjongsma/")
