
CC=g++
CFLAGS=-Wall -I. -I../../../Band/inc -I../../Api

# Define USBV as 1 if wish to use libusb 1.0
##USBV=1

ifeq ($(USBV),1)
mrc: ../CmdLineSrc/mrc.cpp ../../Api/band_if.cpp ../../../Band/src/shared.c
	$(CC) $(CFLAGS) ../CmdLineSrc/mrc.cpp ../../Api/band_if.cpp  ../../../Band/src/shared.c -lusb-1.0 -DUSBLIB1 -o mrc
else
mrc: ../CmdLineSrc/mrc.cpp ../../Api/band_if.cpp ../../../Band/src/shared.c
	$(CC) $(CFLAGS) ../CmdLineSrc/mrc.cpp ../../Api/band_if.cpp  ../../../Band/src/shared.c -lusb -o mrc
endif

clean:
	rm -f mrc mrc.o

