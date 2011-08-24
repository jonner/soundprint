/* GStreamer
 * Copyright (C) <1999> Erik Walthinsen <omega@cse.ogi.edu>
 * Copyright (C) <2009> Sebastian Dröge <sebastian.droege@collabora.co.uk>
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


#ifndef __GST_SPECTROGRAM_H__
#define __GST_SPECTROGRAM_H__

#include <gst/gst.h>
#include <gst/base/gstadapter.h>
#include <gst/fft/gstfftf32.h>

G_BEGIN_DECLS

#define GST_TYPE_SPECTROGRAM            (gst_spectrogram_get_type())
#define GST_SPECTROGRAM(obj)            (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_SPECTROGRAM,GstSpectrogram))
#define GST_IS_SPECTROGRAM(obj)         (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_SPECTROGRAM))
#define GST_SPECTROGRAM_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_SPECTROGRAM,GstSpectrogramClass))
#define GST_IS_SPECTROGRAM_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE((klass), GST_TYPE_SPECTROGRAM))
typedef struct _GstSpectrogram GstSpectrogram;
typedef struct _GstSpectrogramClass GstSpectrogramClass;
typedef struct _GstSpectrumChannel GstSpectrumChannel;

typedef void (*GstSpectrumInputData)(const guint8 * in, gfloat * out,
    guint len, guint channels, gfloat max_value, guint op, guint nfft);

struct _GstSpectrumChannel
{
  gfloat *input;
  gfloat *input_tmp;
  GstFFTF32Complex *freqdata;
  gfloat *spect_magnitude;      /* accumulated mangitude and phase */
  gfloat *spect_phase;          /* will be scaled by num_fft before sending */
  GstFFTF32 *fft_ctx;
};

struct _GstSpectrogram
{
  GstElement parent;

  GstPad *sinkpad;
  GstPad *srcpad;

  /* input format */
  gint rate;
  gint format_channels;
  gint bps;

  /* output format */
  gint width;
  gint height;
  gint fps_n;
  gint fps_d;

  GQueue *spectrogram_data;

  /* properties */
  guint64 interval;             /* how many nanoseconds between fft samples */
  guint64 frames_per_interval;  /* how many frames per interval */
  guint64 frames_todo;
  guint bands;                  /* number of spectrum bands */
  gint threshold;               /* energy level treshold */
  gboolean multi_channel;       /* send separate channel results */

  gint video_count;

  guint64 num_frames;           /* frame count (1 sample per channel)
                                 * since last emit */
  guint64 num_fft;              /* number of FFTs since last emit */
  GstClockTime message_ts;      /* starttime for next message */

  /* <private> */
  GstSpectrumChannel *channel_data;
  guint num_channels;

  guint input_pos;
  guint64 error_per_interval;
  guint64 accumulated_error;

  GstSpectrumInputData input_data;
};

struct _GstSpectrogramClass
{
  GstElementClass parent_class;
};

GType gst_spectrogram_get_type (void);

G_END_DECLS

#endif /* __GST_SPECTROGRAM_H__ */
