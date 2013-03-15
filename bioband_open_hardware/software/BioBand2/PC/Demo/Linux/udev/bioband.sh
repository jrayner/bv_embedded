#!/bin/bash

## An example shell script, showing how to launch the demo program upon a new
## Bioband being added to Linux sysfs, see the udev rule (12-bioband.rules)

## This has been tested on Ubuntu 10.04. It should work on other flavours of
## Linux but these have not been tested.

## Adjust directory names as required.

if [ -f $1 ]
then
  exit 0
fi

export DISPLAY=:0.0

xterm -geometry 90x40+0+0 -e "/data/projects/mrc/svn/mrc/software/trunk/dev_board/BioBand2/PC/Demo/Linux/demo -snum $1 -downdir /data/projects/mrc/test_raw_files -autodown -centre uk" &

exit 0
