AC_INIT([soundprint],
        [1],
        [jonathon@quotidian.org],
        [soundprint])

AC_CONFIG_SRCDIR([src/soundprint.cc])
AM_INIT_AUTOMAKE([foreign -Wno-portability])
m4_ifdef([AM_SILENT_RULES], [AM_SILENT_RULES([yes])])
AC_LANG(C++)
AC_PROG_CXX
AC_PROG_LIBTOOL
PKG_CHECK_MODULES(SOUNDPRINT, [
                               gstreamer-0.10
                               glibmm-2.4
                               cairomm-1.0
                               ])

PKG_CHECK_MODULES(SONOGEN, [
                               gstreamer-0.10
                               glibmm-2.4
                               cairomm-1.0
                               pangocairo
                               giomm-2.4
                               ])

PKG_CHECK_MODULES(SPECTROGRAM, [
                                gstreamer-0.10
                                gstreamer-plugins-base-0.10
                                ])

AC_CONFIG_FILES([
                 Makefile
                 soundprint.thumbnailer
                 ])
AC_OUTPUT
