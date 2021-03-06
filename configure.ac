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

AC_ARG_ENABLE([gio],
              [AS_HELP_STRING([--disable-gio],
                              [Disable gio functionality])],
              [],
              [enable_gio=yes])

PKG_CHECK_MODULES(SOUNDPRINT, [
                               gstreamer-1.0
                               glibmm-2.4
                               cairomm-1.0
                               ])

AS_IF([test "x$enable_gio" = "xyes"],
      [
       PKG_CHECK_MODULES(SONOGEN, [
                                   gstreamer-1.0
                                   glibmm-2.4
                                   cairomm-1.0
                                   pangocairo
                                   giomm-2.4
                                   ])
       AC_DEFINE([ENABLE_GIO], [1])
       ],
       [
        PKG_CHECK_MODULES(SONOGEN, [
                                    gstreamer-1.0
                                    glibmm-2.4
                                    cairomm-1.0
                                    pangocairo
                                    ])
        ])

AC_ARG_ENABLE([plugin],
              [AS_HELP_STRING([--enable-plugin],
                              [Enable gstreamer spectrogram visualization plugin])],
              [enable_plugin=yes],
              [])

AM_CONDITIONAL(BUILD_PLUGIN, [test "x$enable_plugin" = "xyes"])
AS_IF([test "x$enable_plugin" = "xyes"],
      [
       PKG_CHECK_MODULES(SPECTROGRAM, [
                                       gstreamer-1.0
                                       gstreamer-plugins-base-1.0
                                       ])
       ])

AC_CONFIG_FILES([
                 Makefile
                 soundprint.thumbnailer
                 ])
AC_OUTPUT
