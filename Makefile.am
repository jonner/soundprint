AUTOMAKE_OPTIONS=subdir-objects
bin_PROGRAMS = spectrogram-thumbnailer

spectrogram_thumbnailer_SOURCES = src/spectrogram-thumbnailer.cc

spectrogram_thumbnailer_CXXFLAGS=@DEPS_CFLAGS@
spectrogram_thumbnailer_LDADD=@DEPS_LIBS@

thumbnailerdir = $(datadir)/thumbnailers
thumbnailer_DATA = spectrogram-thumbnailer.thumbnailer

EXTRA_DIST = spectrogram-thumbnailer.thumbnailer.in
