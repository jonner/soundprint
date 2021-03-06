/*******************************************************************************
 *
 *  Copyright (c) 2011 Jonathon Jongsma
 *
 *  This file is part of soundprint
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, see <http://www.gnu.org/licenses/>
 *
 *******************************************************************************/

#include <cairomm/cairomm.h>
#include <glibmm.h>
#include <gst/gst.h>

const double DEFAULT_THUMBNAIL_SIZE = 128.0;
const double DEFAULT_START_TIME = 0.0;
const double DEFAULT_SPECTROGRAM_LENGTH = 5.0;
const double DEFAULT_NOISE_THRESHOLD = -100.0;
const char * DEFAULT_OUTPUT_FILENAME = "thumbnail.png";

using Glib::ustring;

class OptionEntry : public Glib::OptionEntry
{
public:
    OptionEntry (const ustring &long_name = ustring(),
                 const ustring &description = ustring())
    {
        set_long_name (long_name);
        set_description (description);
    }
    OptionEntry (gchar short_name,
                 const ustring &long_name = ustring(),
                 const ustring &description = ustring())
    {
        set_short_name (short_name);
        set_long_name (long_name);
        set_description (description);
    }
};

class AppOptions : public Glib::OptionGroup
{
public:
    AppOptions (AppOptions&);
    AppOptions ()
        : Glib::OptionGroup ("application", "Application options")
          , m_size (DEFAULT_THUMBNAIL_SIZE)
          , m_length (DEFAULT_SPECTROGRAM_LENGTH)
          , m_threshold (DEFAULT_NOISE_THRESHOLD)
          , m_output_file (DEFAULT_OUTPUT_FILENAME)
          , m_start (DEFAULT_START_TIME)
          , m_benchmark (0)
    {
        add_entry (OptionEntry ('s', "size",
                                ustring::compose ("Size in pixels of the generated thumbnail (default %1px)",
                                                  DEFAULT_THUMBNAIL_SIZE)),
                   m_size);
        add_entry (OptionEntry ('l', "length",
                                ustring::compose ("Length (in seconds) of audio to use for thumbnail (default %1s)",
                                                  DEFAULT_SPECTROGRAM_LENGTH)),
                   m_length);
        add_entry (OptionEntry ('t', "threshold",
                                ustring::compose ("Noise threshold in dB (default -100)",
                                                  DEFAULT_NOISE_THRESHOLD)),
                   m_threshold);
        add_entry_filename (OptionEntry ('o', "output",
                                         ustring::compose ("file name for generated thumbnail (default '%1')",
                                                           DEFAULT_OUTPUT_FILENAME)),
                            m_output_file);
        add_entry (OptionEntry ("start",
                                ustring::compose ("Start time for the spectrogram (default %1s)",
                                                  DEFAULT_START_TIME)),
                   m_start);
        add_entry (OptionEntry ("benchmark",
                                "Run the specified number of times and report average time spent"),
                   m_benchmark);
    }

    double m_size;
    double m_length;
    double m_threshold;
    std::string m_output_file;
    double m_start;
    int m_benchmark;
};

class OptionContext : public Glib::OptionContext
{
public:
    OptionContext ()
        : Glib::OptionContext ("FILE_URI")
    {
        set_main_group (m_options);
        g_option_context_add_group (gobj (), gst_init_get_option_group ());
    }

    AppOptions m_options;
};

class App
{
public:
    App (const std::string & fileuri, AppOptions &options)
    : m_spectrogram_length (options.m_length)
    , m_start (options.m_start)
    , m_threshold (options.m_threshold)
    , m_thumbnail_size (options.m_size)
    , m_sample_width (m_thumbnail_size / m_num_samples)
    , m_sample_height (m_thumbnail_size / m_freq_bands)
    , m_num_samples (m_thumbnail_size)
    , m_freq_bands (m_thumbnail_size)
    , m_fileuri (fileuri)
    , m_output_file (options.m_output_file)
    , m_pipeline (0)
    , m_decoder (0)
    , m_spectrum (0)
    , m_sink (0)
    , m_bus (0)
    , m_sample_no (0)
    , m_prerolled (false)
    {
        // Set up the drawing surface
        m_surface = Cairo::ImageSurface::create (Cairo::FORMAT_RGB24,
                                                 m_thumbnail_size,
                                                 m_thumbnail_size);
        m_cr = Cairo::Context::create (m_surface);
        m_cr->set_source_rgb (1.0, 1.0, 1.0);
        m_cr->paint ();
    }

    ~App ()
    {
        g_object_unref (m_bus);
        g_object_unref (m_pipeline);
    }

    int run ()
    {
        try {
            m_mainloop = Glib::MainLoop::create();
            m_pipeline = gst_pipeline_new (0);
            m_decoder = gst_element_factory_make ("uridecodebin", 0);
            m_spectrum = gst_element_factory_make ("spectrum", 0);
            m_sink = gst_element_factory_make ("fakesink", 0);
            m_bus = gst_pipeline_get_bus (GST_PIPELINE (m_pipeline));

            gst_bin_add_many (GST_BIN (m_pipeline),
                              m_decoder, m_spectrum, m_sink, NULL);

            g_object_set (m_decoder,
                          "uri", Glib::filename_to_utf8 (m_fileuri).c_str (),
                          NULL);
            g_signal_connect (m_decoder, "pad-added",
                              G_CALLBACK (on_pad_added_proxy), this);

            gint64 interval = (m_spectrogram_length /
                               static_cast<double>(m_num_samples)) *
                static_cast<double>(GST_SECOND);
            g_object_set (m_spectrum,
                          "post-messages", TRUE,
                          "interval", interval,
                          "threshold", static_cast<int>(m_threshold),
                          "bands", m_freq_bands,
                          NULL);
            gst_element_link (m_spectrum, m_sink);

            gst_bus_add_signal_watch (m_bus);

            g_signal_connect (m_bus, "message::eos",
                              G_CALLBACK (on_eos_proxy), this);
            g_signal_connect (m_bus, "message::info",
                              G_CALLBACK (on_error_message), this);
            g_signal_connect (m_bus, "message::warning",
                              G_CALLBACK (on_error_message), this);
            g_signal_connect (m_bus, "message::error",
                              G_CALLBACK (on_error_message), this);
            g_signal_connect (m_bus, "message::element",
                              G_CALLBACK (on_element_message_proxy), this);

            m_prerolled = (gst_element_set_state (m_pipeline, GST_STATE_PAUSED) ==
                           GST_STATE_CHANGE_SUCCESS);

            if (!m_prerolled)
            {
                g_signal_connect (m_bus, "message::async-done",
                                  G_CALLBACK (on_async_done_proxy), this);
            }

            m_mainloop->run ();
        } catch (std::exception &e)
        {
            gst_element_set_state (m_pipeline, GST_STATE_NULL);
            g_printerr ("%s", e.what ());
            return 1;
        }
        return 0;
    }

    static void on_pad_added_proxy (GstElement *element,
                                    GstPad *pad,
                                    gpointer user_data)
    {
        App *self = static_cast<App*>(user_data);
        self->on_pad_added (element, pad);
    }

    bool start_pipeline ()
    {
        // only process the first X seconds
        bool success = gst_element_seek (m_pipeline, 1.0, GST_FORMAT_TIME,
                                         GST_SEEK_FLAG_FLUSH,
                                         GST_SEEK_TYPE_SET,
                                         m_start * GST_SECOND,
                                         GST_SEEK_TYPE_SET,
                                         (m_start + m_spectrogram_length) * GST_SECOND);

        if (!success)
            g_warning ("Failed to seek to first %g seconds", m_spectrogram_length);

        gst_element_set_state (m_pipeline, GST_STATE_PLAYING);

        return false;
    }

    void on_pad_added (GstElement *, GstPad *pad)
    {
        GstCaps *caps = gst_pad_query_caps (pad, NULL);
        GstStructure *structure = gst_caps_get_structure (caps, 0);
        const char *name = gst_structure_get_name (structure);

        if (g_str_has_prefix (name, "audio/"))
        {
            GstPad *spectrum_pad =
                gst_element_get_static_pad (m_spectrum, "sink");

            if (!gst_pad_link (pad, spectrum_pad) == GST_PAD_LINK_OK)
                g_warning ("unable to link pad");

            if (m_prerolled)
                Glib::signal_idle ().connect (sigc::mem_fun (this,
                                                             &App::start_pipeline));
        }
        gst_caps_unref (caps);
    }

    static void on_error_message (GstBus *, GstMessage *message, gpointer)
    {
        GError *error = NULL;
        gchar *debug = NULL;

        switch (message->type)
        {
            case GST_MESSAGE_INFO:
                gst_message_parse_info (message, &error, &debug);
                break;
            case GST_MESSAGE_WARNING:
                gst_message_parse_warning (message, &error, &debug);
                break;
            case GST_MESSAGE_ERROR:
                gst_message_parse_error (message, &error, &debug);
                break;
            default:
                g_warning ("unexpected message type");
        }

        if (error)
            g_print ("%s", error->message);
        if (debug)
            g_print ("%s", debug);

        if (message->type == GST_MESSAGE_ERROR)
            throw std::runtime_error (debug);

        g_clear_error (&error);
        g_free (debug);
    }

    static void on_eos_proxy (GstBus *bus,
                              GstMessage *message,
                              gpointer user_data)
    {
        App *self = static_cast<App*>(user_data);
        self->on_eos (bus, message);
    }

    void on_eos (GstBus *, GstMessage *)
    {
        gst_element_set_state (m_pipeline, GST_STATE_NULL);

        m_surface->write_to_png (m_output_file);
        m_mainloop->quit ();
    }

    static void on_element_message_proxy (GstBus *bus,
                                          GstMessage *message,
                                          gpointer user_data)
    {
        App *self = static_cast<App*>(user_data);
        const GstStructure *structure = gst_message_get_structure (message);
        if (gst_structure_has_name (structure, "spectrum"))
            self->on_spectrum (bus, structure);
    }

    // example data:
    // spectrum, endtime=(guint64)189252222222, timestamp=(guint64)189152222222,
    // stream-time=(guint64)189152222222, running-time=(guint64)189152222222,
    // duration=(guint64)100000000, magnitude=(float){ -36.146251678466797,
    // -22.013933181762695, -27.303266525268555, -39.544231414794922,
    // -47.111648559570312, -48.796600341796875, -51.38055419921875,
    // -52.118595123291016, -48.332794189453125, -50.224254608154297,
    // -54.032829284667969, -47.853244781494141, -48.721431732177734,
    // -54.715785980224609, -53.286895751953125, -57.693508148193359,
    // -59.937911987304688, -59.571735382080078, -57.537551879882812,
    // -58.367549896240234, -59.908416748046875, -59.729766845703125, -60, -60,
    // -60, -59.997806549072266, -59.669063568115234, -58.566783905029297,
    // -57.596271514892578, -57.8333740234375, -59.022228240966797,
    // -59.885730743408203, -60, -60, -60, -60, -60, -60, -60, -60, -60, -60,
    // -60, -60, -60, -60, -60, -60, -60, -60, -60, -60, -60, -60, -60, -60,
    // -60, -60, -60, -60, -60, -60, -60, -60, -60, -60, -60, -60, -60, -60,
    // -60, -60, -60, -60, -60, -60, -60, -60, -60, -60, -60, -60, -60, -60,
    // -60, -60, -60, -60, -60, -60, -60, -60, -60, -60, -60, -60, -60, -60,
    // -60, -60, -60, -60, -60, -60, -60, -60, -60, -60, -60, -60, -60, -60,
    // -60, -60, -60, -60, -60, -60, -60, -60, -60, -60, -60, -60, -60, -60,
    // -60, -60 };

    void on_spectrum (GstBus *, const GstStructure *structure)
    {
        g_assert (m_cr);

        // if I ask for an interval that is equal to LENGTH/NUM_SAMPLES, this
        // will result in NUM_SAMPLES+1 messages being emitted, so just ignore
        // messages that are beyond our size.
        if (m_sample_no > m_thumbnail_size)
            return;

        const GValue *val = gst_structure_get_value (structure, "magnitude");
        int size = gst_value_list_get_size (val);
        int i;

        // the inflection point between the two halves of the alpha formula
        const float TX = 0.6;
        const float TY = 0.85;
        // multiplier for the first segment
        const float k = (1 / TX) * (1 / TX) * TY;
        // slope and offset of the second segment
        static const float m = (1.0 - TY) / (1.0 - TX);
        static const float b = TY - m * TX;
        unsigned char *data = m_surface->get_data ();
        const int stride = m_surface->format_stride_for_width (Cairo::FORMAT_RGB24, m_thumbnail_size);

        m_surface->flush ();
        for (i = 0; i < size; ++i)
        {
            const GValue *floatval = gst_value_list_get_value (val, i);
            float v = g_value_get_float (floatval);
            double shade = (v - m_threshold) / std::abs(m_threshold);
            if (shade > 0.0)
            {
                unsigned char *pixel = data + ((size - 1 - i) * stride) +
                    m_sample_no * sizeof (guint32);
                // Try to decrease the background noise a bit while making the
                // foreground noise stand out a bit better.  From 0 to T, we
                // use a parabolic (squared) slope to de-emphasize the lower
                // levels, and from T and up, we simply map the aplitude
                // directly to the alpha.
                if (shade < TX)
                {
                    shade = k * shade * shade;
                }
                else
                {
                    shade = m * shade + b;
                }

                // clamp value betwen 0.0 and 1.0, just in case
                unsigned int byte = 0xFF - (std::max (0.0, std::min (1.0, shade)) * 0xFF);

                memset (pixel, byte, sizeof (guint32));
            }
        }
        m_surface->mark_dirty ();
        ++m_sample_no;
    }

    static void on_async_done_proxy (GstBus *bus,
                                     GstMessage *message,
                                     gpointer user_data)
    {
        App *self = static_cast<App*>(user_data);
        self->on_async_done (bus, message);
    }

    void on_async_done (GstBus *, GstMessage *)
    {
        if (m_prerolled)
            return;

        m_prerolled = true;
        start_pipeline ();
        g_signal_handlers_disconnect_by_func (m_bus,
                                              (gpointer)on_async_done_proxy,
                                              this);
    }


private:
    Glib::RefPtr<Glib::MainLoop> m_mainloop;

    double m_spectrogram_length;
    double m_start;
    double m_threshold;
    double m_thumbnail_size;
    double m_sample_width;
    double m_sample_height;
    int m_num_samples;
    int m_freq_bands;

    std::string m_fileuri;
    std::string m_output_file;

    GstElement *m_pipeline;
    GstElement *m_decoder; // weak ref
    GstElement *m_spectrum; // weak ref
    GstElement *m_sink; // weak ref
    GstBus *m_bus;

    Cairo::RefPtr<Cairo::ImageSurface> m_surface;
    Cairo::RefPtr<Cairo::Context> m_cr;

    int m_sample_no;
    bool m_prerolled;
};

int main (int argc, char** argv)
{
    Glib::init ();

    try
    {
        OptionContext octx;
        octx.parse (argc, argv);

        if (argc != 2)
        {
            g_print ("%s\n", octx.get_help().c_str ());
            std::exit (0);
        }

        int iterations = octx.m_options.m_benchmark;

        if (iterations > 0)
        {
            Glib::Timer timer;
            for (int i = 0; i < octx.m_options.m_benchmark; ++i)
            {
                App app (argv[1], octx.m_options);
                app.run();
                g_print (".");
            }
            double elapsed = timer.elapsed ();
            g_print ("\nTotal time elepased: %g\n", elapsed);
            g_print ("Mean iteration time: %g\n", elapsed / iterations);
        }
        else
        {
            App app (argv[1], octx.m_options);
            return app.run();
        }
    }
    catch (std::exception &e)
    {
        g_printerr ("%s\n", e.what ());
    }
    catch (Glib::Error &e)
    {
        g_printerr ("%s\n", e.what ().c_str ());
    }
    return 1;
}
