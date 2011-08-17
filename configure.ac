AC_INIT([spectrogram-thumbnailer],
        [1],
        [jonathon@quotidian.org],
        [spectrogram-thumbnailer])

AC_CONFIG_SRCDIR([src/spectrogram-thumbnailer.cc])
AM_INIT_AUTOMAKE([foreign -Wno-portability no-define])
m4_ifdef([AM_SILENT_RULES], [AM_SILENT_RULES([yes])])
AC_LANG(C++)
AC_PROG_CXX
PKG_CHECK_MODULES(DEPS, [
                         gstreamer-0.10
                         glibmm-2.4
                         cairomm-1.0
                         ])

AC_CONFIG_FILES([
                 Makefile
                 spectrogram-thumbnailer.thumbnailer
                 ])
AC_OUTPUT
