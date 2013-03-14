SUBDIRS = .

INCLUDES = $(all_includes) -I$(top_srcdir)/libnfc -I$(top_builddir)/include -I$(top_srcdir)/include

lib_LTLIBRARIES = libnfc.la
libnfc_la_SOURCES = nfc.c iso14443-subr.c
libnfc_la_LDFLAGS = -no-undefined -version-info=0:0:0

EXTRA_DIST = CMakeLists.txt
