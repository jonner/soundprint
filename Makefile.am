AUTOMAKE_OPTIONS=subdir-objects
bin_PROGRAMS = soundprint

soundprint_SOURCES = src/soundprint.cc

soundprint_CXXFLAGS=@DEPS_CFLAGS@
soundprint_LDADD=@DEPS_LIBS@

thumbnailerdir = $(datadir)/thumbnailers
thumbnailer_DATA = soundprint.thumbnailer

EXTRA_DIST = soundprint.thumbnailer.in
