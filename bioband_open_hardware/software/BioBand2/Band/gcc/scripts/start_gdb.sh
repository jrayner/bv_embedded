#!/bin/sh

## Remember you need /usr/local/cross-cortex-m3 in your path & openocd running

arm-none-eabi-gdbtui --eval-command="target remote localhost:3333" main.out
