AUTOMAKE_OPTIONS=subdir-objects

# thumbnailer program
bin_PROGRAMS = soundprint sonogen

soundprint_SOURCES = src/soundprint.cc

soundprint_CXXFLAGS=@SOUNDPRINT_CFLAGS@
soundprint_LDADD=@SOUNDPRINT_LIBS@

sonogen_SOURCES = src/sonogen.cc

sonogen_CXXFLAGS=@SONOGEN_CFLAGS@
sonogen_LDADD=@SONOGEN_LIBS@

thumbnailerdir = $(datadir)/thumbnailers
thumbnailer_DATA = soundprint.thumbnailer

if BUILD_PLUGIN
# gstreamer visualization plugin
plugin_LTLIBRARIES = libgstspectrogram.la
plugindir = $(libdir)/gstreamer-0.10/
libgstspectrogram_la_SOURCES = gst/gstspectrogram.c
libgstspectrogram_la_CFLAGS = @SPECTROGRAM_CFLAGS@
libgstspectrogram_la_LIBADD = @SPECTROGRAM_LIBS@ -lgstaudio-0.10 -lgstfft-0.10 -lm
libgstspectrogram_la_LIBTOOLFLAGS = --tag=disable-static

noinst_HEADERS = gst/gstspectrogram.h
endif

EXTRA_DIST = soundprint.thumbnailer.in
