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
#include <pangomm/init.h>
#include <pangomm.h>
#include <glibmm.h>
#include <giomm.h>
#include <gst/gst.h>

const double DEFAULT_HEIGHT = 200.0;
const double DEFAULT_WIDTH = 0.0;
const double DEFAULT_RESOLUTION = 100.0; // pixels per second
const double DEFAULT_NOISE_THRESHOLD = -100.0;
const double DEFAULT_MAX_FREQUENCY = 12000;
const char * DEFAULT_OUTPUT_FILENAME = "sonogram.png";
const bool DEFAULT_DRAW_GRID = false;

const double GRID_MARKER_LARGE = 6.0;
const double GRID_MARKER_MED = 4.0;
const double GRID_MARKER_SMALL = 2.0;
const double GRID_ALPHA_DARK = 0.08;
const double GRID_ALPHA_LIGHT = 0.04;
const int FONT_SIZE = 7;
const char* FONT_FAMILY = "monospace";

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
          , m_height (DEFAULT_HEIGHT)
          , m_width (DEFAULT_WIDTH)
          , m_resolution (DEFAULT_RESOLUTION)
          , m_threshold (DEFAULT_NOISE_THRESHOLD)
          , m_output_file (DEFAULT_OUTPUT_FILENAME)
          , m_max_frequency (DEFAULT_MAX_FREQUENCY)
          , m_draw_grid (DEFAULT_DRAW_GRID)
          , m_benchmark (0)
    {
        add_entry (OptionEntry ('h', "height",
                                ustring::compose ("Size in pixels of the height of the sonogram (default %1px)",
                                                  DEFAULT_HEIGHT)),
                   m_height);
        add_entry (OptionEntry ('w', "width",
                                ustring::compose ("Size in pixels of the width of the sonogram (default unlimited)",
                                                  DEFAULT_WIDTH)),
                   m_width);
        add_entry (OptionEntry ('r', "resolution",
                                ustring::compose ("Number of pixels per second of audio (default %1px)",
                                                  DEFAULT_RESOLUTION)),
                   m_resolution);
        add_entry (OptionEntry ('t', "noise-threshold",
                                ustring::compose ("Treat all signals below this noise threshold (in dB) as silence (default %1)",
                                                  DEFAULT_NOISE_THRESHOLD)),
                   m_threshold);
        add_entry (OptionEntry ('f', "max-frequency",
                                ustring::compose ("The maximum frequency to plot on the sonogram (default %1)",
                                                  DEFAULT_MAX_FREQUENCY)),
                   m_max_frequency);
        add_entry (OptionEntry ('g', "grid", "Draw grid"),
                   m_draw_grid);
        add_entry_filename (OptionEntry ('o', "output",
                                         ustring::compose ("File name for generated file (default '%1')",
                                                           DEFAULT_OUTPUT_FILENAME)),
                            m_output_file);
        add_entry (OptionEntry ("benchmark",
                                "Run the specified number of times and report average time spent"),
                   m_benchmark);
    }

    double m_height;
    double m_width;
    double m_resolution;
    double m_threshold;
    std::string m_output_file;
    double m_max_frequency;
    bool m_draw_grid;
    int m_benchmark;
};

class OptionContext : public Glib::OptionContext
{
public:
    OptionContext ()
        : Glib::OptionContext ("( FILE_URI | FILE_PATH )")
    {
        set_main_group (m_options);
        g_option_context_add_group (gobj (), gst_init_get_option_group ());
    }

    AppOptions m_options;
};

class App
{
public:
    App (const std::string & filearg, AppOptions &options)
    : m_threshold (options.m_threshold)
    , m_height (options.m_height)
    , m_width (options.m_width)
    , m_resolution (options.m_resolution)
    , m_sampling_rate (0)
    , m_max_frequency (options.m_max_frequency)
    , m_draw_grid (options.m_draw_grid)
    , m_output_file (options.m_output_file)
    , m_pipeline (0)
    , m_decoder (0)
    , m_spectrum (0)
    , m_sink (0)
    , m_bus (0)
    , m_sample_no (0)
    , m_prerolled (false)
    {
        g_debug("%s", G_STRFUNC);
        Glib::RefPtr<Gio::File> f = Gio::File::create_for_commandline_arg(filearg);
        m_fileuri = f->get_uri();
    }

    ~App ()
    {
        g_object_unref (m_bus);
        g_object_unref (m_pipeline);
    }

    void set_duration()
    {
        gint64 duration = 0;
        GstFormat format = GST_FORMAT_TIME;
        if (!gst_element_query_duration(m_pipeline, &format, &duration))
            throw std::runtime_error("Couldn't query duration");
        g_debug("duration is %li", duration);

        //set up the cairo surface
        double num_samples = (m_resolution * GST_TIME_AS_SECONDS(duration));

        if (!m_width)
            m_width = num_samples;

        m_surface = Cairo::ImageSurface::create (Cairo::FORMAT_ARGB32,
                                                 m_width,
                                                 m_height);
        m_cr = Cairo::Context::create (m_surface);
        m_cr->set_source_rgb (1.0, 1.0, 1.0);
        m_cr->paint ();

    }

    int run ()
    {
        g_debug("%s", G_STRFUNC);
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

            GstStateChangeReturn ret = gst_element_set_state (m_pipeline, GST_STATE_PAUSED);
            g_debug ("set_state return = %u", ret);
            m_prerolled = (ret == GST_STATE_CHANGE_SUCCESS);

            if (!m_prerolled)
            {
                g_debug("not prerolled, waiting for async-done");
                g_signal_connect (m_bus, "message::async-done",
                                  G_CALLBACK (on_async_done_proxy), this);
            }
            else
            {
                set_duration();
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
        g_debug("%s", G_STRFUNC);
        App *self = static_cast<App*>(user_data);
        self->on_pad_added (element, pad);
    }

    bool start_pipeline ()
    {
        g_debug("%s", G_STRFUNC);
        if (!m_sampling_rate)
        {
            GstPad *spectrum_pad =
                gst_element_get_static_pad (m_spectrum, "sink");

            GstCaps *caps = gst_pad_get_negotiated_caps (spectrum_pad);
            GstStructure *structure = gst_caps_get_structure (caps, 0);

            const GValue *val = gst_structure_get_value (structure, "rate");
            if (val)
                m_sampling_rate = g_value_get_int (val);

            g_debug("sampling rate: %i", m_sampling_rate);

            gst_caps_unref (caps);
            gst_object_unref (spectrum_pad);

            int band_freq = m_max_frequency / m_height;
            // according to nyquist, max frequency is half the sampling rate...
            int num_bands = (m_sampling_rate / 2) / band_freq;

            gint64 interval = GST_SECOND / m_resolution;
            g_object_set (m_spectrum,
                          "post-messages", TRUE,
                          "interval", interval,
                          "threshold", static_cast<int>(m_threshold),
                          "bands", num_bands,
                          NULL);
        }

        gst_element_set_state (m_pipeline, GST_STATE_PLAYING);

        return false;
    }

    void on_pad_added (GstElement *, GstPad *pad)
    {
        g_debug("%s", G_STRFUNC);
        GstCaps *caps = gst_pad_get_caps (pad);
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
        g_debug("%s", G_STRFUNC);
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

    static void on_eos_proxy (GstBus *,
                              GstMessage *,
                              gpointer user_data)
    {
        g_debug("%s", G_STRFUNC);
        App *self = static_cast<App*>(user_data);
        self->finish ();
    }

    void finish ()
    {
        g_debug("%s", G_STRFUNC);
        gst_element_set_state (m_pipeline, GST_STATE_NULL);

        if (m_draw_grid)
        {
            Pango::FontDescription fd;
            fd.set_family(FONT_FAMILY);
            fd.set_absolute_size(FONT_SIZE * Pango::SCALE);
            fd.set_weight(Pango::WEIGHT_NORMAL);
            fd.set_stretch(Pango::STRETCH_CONDENSED);

            double pxPerKhz = m_height / (m_max_frequency / 1000);
            double nKhz = static_cast<int>(m_max_frequency / 1000);

            double borderX, borderY;
            // limit scope
            {
                Glib::RefPtr<Pango::Layout> layout = Pango::Layout::create(m_cr);
                layout->set_font_description(fd);
                layout->set_text(Glib::ustring::compose("%1k", nKhz));
                Pango::Rectangle extents = layout->get_pixel_logical_extents();
                borderX = GRID_MARKER_SMALL + extents.get_width() + GRID_MARKER_SMALL + GRID_MARKER_LARGE;
                borderY = GRID_MARKER_SMALL + extents.get_height() + GRID_MARKER_SMALL + GRID_MARKER_LARGE;
            }

            double w = borderX + m_width;
            double h = borderY + m_height;
            Cairo::RefPtr<Cairo::ImageSurface> graph = Cairo::ImageSurface::create (Cairo::FORMAT_ARGB32, w, h);
            Cairo::RefPtr<Cairo::Context> cr = Cairo::Context::create (graph);
            // clear to white
            cr->set_source_rgb (1.0, 1.0, 1.0);
            cr->paint ();

            cr->set_source(m_surface, borderX, 0);
            cr->paint();

            cr->scale (1, -1);
            cr->translate (0, -h);
            // translate by 0.5 to be pixel-aligned
            cr->translate(-0.5, -0.5);
            cr->translate(borderX, borderY);

            // draw main axes
            cr->set_source_rgb(0.0, 0.0, 0.0);
            cr->move_to(0, m_height);
            cr->set_line_width(1.0);
            cr->line_to (0, 0);
            cr->line_to (m_width, 0);
            cr->stroke();

            for (int f = 1; f <= nKhz; f++)
            {
                cr->save();
                double markerSize = GRID_MARKER_SMALL;
                double gridAlpha = GRID_ALPHA_LIGHT;

                // always draw text for the max frequency
                bool drawText = (f == nKhz);

                if ((f % 5) == 0)
                {
                    markerSize = GRID_MARKER_MED;
                    gridAlpha = GRID_ALPHA_DARK;
                    drawText = true;
                }

                if ((f % 10) == 0)
                {
                    markerSize = GRID_MARKER_LARGE;
                }

                // align to pixel
                int y = static_cast<int>(f * pxPerKhz);
                cr->move_to (-markerSize, y);
                cr->line_to (0, y);
                cr->stroke();

                // draw grid line with alpha
                cr->set_source_rgba(0.0, 0.0, 0.0, gridAlpha);
                cr->move_to(0, y);
                cr->line_to(m_width, y);
                cr->stroke();

                if (drawText)
                {
                    Glib::RefPtr<Pango::Layout> layout = Pango::Layout::create(cr);
                    layout->set_font_description(fd);
                    layout->set_text(Glib::ustring::compose("%1k", f));
                    Pango::Rectangle extents = layout->get_pixel_logical_extents();
                    int tx = - (GRID_MARKER_LARGE + GRID_MARKER_SMALL) - extents.get_width();
                    int ty = std::min(y + (extents.get_height() / 2.0), m_height);
                    cr->move_to (tx, ty);
                    // revert inverted scale so that the text doesnt' get mirrored
                    cr->scale(1, -1);
                    layout->update_from_cairo_context(cr);
                    cr->set_source_rgb(0.0, 0.0, 0.0);
                    layout->show_in_cairo_context(cr);
                }

                cr->restore();
            }

            // draw a line every second
            int seconds = static_cast<int>(m_width / m_resolution);
            for (int s = 1; s <= seconds; s++)
            {
                cr->save();
                double markerSize = GRID_MARKER_MED;
                if (s % 5 == 0)
                    markerSize = GRID_MARKER_LARGE;

                // draw text every N marks
                int textN = 1;
                if (m_resolution <= 10)
                    textN = 10;
                else if (m_resolution <= 30)
                    textN = 5;

                bool drawText = (s % textN) == 0;

                int x = static_cast<int>(m_resolution * s);
                cr->move_to (x, -markerSize);
                cr->line_to (x, 0);
                cr->stroke();

                if (drawText)
                {
                    Glib::RefPtr<Pango::Layout> layout = Pango::Layout::create(cr);
                    layout->set_font_description(fd);
                    layout->set_text(Glib::ustring::compose("%1s", s));
                    Pango::Rectangle extents = layout->get_pixel_logical_extents();
                    int tx = std::min (x - (extents.get_width() / 2.0), m_width - extents.get_width());
                    int ty = - (GRID_MARKER_LARGE + GRID_MARKER_SMALL);
                    cr->move_to (tx, ty);
                    // revert inverted scale so that the text doesnt' get mirrored
                    cr->scale(1, -1);
                    layout->update_from_cairo_context(cr);
                    cr->set_source_rgb(0.0, 0.0, 0.0);
                    layout->show_in_cairo_context(cr);
                }

                cr->restore();
            }

            graph->write_to_png (m_output_file);
        }
        else
        {
            m_surface->write_to_png (m_output_file);
        }
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

        if (!m_cr)
        {
            //g_debug("got 'spectrum' message before duration: %s", gst_structure_to_string(structure));
            return;
        }

        // if I ask for an interval that is equal to LENGTH/NUM_SAMPLES, this
        // will result in NUM_SAMPLES+1 messages being emitted, so just ignore
        // messages that are beyond our size.
        if (m_sample_no >= m_width)
        {
            finish();
            return;
        }

        const GValue *val = gst_structure_get_value (structure, "magnitude");
        int size = gst_value_list_get_size (val);
        int i;

        if (m_height < size)
            size = m_height;

        // the inflection point between the two halves of the alpha formula
        const float TX = 0.6;
        const float TY = 0.85;
        // multiplier for the first segment
        const float k = (1 / TX) * (1 / TX) * TY;
        // slope and offset of the second segment
        static const float m = (1.0 - TY) / (1.0 - TX);
        static const float b = TY - m * TX;
        unsigned char *data = m_surface->get_data ();
        const int stride = m_surface->format_stride_for_width (Cairo::FORMAT_ARGB32, m_width);

        m_surface->flush ();
        for (i = 0; i < size; ++i)
        {
            const GValue *floatval = gst_value_list_get_value (val, i);
            float v = g_value_get_float (floatval);
            double shade = (v - m_threshold) / std::abs(m_threshold);
            if (shade > 0.0)
            {
                unsigned char *pixel = data + ((static_cast<int>(m_height) - 1 - i) * stride) +
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
                pixel[3] = 0xff;
            }
        }
        m_surface->mark_dirty ();
        ++m_sample_no;
    }

    static void on_async_done_proxy (GstBus *bus,
                                     GstMessage *message,
                                     gpointer user_data)
    {
        g_debug("%s", G_STRFUNC);
        App *self = static_cast<App*>(user_data);
        self->on_async_done (bus, message);
    }

    void on_async_done (GstBus *, GstMessage *)
    {
        g_debug("%s", G_STRFUNC);
        if (m_prerolled)
            return;

        m_prerolled = true;
        set_duration();
        start_pipeline ();
        g_signal_handlers_disconnect_by_func (m_bus,
                                              (gpointer)on_async_done_proxy,
                                              this);
    }


private:
    Glib::RefPtr<Glib::MainLoop> m_mainloop;

    double m_threshold;
    double m_height;
    double m_width;
    double m_resolution;
    int m_sampling_rate;
    double m_max_frequency;
    bool m_draw_grid;

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
    Gio::init ();
    Pango::init ();

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
