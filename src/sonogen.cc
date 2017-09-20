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
#include <pango/pangocairo.h>
#include <glibmm.h>
#ifdef ENABLE_GIO
#include <giomm.h>
#endif

#include <gst/gst.h>

const double DEFAULT_HEIGHT = 200.0;
const double DEFAULT_WIDTH = 0.0;
const double DEFAULT_RESOLUTION = 100.0; // pixels per second
const double DEFAULT_DURATION = 0.0; // seconds
const double DEFAULT_NOISE_FLOOR = -100.0;
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

static const char* format(const char* fmt, ...)
{
    static char m_buf[255];
    va_list argp;
    va_start(argp, fmt);
    g_vsnprintf(m_buf, sizeof(m_buf), fmt, argp);
    va_end(argp);
    return m_buf;
}

typedef enum {
    STATE_START,
    STATE_DURATION,
    STATE_SEEK,
    STATE_GENERATE,
    STATE_DONE
} AppState;

// helper class to avoid mis-matched save()/restore() pairs.  Rely on scoping to
// restore the context to the previously-saved graphics state. Also makes things
// safer in the presence of exceptions / early returns.
struct ContextGuard
{
    ContextGuard(const Cairo::RefPtr<Cairo::Context>& cr)
        : m_cr (cr)
    {
        m_cr->save();
    }

    ~ContextGuard()
    {
        m_cr->restore();
    }

    Cairo::RefPtr<Cairo::Context> m_cr;
};

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

struct AppOptions
{
    AppOptions()
          : height (DEFAULT_HEIGHT)
          , width (DEFAULT_WIDTH)
          , resolution (DEFAULT_RESOLUTION)
          , duration (DEFAULT_DURATION)
          , noise_floor (DEFAULT_NOISE_FLOOR)
          , output_file (DEFAULT_OUTPUT_FILENAME)
          , max_frequency (DEFAULT_MAX_FREQUENCY)
          , draw_grid (DEFAULT_DRAW_GRID)
          , benchmark (0)
          {}

    double height;
    double width;
    double resolution;
    double duration;
    double noise_floor;
    std::string output_file;
    double max_frequency;
    bool draw_grid;
    int benchmark;
};

class AppOptionGroup : public Glib::OptionGroup
{
public:
    AppOptionGroup (AppOptionGroup&);
    AppOptionGroup ()
        : Glib::OptionGroup ("application", "Application options")
    {
        add_entry (OptionEntry ('h', "height",
                                format("Height of the sonogram in pixels (default %fpx)",
                                                  DEFAULT_HEIGHT)),
                   m_options.height);
        add_entry (OptionEntry ('w', "width",
                                "Width of the sonogram in pixels (default unlimited)"),
                   m_options.width);
        add_entry (OptionEntry ('d', "duration",
                                format("Duration of the sonogram in seconds (default unlimited)",
                                                  DEFAULT_DURATION)),
                   m_options.duration);
        add_entry (OptionEntry ('r', "resolution",
                                format("Number of pixels per second of audio (default %fpx)",
                                                  DEFAULT_RESOLUTION)),
                   m_options.resolution);
        add_entry (OptionEntry ('n', "noise-floor",
                                format("Treat signals below this level (in dB) as silence (default %f)",
                                                  DEFAULT_NOISE_FLOOR)),
                   m_options.noise_floor);
        add_entry (OptionEntry ('f', "max-frequency",
                                format("The maximum frequency of the sonogram (default %f)",
                                                  DEFAULT_MAX_FREQUENCY)),
                   m_options.max_frequency);
        add_entry (OptionEntry ('g', "grid", "Draw axes and grid"),
                   m_options.draw_grid);
        add_entry_filename (OptionEntry ('o', "output",
                                         format("Output image file name (default '%s')",
                                                           DEFAULT_OUTPUT_FILENAME)),
                            m_options.output_file);
        add_entry (OptionEntry ("benchmark",
                                "Run the specified number of times and report average time spent"),
                   m_options.benchmark);
    }

    AppOptions m_options;
};

const char *desc =
"This program allows you to generate a sonogram image in PNG\n\
format for a given input audio file (or video file with an audio\n\
track). The output can be customized in various ways, including\n\
adjusting both the horizontal and vertical resolution of the\n\
FFT, and the size of the image to be generated.\n\n\
Note: only two of the options '--duration', '--resolution', and\n\
'--width' can be specified at once.  If all three are specified,\n\
'--resolution' will be ignored.\n\n\
Note: if no width is specified, it will generate a sonogram\n\
for the entire audio track, so the width of the generated image\n\
will depend on the length of the audio.  If a width is given, it\n\
will always generate an image of that width, even if the audio\n\
ends before the width is reached.\n\n\
Note: the height and width only specifies the dimensions of\n\
the sonogram.  If the -g option is used to draw a grid, the size\n\
of the generated image will be expanded to accomodate the\n\
axes and grid.";

class OptionContext : public Glib::OptionContext
{
public:
    OptionContext ()
        : Glib::OptionContext ("( FILE_URI | FILE_PATH )")
    {
        set_main_group (m_option_group);
        g_option_context_add_group (gobj (), gst_init_get_option_group ());
        set_summary("Generate a sonogram image from an audio file");
        set_description(desc);
    }

    AppOptionGroup m_option_group;
};

class App
{
public:
    App (const std::string & filearg, AppOptions &options)
    : m_options (options)
    , m_state(STATE_START)
    , m_sampling_rate (0)
    , m_pipeline (0)
    , m_decoder (0)
    , m_decoder_pad (0)
    , m_convert (0)
    , m_spectrum (0)
    , m_sink (0)
    , m_bus (0)
    , m_duration (0)
    , m_peak_rms (options.noise_floor)
    , m_min_rms (options.noise_floor)
    , m_sample_no (0)
    , m_prerolled (false)
    , m_fd(pango_font_description_new())
    {
        g_debug("%s", G_STRFUNC);
#ifdef ENABLE_GIO
        Glib::RefPtr<Gio::File> f = Gio::File::create_for_commandline_arg(filearg);
        m_fileuri = f->get_uri();
#else
        std::string abspath = filearg;
        if (!Glib::path_is_absolute(filearg))
        {
            abspath = Glib::build_filename(Glib::get_current_dir(), filearg);
        }
        m_fileuri = Glib::filename_to_uri(abspath);
#endif

        if (m_options.duration && m_options.width)
        {
            m_options.resolution = m_options.width / m_options.duration;
        }
        else if (m_options.duration)
        {
            m_options.width = m_options.duration * m_options.resolution;
        }

        pango_font_description_set_family(m_fd, FONT_FAMILY);
        pango_font_description_set_absolute_size(m_fd, FONT_SIZE * PANGO_SCALE);
        pango_font_description_set_weight(m_fd, PANGO_WEIGHT_NORMAL);
        pango_font_description_set_stretch(m_fd, PANGO_STRETCH_CONDENSED);
    }

    ~App ()
    {
        g_object_unref (m_bus);
        g_object_unref (m_pipeline);
        g_object_unref(m_decoder_pad);
        pango_font_description_free(m_fd);
    }

    void reset_pipeline()
    {
        g_debug("%s", G_STRFUNC);
        if (gst_element_set_state(m_pipeline, GST_STATE_PAUSED) != GST_STATE_CHANGE_SUCCESS)
            throw std::runtime_error("Unable to pause pipeline");

        m_prerolled = false;
        /* restart at the beginning */
        if (!gst_element_seek_simple(m_pipeline, GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH, 0)) {
            throw std::runtime_error("Unable to seek to the beginning");
        }
        g_signal_connect (m_bus, "message::async-done",
                          G_CALLBACK (on_async_done_proxy), this);
    }

    void generate_sonogram()
    {
        g_debug("%s", G_STRFUNC);
        //set up the cairo surface
        double seconds = GST_TIME_AS_SECONDS(m_duration);
        double num_samples = (m_options.resolution * seconds);

        g_debug("Total file duration is %g", seconds);

        if (!m_options.width)
            m_options.width = num_samples;

        m_surface = Cairo::ImageSurface::create (Cairo::FORMAT_ARGB32,
                                                 m_options.width,
                                                 m_options.height);
        m_cr = Cairo::Context::create (m_surface);

        m_convert = gst_element_factory_make ("audioconvert", 0);
        gst_element_set_state(m_convert, GST_STATE_PAUSED);
        m_spectrum = gst_element_factory_make ("spectrum", 0);
        gst_element_set_state(m_spectrum, GST_STATE_PAUSED);
        m_filter = gst_element_factory_make ("audiocheblimit", 0);
        gst_element_set_state(m_filter, GST_STATE_PAUSED);
        m_level = gst_element_factory_make ("level", 0);
        gst_element_set_state(m_level, GST_STATE_PAUSED);

        g_object_set (m_filter,
                      "mode", 1, // high-pass
                      "cutoff", 440.0,
                      NULL);

        add(m_convert);
        add(m_spectrum);
        add(m_filter);
        add(m_level);
        gst_element_unlink(m_decoder, m_sink);

        GstPad *convert_pad =
            gst_element_get_static_pad (m_convert, "sink");

        if (!gst_pad_link (m_decoder_pad, convert_pad) == GST_PAD_LINK_OK)
            throw std::runtime_error("unable to link pad");

        if (!gst_element_link (m_convert, m_spectrum)) throw std::runtime_error("Unable to link");
        if (!gst_element_link (m_spectrum, m_filter)) throw std::runtime_error("Unable to link");
        if (!gst_element_link (m_filter, m_level)) throw std::runtime_error("Unable to link");
        if (!gst_element_link (m_level, m_sink)) throw std::runtime_error("Unable to link");

        start_pipeline();
    }

    void on_duration_changed(GstBus *bus, GstMessage *message)
    {
        g_debug("%s", G_STRFUNC);
        if (!gst_element_query_duration(m_pipeline, GST_FORMAT_TIME, &m_duration)) {
            g_warning("Unable to query duration");
        } else {
            g_debug("Duration = %li", m_duration);
        }
    }

    static void on_duration_changed_message_proxy (GstBus *bus,
                                                   GstMessage *message,
                                                   gpointer user_data)
    {
        App *self = static_cast<App*>(user_data);
        self->on_duration_changed(bus, message);
    }

    void calculate_duration()
    {
        if (!m_prerolled || !m_decoder_pad)
            throw std::runtime_error("Missing prerequisites for calculating duration");

        GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(m_pipeline));
            g_signal_connect (m_bus, "message::duration-changed",
                              G_CALLBACK (on_duration_changed_message_proxy), this);

        gst_element_set_state (m_pipeline, GST_STATE_PLAYING);
    }

    void add(GstElement *element)
    {
        if (!m_pipeline)
            throw std::runtime_error("No pipeline");

        if (!gst_bin_add(GST_BIN(m_pipeline), element))
            throw std::runtime_error("Couldn't add element to pipeline");
    }

    int run ()
    {
        g_debug("%s", G_STRFUNC);
        try {
            m_mainloop = Glib::MainLoop::create();
            m_pipeline = gst_pipeline_new (0);
            m_decoder = gst_element_factory_make ("uridecodebin", 0);
            m_sink = gst_element_factory_make ("fakesink", 0);
            m_bus = gst_pipeline_get_bus (GST_PIPELINE (m_pipeline));

            add(m_decoder);
            add(m_sink);

            g_object_set (m_decoder,
                          "uri", m_fileuri.c_str (),
                          NULL);
            g_signal_connect (m_decoder, "pad-added",
                              G_CALLBACK (on_pad_added_proxy), this);

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
                state_done();
            }

            m_mainloop->run ();
        } catch (std::exception &e)
        {
            gst_element_set_state (m_pipeline, GST_STATE_NULL);
            g_error ("%s", e.what ());
            exit(1);
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
        GST_DEBUG_BIN_TO_DOT_FILE(GST_BIN(m_pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "start_pipeline");
        if (!m_sampling_rate)
        {
            GstCaps *caps = gst_pad_get_current_caps (m_decoder_pad);
            if (!caps)
                throw std::runtime_error("Unable to get caps for decoder output");

            GstStructure *structure = gst_caps_get_structure (caps, 0);

            const GValue *val = gst_structure_get_value (structure, "rate");
            if (val)
                m_sampling_rate = g_value_get_int (val);

            g_debug("sampling rate: %i", m_sampling_rate);

            gst_caps_unref (caps);

            int band_freq = m_options.max_frequency / m_options.height;
            // according to nyquist, max frequency is half the sampling rate...
            int num_bands = (m_sampling_rate / 2) / band_freq;

            gint64 interval = GST_SECOND / m_options.resolution;
            g_debug ("setting interval %li", interval);
            g_object_set (m_spectrum,
                          "post-messages", TRUE,
                          "interval", interval,
                          "threshold", static_cast<int>(m_options.noise_floor),
                          "bands", num_bands,
                          NULL);
            g_object_set (m_level,
                          "message", TRUE,
                          "interval", interval / 2,
                          "peak-falloff", 0.0,
                          "peak-ttl", 0,
                          NULL);
        }

        gst_element_set_state (m_pipeline, GST_STATE_PLAYING);

        return false;
    }

    void on_pad_added (GstElement *, GstPad *pad)
    {
        g_debug("%s", G_STRFUNC);
        GstCaps *caps = gst_pad_query_caps (pad, NULL);
        GstStructure *structure = gst_caps_get_structure (caps, 0);
        const char *name = gst_structure_get_name (structure);

        if (g_str_has_prefix (name, "audio/"))
        {
            m_decoder_pad = GST_PAD(g_object_ref(pad));
            GstPad *sink_pad =
                gst_element_get_static_pad (m_sink, "sink");

            if (!gst_pad_link (m_decoder_pad, sink_pad) == GST_PAD_LINK_OK)
                throw std::runtime_error("unable to link pad");

            state_done();
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
        self->state_done ();
    }

    void change_state(AppState new_state)
    {
        m_state = new_state;
        g_debug("%s: new state = %i", G_STRFUNC, m_state);
        char *filename = g_strdup_printf("state-%i", m_state);
        GST_DEBUG_BIN_TO_DOT_FILE(GST_BIN(m_pipeline), GST_DEBUG_GRAPH_SHOW_ALL, filename);
        g_free(filename);
        switch (m_state) {
            case STATE_DURATION:
                calculate_duration();
                break;
            case STATE_SEEK:
                reset_pipeline();
                break;
            case STATE_GENERATE:
                generate_sonogram();
                break;
            case STATE_DONE:
                draw_sonogram();
                m_mainloop->quit ();
                break;
        }
    }

    void state_done()
    {
        g_debug("%s: current state = %i", G_STRFUNC, m_state);
        switch (m_state) {
            case STATE_START:
                if (m_prerolled && m_decoder_pad) {
                    change_state(STATE_DURATION);
                }
                break;
            case STATE_DURATION:
                change_state(STATE_SEEK);
                break;
            case STATE_SEEK:
                change_state(STATE_GENERATE);
                break;
            case STATE_GENERATE:
                change_state(STATE_DONE);
                break;
        }
    }

    void draw_sonogram()
    {
        try {
            g_debug("%s", G_STRFUNC);
            gst_element_set_state (m_pipeline, GST_STATE_NULL);

            Cairo::RefPtr<Cairo::ImageSurface> graph;
            if (m_options.draw_grid)
            {

                double pxPerKhz = m_options.height / (m_options.max_frequency / 1000);
                double nKhz = static_cast<int>(m_options.max_frequency / 1000);

                double borderL, borderB;
                // limit scope
                {
                    // measure width of frequency text
                    PangoLayout* layout = pango_cairo_create_layout(m_cr->cobj());
                    pango_layout_set_font_description(layout, m_fd);
                    pango_layout_set_text(layout, format("%fk", nKhz), -1);
                    PangoRectangle logical_extents;
                    pango_layout_get_extents(layout, NULL, &logical_extents);
                    double w = logical_extents.width / PANGO_SCALE;
                    double h = logical_extents.height / PANGO_SCALE;
                    borderL = GRID_MARKER_SMALL + w + GRID_MARKER_SMALL + GRID_MARKER_LARGE;
                    borderB = GRID_MARKER_LARGE + h + GRID_MARKER_SMALL + GRID_MARKER_LARGE;

                    // now measure text for level (dB) axis
                    layout = pango_cairo_create_layout(m_cr->cobj());
                    pango_layout_set_font_description(layout, m_fd);
                    pango_layout_set_text(layout, format("%fdB", m_options.noise_floor), -1);
                    pango_layout_get_extents(layout, NULL, &logical_extents);
                    w = logical_extents.width / PANGO_SCALE;
                    borderL = std::max(GRID_MARKER_SMALL + w + GRID_MARKER_SMALL + GRID_MARKER_LARGE, borderL);
                }

                int seconds = static_cast<int>(m_options.width / m_options.resolution);
                double w = borderL + m_options.width;
                // draw emplitide below sonograph, witha  much smaller height. Add
                // borderB space between them
                double dbHeight = m_options.height / 6.0;
                double h = borderB + m_options.height + dbHeight;
                graph = Cairo::ImageSurface::create (Cairo::FORMAT_RGB24, w, h);
                Cairo::RefPtr<Cairo::Context> cr = Cairo::Context::create (graph);
                // clear to white
                cr->set_source_rgb (1.0, 1.0, 1.0);
                cr->paint ();

                {
                    ContextGuard gOuter(cr);
                    cr->scale (1, -1);
                    cr->translate (borderL, -m_options.height);
                    // translate by 0.5 to be pixel-aligned
                    cr->translate(-0.5, -0.5);

                    // draw main axes
                    cr->set_source_rgb(0.0, 0.0, 0.0);
                    cr->move_to(0, m_options.height);
                    cr->set_line_width(1.0);
                    cr->line_to (0, 0);
                    cr->line_to (m_options.width, 0);
                    cr->stroke();

                    // draw frequency axis markers
                    for (int f = 1; f <= nKhz; f++)
                    {
                        ContextGuard gFreqAxis(cr);
                        double markerSize = GRID_MARKER_SMALL;
                        double gridAlpha = GRID_ALPHA_LIGHT;
                        int y = static_cast<int>(f * pxPerKhz);

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

                        {
                            ContextGuard gGridLine(cr);
                            // align to pixel
                            cr->move_to (-markerSize, y);
                            cr->line_to (0, y);
                            cr->stroke();

                            // draw grid line with alpha
                            cr->set_source_rgba(0.0, 0.0, 0.0, gridAlpha);
                            cr->move_to(0, y);
                            cr->line_to(m_options.width, y);
                            cr->stroke();
                        }

                        if (drawText)
                        {
                            PangoLayout* layout = pango_cairo_create_layout(cr->cobj());
                            pango_layout_set_font_description(layout, m_fd);
                            pango_layout_set_text(layout, format("%ik", f), -1);
                            PangoRectangle extents;
                            pango_layout_get_extents(layout, NULL, &extents);
                            double w = extents.width / PANGO_SCALE;
                            double h = extents.height / PANGO_SCALE;
                            int tx = - (GRID_MARKER_LARGE + GRID_MARKER_SMALL) - w;
                            int ty = std::min(y + (h / 2.0), m_options.height);
                            cr->move_to (tx, ty);
                            // revert inverted scale so that the text doesnt' get mirrored
                            cr->scale(1, -1);
                            pango_cairo_update_layout (cr->cobj(), layout);
                            pango_cairo_show_layout(cr->cobj(), layout);
                        }
                    }

                    // draw a line every second
                    {
                        ContextGuard guard(cr);
                        for (int s = 1; s <= seconds; s++)
                        {
                            ContextGuard gIter(cr);
                            double markerSize = GRID_MARKER_MED;
                            if (s % 5 == 0)
                                markerSize = GRID_MARKER_LARGE;

                            // draw text every N marks
                            int textN = 1;
                            if (m_options.resolution <= 10)
                                textN = 10;
                            else if (m_options.resolution <= 30)
                                textN = 5;

                            bool drawText = (s % textN) == 0;

                            int x = static_cast<int>(m_options.resolution * s);
                            cr->move_to (x, -markerSize);
                            cr->line_to (x, 0);
                            cr->stroke();

                            if (drawText)
                            {
                                PangoLayout* layout = pango_cairo_create_layout(cr->cobj());
                                pango_layout_set_font_description(layout, m_fd);
                                pango_layout_set_text(layout, format("%is", s), -1);
                                PangoRectangle extents;
                                pango_layout_get_extents(layout, NULL, &extents);
                                double w = extents.width / PANGO_SCALE;
                                int tx = std::min (x - (w / 2.0), m_options.width - w);
                                int ty = - (GRID_MARKER_LARGE + GRID_MARKER_SMALL);
                                cr->move_to (tx, ty);
                                // revert inverted scale so that the text doesnt' get mirrored
                                cr->scale(1, -1);
                                pango_cairo_update_layout (cr->cobj(), layout);
                                cr->set_source_rgb(0.0, 0.0, 0.0);
                                pango_cairo_show_layout(cr->cobj(), layout);
                            }
                        }
                    }

                    // draw dB levels
                    double dbRange = 70;
                    // add 1 here since we offset 0.5 above and otherwise the bottom
                    // axis would end up off the edge of the image
                    cr->translate(0.0, -borderB + 1);
                    cr->set_line_width(1.0);

                    {
                        ContextGuard gLevelClip(cr);
                        cr->rectangle(0, 0, m_options.width, -dbHeight);
                        cr->clip();
                        {
                            ContextGuard gLevels(cr);
                            cr->scale(m_options.width / seconds, dbHeight / (dbRange));

                            cr->move_to(0, -dbRange);
                            for (std::map<double, double>::const_iterator it = m_levels.begin();
                                 it != m_levels.end(); ++it)
                            {
                                cr->line_to(it->first, it->second);
                            }
                            cr->line_to(m_levels.rbegin()->first, -dbRange);
                        }

                        Cairo::RefPtr<Cairo::LinearGradient> gradient = Cairo::LinearGradient::create(0.0, 0.0, 0.0, -dbRange);
                        gradient->add_color_stop_rgba(0.0, 0.5255, 0.1529, 0.0353, 0.7);
                        gradient->add_color_stop_rgba(0.2, 0.5255, 0.1529, 0.0353, 0.8);
                        gradient->add_color_stop_rgba(0.7, 0.5255, 0.1529, 0.0353, 1.0);
                        cr->set_source(gradient);
                        cr->fill_preserve();
                        cr->set_line_width(1.5);
                        cr->set_line_join(Cairo::LINE_JOIN_ROUND);
                        cr->set_source_rgb(0.3451, 0.1137, 0.051);
                        cr->stroke();
                    }

                    // draw axes for amplitude graph
                    cr->set_source_rgb(0.0, 0.0, 0.0);
                    cr->move_to(0, 0);
                    cr->set_line_width(1.0);
                    cr->line_to (0, -dbHeight);
                    cr->rel_line_to (m_options.width, 0);
                    cr->stroke();

                    // draw level (dB) axis markers
                    for (int l = 0; l >= -dbRange; l-=15)
                    {
                        ContextGuard gLevelAxis(cr);
                        bool drawText = false;
                        double markerSize = GRID_MARKER_SMALL;

                        if ((l % 30) == 0)
                        {
                            markerSize = GRID_MARKER_MED;
                            drawText = true;
                        }

                        int y = (l / dbRange) * dbHeight;

                        cr->move_to (-markerSize, y);
                        cr->rel_line_to (markerSize, 0);
                        cr->stroke();

                        if (drawText)
                        {
                            PangoLayout* layout = pango_cairo_create_layout(cr->cobj());
                            pango_layout_set_font_description(layout, m_fd);
                            pango_layout_set_text(layout, format("%idB", l), -1);
                            PangoRectangle extents;
                            pango_layout_get_extents(layout, NULL, &extents);
                            double w = extents.width / PANGO_SCALE;
                            double h = extents.height / PANGO_SCALE;
                            int tx = - (GRID_MARKER_MED + GRID_MARKER_SMALL) - w;
                            int ty = std::max(y + (h / 2.0), -dbHeight + h);
                            cr->move_to (tx, ty);

                            // revert inverted scale so that the text doesnt' get mirrored
                            cr->scale(1, -1);
                            pango_cairo_update_layout (cr->cobj(), layout);
                            pango_cairo_show_layout(cr->cobj(), layout);
                        }
                    }
                }

                cr->set_source(m_surface, borderL, 0);
                cr->paint();

            }
            else
            {
                // no grid, but the sono image is curently transparent, so paint it
                // over a solid background
                graph = Cairo::ImageSurface::create (Cairo::FORMAT_RGB24, m_options.width, m_options.height);
                Cairo::RefPtr<Cairo::Context> cr = Cairo::Context::create (graph);
                // clear to white
                cr->set_source_rgb (1.0, 1.0, 1.0);
                cr->paint ();

                // paint the sono image onto the new solid surface
                cr->set_source(m_surface, 0, 0);
                cr->paint();
            }

            graph->write_to_png (m_options.output_file);
        }
        catch(const std::exception& e)
        {
            g_warning("Unable to finish '%s': %s", m_options.output_file.c_str(), e.what());
            exit(1);
        }
    }

    static void on_element_message_proxy (GstBus *bus,
                                          GstMessage *message,
                                          gpointer user_data)
    {
        App *self = static_cast<App*>(user_data);
        const GstStructure *structure = gst_message_get_structure (message);
        if (gst_structure_has_name (structure, "spectrum"))
            self->on_spectrum (bus, structure);
        else if (gst_structure_has_name (structure, "level"))
            self->on_level (bus, structure);
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

    void paint_spectrum_at_offset(const GValue *val, int offset)
    {
        int size = gst_value_list_get_size (val);
        int i;

        if (m_options.height < size)
            size = m_options.height;

        // the inflection point between the two halves of the alpha formula
        const float TX = 0.6;
        const float TY = 0.85;
        // multiplier for the first segment
        const float k = (1 / TX) * (1 / TX) * TY;
        // slope and offset of the second segment
        static const float m = (1.0 - TY) / (1.0 - TX);
        static const float b = TY - m * TX;
        unsigned char *data = m_surface->get_data ();
        const int stride = m_surface->format_stride_for_width (Cairo::FORMAT_ARGB32, m_options.width);

        m_surface->flush ();
        for (i = 0; i < size; ++i)
        {
            const GValue *floatval = gst_value_list_get_value (val, i);
            float v = g_value_get_float (floatval);
            double shade = (v - m_options.noise_floor) / std::abs(m_options.noise_floor);
            if (shade > 0.0)
            {
                unsigned char *pixel = data + ((static_cast<int>(m_options.height) - 1 - i) * stride) +
                    offset * sizeof (guint32);
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
                unsigned int byte = (std::max (0.0, std::min (1.0, shade)) * 0xFF);

                memset (pixel, 0x0, sizeof (guint32));
                pixel[3] = byte;
            }
        }
        m_surface->mark_dirty ();
        ++m_sample_no;
    }

    void on_spectrum (GstBus *, const GstStructure *structure)
    {
        const GValue *vtimestamp = gst_structure_get_value (structure, "endtime");
        double seconds = static_cast<double>(g_value_get_uint64(vtimestamp)) / GST_SECOND;
        static int n = 0;
        if (!m_cr)
        {
            //g_debug("got 'spectrum' message before duration: %s", gst_structure_to_string(structure));
            return;
        }
        //g_debug("got spectrum message %i @ %g seconds", n++, seconds); 

        static int last_px = -1;
        int pixel_offset = (seconds  * m_options.resolution);
        if (pixel_offset >= m_options.width)
        {
            state_done();
            return;
        }
        if (pixel_offset == last_px)
        {
            //jitter probably caused the message to fall on the previous pixel
            //offset, so just draw it at the next one
            pixel_offset++;
        }

        if (pixel_offset - last_px > 1)
            g_debug("skipped pixels between %i and %i", last_px, pixel_offset);

        const GValue *val = gst_structure_get_value (structure, "magnitude");
        if (last_px != -1)
        {
            // paint columns that were missed due to jitter with the current
            // magnitude just to avoid blank spots in the spectrogram
            for (int i = last_px + 1; i < pixel_offset; i++)
            {
                paint_spectrum_at_offset(val, i);
            }
        }
        paint_spectrum_at_offset(val, pixel_offset);
        last_px = pixel_offset;
    }

    /* example data:
     * level, endtime=(guint64)9926530242, timestamp=(guint64)9874285344,
     * stream-time=(guint64)9874285344, running-time=(guint64)9874285344,
     * duration=(guint64)52244898, rms=(double){ -47.042540794668575,
     * -46.940355969821333 }, peak=(double){ -35.856320248518109,
     * -35.88928381611958 }, decay=(double){ -15.877065293851821,
     * -15.914867328075106 };
     */
    void on_level (GstBus *, const GstStructure *structure)
    {
        const GValue *vtimestamp = gst_structure_get_value (structure, "timestamp");
        double seconds = static_cast<double>(g_value_get_uint64(vtimestamp)) / GST_SECOND;
        const GValue *vrms = gst_structure_get_value(const_cast<GstStructure*>(structure), "rms");
        GValueArray *rms = reinterpret_cast<GValueArray*>(g_value_get_boxed(vrms));
        double max_channel = m_options.noise_floor;
        for (gsize i = 0; i < rms->n_values; ++i)
        {
            const GValue *floatval = g_value_array_get_nth(rms, i);
            max_channel = std::max(max_channel, g_value_get_double(floatval));
        }

        if (m_levels.empty())
        {
            m_peak_rms = max_channel;
            m_min_rms = max_channel;
        }
        else
        {
            m_peak_rms = std::max(max_channel, m_peak_rms);
            m_min_rms = std::min(max_channel, m_min_rms);
        }
        m_levels[seconds] = max_channel;
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
        m_prerolled = true;
        state_done();
        g_signal_handlers_disconnect_by_func (m_bus,
                                              (gpointer)on_async_done_proxy,
                                              this);
    }


private:
    Glib::RefPtr<Glib::MainLoop> m_mainloop;

    AppOptions m_options;
    AppState m_state;

    int m_sampling_rate;
    std::string m_fileuri;

    GstElement *m_pipeline;
    GstElement *m_decoder; // weak ref
    GstPad *m_decoder_pad;
    GstElement *m_convert; // weak ref
    GstElement *m_spectrum; // weak ref
    GstElement *m_filter; // weak ref
    GstElement *m_level; // weak ref
    GstElement *m_sink; // weak ref
    GstBus *m_bus;

    gint64 m_duration;
    double m_peak_rms;
    double m_min_rms;
    std::map<double, double> m_levels;
    Cairo::RefPtr<Cairo::ImageSurface> m_surface;
    Cairo::RefPtr<Cairo::Context> m_cr;

    int m_sample_no;
    bool m_prerolled;
    PangoFontDescription* m_fd;
};

int main (int argc, char** argv)
{
    Glib::init ();
#ifdef ENABLE_GIO
    Gio::init ();
#endif

    try
    {
        OptionContext octx;
        octx.parse (argc, argv);

        if (argc != 2)
        {
            g_print ("%s\n", octx.get_help().c_str ());
            std::exit (1);
        }

        int iterations = octx.m_option_group.m_options.benchmark;

        if (iterations > 0)
        {
            Glib::Timer timer;
            for (int i = 0; i < octx.m_option_group.m_options.benchmark; ++i)
            {
                App app (argv[1], octx.m_option_group.m_options);
                app.run();
                g_print (".");
            }
            double elapsed = timer.elapsed ();
            g_print ("\nTotal time elepased: %g\n", elapsed);
            g_print ("Mean iteration time: %g\n", elapsed / iterations);
        }
        else
        {
            App app (argv[1], octx.m_option_group.m_options);
            return app.run();
        }
    }
    catch (std::exception &e)
    {
        g_error ("%s", e.what ());
    }
    catch (Glib::Error &e)
    {
        g_error ("%s", e.what ().c_str ());
    }
    exit(1);
}
