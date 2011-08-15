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

const double SPECTROGRAM_LENGTH = 10.0;
const double SAMPLE_INTERVAL = 0.01;
const double THRESHOLD = -80.0;
const int NUM_FREQ_BANDS = 200;
const double SAMPLE_SIZE = 2.0;

class App
{
public:
    App (std::string& fileuri)
    : m_mainloop (Glib::MainLoop::create())
    , m_fileuri (fileuri)
    , m_pipeline (gst_pipeline_new (0))
    , m_decoder (gst_element_factory_make ("uridecodebin", 0))
    , m_spectrum (gst_element_factory_make ("spectrum", 0))
    , m_sink (gst_element_factory_make ("fakesink", 0))
    , m_bus (gst_pipeline_get_bus (GST_PIPELINE (m_pipeline)))
    , m_sample_no (0)
    {
        gst_bin_add_many (GST_BIN (m_pipeline), m_decoder, m_spectrum, m_sink, NULL);

        g_object_set (m_decoder,
                      "uri", m_fileuri.c_str (),
                      NULL);
        g_signal_connect (m_decoder, "pad-added", G_CALLBACK (on_pad_added_proxy), this);

        g_object_set (m_spectrum,
                      "post-messages", TRUE,
                      "interval", static_cast<guint64>(SAMPLE_INTERVAL *
                                                       static_cast<double>(GST_SECOND)),
                      "threshold", static_cast<int>(THRESHOLD),
                      "bands", NUM_FREQ_BANDS,
                      NULL);
        gst_element_link (m_spectrum, m_sink);

        gst_bus_add_signal_watch (m_bus);
        g_signal_connect (m_bus, "message::eos", G_CALLBACK (on_eos_proxy), this);
        g_signal_connect (m_bus, "message::info", G_CALLBACK (on_error_message), this);
        g_signal_connect (m_bus, "message::warning", G_CALLBACK (on_error_message), this);
        g_signal_connect (m_bus, "message::error", G_CALLBACK (on_error_message), this);
        g_signal_connect (m_bus, "message::element", G_CALLBACK (on_element_message_proxy), this);
    }

    ~App ()
    {
        g_object_unref (m_bus);
        g_object_unref (m_pipeline);
    }

    int run ()
    {
        gst_element_set_state (m_pipeline, GST_STATE_PAUSED);
        m_mainloop->run ();
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
                                             GST_SEEK_TYPE_SET, SPECTROGRAM_LENGTH * GST_SECOND);

            if (!success)
                g_warning ("Failed to seek to first %g seconds", SPECTROGRAM_LENGTH);

            gst_element_set_state (m_pipeline, GST_STATE_PLAYING);

            // Set up the drawing surface
            int h = SAMPLE_SIZE * NUM_FREQ_BANDS;
            int w = (SPECTROGRAM_LENGTH / SAMPLE_INTERVAL) * SAMPLE_SIZE;
            m_surface = Cairo::ImageSurface::create (Cairo::FORMAT_ARGB32,
                                                     w, h);
            m_cr = Cairo::Context::create (m_surface);
            m_cr->translate (0, h);
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
        m_surface->write_to_png ("thumbnail.png");
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

        for (i = 0; i < size; i++)
        {
            const GValue *floatval = gst_value_list_get_value (val, i);
            float v = g_value_get_float (floatval);
            double shade = (v - THRESHOLD) / std::abs(THRESHOLD);
            // clamp value betwen 0.0 and 1.0, just in case
            shade = std::max (0.0, std::min (1.0, shade));
            if (shade > 0.0)
            {
                // Try to decrease the background noise a bit while making the
                // foreground noise stand out a bit better.  So the slope from 0
                // to 0.5 is a bit steeper (e.g. the low-level noise drops off
                // faster) while the slope from 0.5 to 1.0 is more level.
                if (shade < 0.5)
                {
                    shade *= 1.5;
                }
                else
                {
                    shade = shade * 0.5 + 0.5;
                }

                // this is likely going to be quite slow.  it'd be much faster
                // to simply access the imagesurface data and write to it
                // directly
                m_cr->rectangle (m_sample_no * SAMPLE_SIZE, i * SAMPLE_SIZE,
                                 SAMPLE_SIZE, SAMPLE_SIZE);
                m_cr->set_source_rgba (0.0, 0.0, 0.0, shade);
                m_cr->fill ();
            }
        }
        ++m_sample_no;
    }

private:
    Glib::RefPtr<Glib::MainLoop> m_mainloop;
    Glib::ustring m_fileuri;
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
    if (argc != 2)
    {
        g_print ("Usage: %s FILE_URI\n", argv[0]);
        return 1;
    }

    gst_init (&argc, &argv);
    Glib::init ();

    std::string uri = argv[1];

    try {
    App app (uri);
    return app.run();
    }
    catch (std::exception& e)
    {
        throw e;
    }
}
