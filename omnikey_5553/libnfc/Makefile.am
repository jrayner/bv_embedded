ACLOCAL_AMFLAGS = -I m4

AM_CFLAGS = $(LIBNFC_CFLAGS)

SUBDIRS = libnfc include cmake_modules

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = libnfc.pc

EXTRA_DIST = Doxyfile pn53x.conf pn53x.rules CMakeLists.txt config.h.cmake LICENSE
CLEANFILES = Doxygen.log

