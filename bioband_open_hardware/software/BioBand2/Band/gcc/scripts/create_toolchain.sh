#!/bin/sh

## Remember to install following first ..
## sudo apt-get install build-essential flex bison libgmp3-dev libmpfr-dev autoconf 
## sudo apt-get install texinfo libncurses5-dev libexpat1 libexpat1-dev
##

sudo mkdir /usr/local/cross-cortex-m3

## The following $USER should be your username
## This ensures don't need sudo for install and appears to solve env issues
sudo chown $USER:$USER /usr/local/cross-cortex-m3


echo "Building stm32 toolchain .."

export TOOLPATH=/usr/local/cross-cortex-m3
export PATH=${TOOLPATH}/bin:$PATH

mkdir ~/stm32
cd ~/stm32

wget http://ftp.gnu.org/gnu/binutils/binutils-2.19.tar.bz2
tar -xvjf binutils-2.19.tar.bz2

wget http://fun-tech.se/stm32/gcc/binutils-2.19_tc-arm.c.patch
patch binutils-2.19/gas/config/tc-arm.c binutils-2.19_tc-arm.c.patch

cd binutils-2.19
mkdir build
cd build
../configure --target=arm-none-eabi  \
             --prefix=$TOOLPATH  \
             --enable-interwork  \
             --enable-multilib  \
             --with-gnu-as  \
             --with-gnu-ld  \
             --disable-nls
make -j 2
make install

cd ~/stm32
wget ftp://ftp.sunet.se/pub/gnu/gcc/releases/gcc-4.3.4/gcc-4.3.4.tar.bz2
tar -xvjf gcc-4.3.4.tar.bz2
cd gcc-4.3.4
mkdir build
cd build
../configure --target=arm-none-eabi  \
             --prefix=$TOOLPATH  \
             --enable-interwork  \
             --enable-multilib  \
             --enable-languages="c,c++"  \
             --with-newlib  \
             --without-headers  \
             --disable-shared  \
             --with-gnu-as  \
             --with-gnu-ld
make -j 2 all-gcc
make install-gcc

cd ~/stm32
wget ftp://sources.redhat.com/pub/newlib/newlib-1.17.0.tar.gz
tar -xvzf newlib-1.17.0.tar.gz

# We need to apply a updated version of this patch.  
# http://www.esden.net/content/embedded/newlib-1.14.0-missing-makeinfo.patch

wget http://fun-tech.se/stm32/gcc/newlib-1.17.0-missing-makeinfo.patch
patch newlib-1.17.0/configure newlib-1.17.0-missing-makeinfo.patch

cd newlib-1.17.0
mkdir build
cd build
../configure --target=arm-none-eabi  \
             --prefix=$TOOLPATH  \
             --enable-interwork  \
             --disable-newlib-supplied-syscalls  \
             --with-gnu-ld  \
             --with-gnu-as  \
             --disable-shared

make -j 2 CFLAGS_FOR_TARGET="-ffunction-sections \
                        -fdata-sections \
                        -DPREFER_SIZE_OVER_SPEED \
                        -D__OPTIMIZE_SIZE__ \
                        -Os \
                        -fomit-frame-pointer \
                        -mcpu=cortex-m3 \
                        -mthumb \
                        -D__thumb2__ \
                        -D__BUFSIZ__=256" \
                        CCASFLAGS="-mcpu=cortex-m3 -mthumb -D__thumb2__"
make install


cd ~/stm32
cd gcc-4.3.4/build
make -j 2 CFLAGS="-mcpu=cortex-m3 -mthumb" \
     CXXFLAGS="-mcpu=cortex-m3 -mthumb" \
     LIBCXXFLAGS="-mcpu=cortex-m3 -mthumb" \
     all
make install

cd ~/stm32
wget http://ftp.gnu.org/gnu/gdb/gdb-7.0.tar.bz2
tar -xvjf gdb-7.0.tar.bz2
cd gdb-7.0
mkdir build
cd build
../configure --target=arm-none-eabi \
                      --prefix=$TOOLPATH  \
                      --enable-languages=c,c++ \
                      --enable-thumb \
                      --enable-interwork \
                      --enable-multilib \
                      --enable-tui \
                      --with-newlib \
                      --disable-werror \
                      --disable-libada \
                      --disable-libssp
make -j 2
make install

echo "Toolchain done"
