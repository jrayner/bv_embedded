#! /bin/sh
# detect what type of machine we're running on
BUILD_SYSTEM_TYPE=`gcc -dumpmachine`
# this is the kind of machine we're cross-building for
# comment out whichever doesn't apply to you
# native build for this platform
HOST_SYSTEM_TYPE=`gcc -dumpmachine`
# cross build for ARM
#HOST_SYSTEM_TYPE=arm-linux-gnueabi
echo Configuring to build on $BUILD_SYSTEM_TYPE for $HOST_SYSTEM_TYPE
autoreconf -v --install || exit 1
#glib-gettextize --force --copy || exit 1
./configure --build=$BUILD_SYSTEM_TYPE --host=$HOST_SYSTEM_TYPE

