/*******************************************************************************
 *
 *  Copyright (c) 2011 Jonathon Jongsma
 *
 *  This file is part of spectrogram-thumbnailer
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

const double SAMPLE_INTERVAL = 0.01;
const double DEFAULT_THUMBNAIL_SIZE = 128.0;
const double DEFAULT_SPECTROGRAM_LENGTH = 5.0;
const double DEFAULT_NOISE_THRESHOLD = -100.0;
const char * DEFAULT_OUTPUT_FILENAME = "thumbnail.png";

class OptionEntry : public Glib::OptionEntry
{
public:
    OptionEntry (gchar short_name,
                 const Glib::ustring &long_name = Glib::ustring(),
                 const Glib::ustring &description = Glib::ustring())
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
    {
        add_entry (OptionEntry ('s', "size",
                                Glib::ustring::compose ("Size in pixels of the generated thumbnail (default %1px)",
                                                        DEFAULT_THUMBNAIL_SIZE)),
                   m_size);
        add_entry (OptionEntry ('l', "length",
                                Glib::ustring::compose ("Length (in seconds) of audio to use for thumbnail (default %1s)",
                                                        DEFAULT_SPECTROGRAM_LENGTH)),
                   m_length);
        add_entry (OptionEntry ('t', "threshold",
                                Glib::ustring::compose ("Noise threshold in dB (default -100)",
                                                        DEFAULT_NOISE_THRESHOLD)),
                   m_threshold);
        add_entry_filename (OptionEntry ('o', "output",
                                         Glib::ustring::compose ("file name for generated thumbnail (default '%1')",
                                                                 DEFAULT_OUTPUT_FILENAME)),
                            m_output_file);
    }

    double m_size;
    double m_length;
    double m_threshold;
    std::string m_output_file;
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
    App (int &argc, char** &argv)
    : m_pipeline (0)
    , m_decoder (0)
    , m_spectrum (0)
    , m_sink (0)
    , m_bus (0)
    , m_sample_no (0)
    {
        parse_args (argc, argv);

        m_mainloop = Glib::MainLoop::create();
        m_pipeline = gst_pipeline_new (0);
        m_decoder = gst_element_factory_make ("uridecodebin", 0);
        m_spectrum = gst_element_factory_make ("spectrum", 0);
        m_sink = gst_element_factory_make ("fakesink", 0);
        m_bus = gst_pipeline_get_bus (GST_PIPELINE (m_pipeline));

        gst_bin_add_many (GST_BIN (m_pipeline), m_decoder, m_spectrum, m_sink, NULL);

        g_object_set (m_decoder,
                      "uri", Glib::filename_to_utf8 (m_fileuri).c_str (),
                      NULL);
        g_signal_connect (m_decoder, "pad-added", G_CALLBACK (on_pad_added_proxy), this);

        g_object_set (m_spectrum,
                      "post-messages", TRUE,
                      "interval", static_cast<guint64>(SAMPLE_INTERVAL *
                                                       static_cast<double>(GST_SECOND)),
                      "threshold", static_cast<int>(m_threshold),
                      "bands", m_freq_bands,
                      NULL);
        gst_element_link (m_spectrum, m_sink);

        gst_bus_add_signal_watch (m_bus);
        g_signal_connect (m_bus, "message::eos", G_CALLBACK (on_eos_proxy), this);
        g_signal_connect (m_bus, "message::info", G_CALLBACK (on_error_message), this);
        g_signal_connect (m_bus, "message::warning", G_CALLBACK (on_error_message), this);
        g_signal_connect (m_bus, "message::error", G_CALLBACK (on_error_message), this);
        g_signal_connect (m_bus, "message::element", G_CALLBACK (on_element_message_proxy), this);
    }

    void parse_args (int &argc, char **&argv)
    {
        OptionContext octx;
        octx.parse (argc, argv);

        if (argc != 2)
        {
            g_print ("%s\n", octx.get_help().c_str ());
            throw std::runtime_error ("");
        }

        m_fileuri = argv[1];
        m_output_file = octx.m_options.m_output_file;
        m_threshold = octx.m_options.m_threshold;
        m_thumbnail_size = octx.m_options.m_size;
        m_spectrogram_length = octx.m_options.m_length;

        // set resolution to double the number of pixels, but with a max
        // limit at 250
        m_freq_bands = std::min (static_cast<int>(2*m_thumbnail_size), 250);

        m_sample_height = m_thumbnail_size / m_freq_bands;
        m_sample_width = m_thumbnail_size / (m_spectrogram_length / SAMPLE_INTERVAL);
    }

    ~App ()
    {
        g_object_unref (m_bus);
        g_object_unref (m_pipeline);
    }

    int run ()
    {
        gst_element_set_state (m_pipeline, GST_STATE_PAUSED);
        try {
            m_mainloop->run ();
        } catch (std::exception &e)
        {
            gst_element_set_state (m_pipeline, GST_STATE_NULL);
            g_printerr ("%s", e.what ());
            return 1;
        }
        return 0;
    }

    static void on_pad_added_proxy (GstElement *element, GstPad *pad, gpointer user_data)
    {
        App *self = static_cast<App*>(user_data);
        self->on_pad_added (element, pad);
    }

    void on_pad_added (GstElement *, GstPad *pad)
    {
        GstCaps *caps = gst_pad_get_caps (pad);
        GstStructure *structure = gst_caps_get_structure (caps, 0);
        const char *name = gst_structure_get_name (structure);

        if (g_str_has_prefix (name, "audio/"))
        {
            GstPad *spectrum_pad = gst_element_get_static_pad (m_spectrum, "sink");
            if (!gst_pad_link (pad, spectrum_pad) == GST_PAD_LINK_OK)
            {
                g_warning ("unable to link pad");
            }

            // only process the first X seconds
            bool success = gst_element_seek (m_pipeline, 1.0, GST_FORMAT_TIME,
                                             GST_SEEK_FLAG_FLUSH,
                                             GST_SEEK_TYPE_SET, 0,
                                             GST_SEEK_TYPE_SET, m_spectrogram_length * GST_SECOND);

            if (!success)
                g_warning ("Failed to seek to first %g seconds", m_spectrogram_length);

            gst_element_set_state (m_pipeline, GST_STATE_PLAYING);

            // Set up the drawing surface
            m_surface = Cairo::ImageSurface::create (Cairo::FORMAT_ARGB32,
                                                     m_thumbnail_size, m_thumbnail_size);
            m_cr = Cairo::Context::create (m_surface);
            m_cr->translate (0, m_thumbnail_size);
            m_cr->scale (1.0, -1.0);
            m_cr->set_source_rgb (1.0, 1.0, 1.0);
            m_cr->paint ();
        }
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

        g_clear_error (&error);
        g_free (debug);
    }

    static void on_eos_proxy (GstBus *bus, GstMessage *message, gpointer user_data)
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

    static void on_element_message_proxy (GstBus *bus, GstMessage *message, gpointer user_data)
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

        for (i = 0; i < size; i++)
        {
            const GValue *floatval = gst_value_list_get_value (val, i);
            float v = g_value_get_float (floatval);
            double shade = (v - m_threshold) / std::abs(m_threshold);
            // clamp value betwen 0.0 and 1.0, just in case
            shade = std::max (0.0, std::min (1.0, shade));
            if (shade > 0.0)
            {
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

                // this is likely going to be quite slow.  it'd be much faster
                // to simply access the imagesurface data and write to it
                // directly
                m_cr->rectangle (m_sample_no * m_sample_width, i * m_sample_height,
                                 m_sample_width, m_sample_height);
                m_cr->set_source_rgba (0.0, 0.0, 0.0, shade);
                m_cr->fill ();
            }
        }
        ++m_sample_no;
    }

private:
    Glib::RefPtr<Glib::MainLoop> m_mainloop;

    double m_spectrogram_length;
    double m_threshold;
    double m_thumbnail_size;
    double m_sample_width;
    double m_sample_height;
    int m_freq_bands;

    std::string m_fileuri;
    std::string m_output_file;

    GstElement *m_pipeline;
    GstElement *m_decoder; // weak ref
    GstElement *m_spectrum; // weak ref
    GstElement *m_sink; // weak ref
    GstBus *m_bus;

    Cairo::RefPtr<Cairo::Surface> m_surface;
    Cairo::RefPtr<Cairo::Context> m_cr;
    int m_sample_no;
};

int main (int argc, char** argv)
{
    Glib::init ();

    App app (argc, argv);
    return app.run();
}
