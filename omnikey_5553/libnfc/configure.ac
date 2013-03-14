# General init
AC_INIT(libnfc, 1.3.9, info@libnfc.org)

AC_CONFIG_MACRO_DIR([m4])

AC_CONFIG_HEADER(config.h)

# SVN revison
define([svn_revision], esyscmd([sh -c "svnversion -n"]))
SVN_REVISION=svn_revision
AC_DEFINE_UNQUOTED([SVN_REVISION], ["$SVN_REVISION"], [SVN revision])

AM_INIT_AUTOMAKE

m4_ifdef([AM_SILENT_RULES],[AM_SILENT_RULES([yes])])

AC_LANG_C
AC_PROG_CC
AC_PROG_MAKE_SET

# Libtool
AC_PROG_LIBTOOL
case "$host" in
   *-pc-linux-gnu)
     AC_MSG_RESULT([Fixing libtool for -rpath problems.])
     sed -i -r 's/(hardcode_into_libs)=.*$/\1=no/' libtool
   ;;
esac

AC_PATH_PROG(PKG_CONFIG, pkg-config, [AC_MSG_ERROR([pkg-config not found.])])

# Checks for header files.
AC_HEADER_STDC
AC_HEADER_STDBOOL
AC_CHECK_HEADERS([stdio.h stdlib.h stdint.h stddef.h stdbool.h])
AC_CHECK_FUNCS([strdup, usleep], [AC_DEFINE([_XOPEN_SOURCE], [600], [Enable POSIX extensions if present])])

AC_DEFINE(_NETBSD_SOURCE, 1, [Define on NetBSD to activate all library features])
AC_DEFINE(_DARWIN_C_SOURCE, 1, [Define on Darwin to activate all library features])

# Checks for types
AC_TYPE_SIZE_T
AC_TYPE_UINT8_T
AC_TYPE_UINT16_T
AC_TYPE_UINT32_T
AC_TYPE_UINT64_T
AC_TYPE_INT32_T

LIBNFC_CFLAGS='-I$(top_srcdir)/libnfc -I$(top_builddir)/include -I$(top_srcdir)/include'
AC_SUBST(LIBNFC_CFLAGS)

# Debug support (default:yes)
AC_ARG_ENABLE([debug],AS_HELP_STRING([--enable-debug],[Enable debug output]),[enable_debug=$enableval],[enable_debug="yes"])
 
AC_MSG_CHECKING(for debug flag)
AC_MSG_RESULT($enable_debug)
 
if test x"$enable_debug" = "xyes"
then
  CFLAGS="$CFLAGS -g -Wall -DDEBUG -pedantic -O0 -ggdb"
fi

# Dependencies
PKG_CONFIG_REQUIRES=""
AC_SUBST(PKG_CONFIG_REQUIRES)

# Defines and C flags
CFLAGS="$CFLAGS -std=c99"

# Workarounds for libusb in c99
CFLAGS="$CFLAGS -Du_int8_t=uint8_t -Du_int16_t=uint16_t"

AC_CONFIG_FILES([
		Makefile
		include/Makefile
		include/nfc/Makefile
		libnfc/Makefile
		cmake_modules/Makefile
		libnfc.pc
		])

AC_OUTPUT

LIBNFC_DRIVERS_SUMMARY
