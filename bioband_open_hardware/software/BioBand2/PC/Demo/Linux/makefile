
CC=g++
CFLAGS=-Wall -I. -I../../../Band/inc -I../../Api

# Define USBV as 1 if wish to use libusb 1.0
##USBV=1

ifeq ($(USBV),1)
demo: ../CmdLineSrc/demo.cpp ../../Api/band_if.cpp ../../../Band/src/shared.c
	$(CC) $(CFLAGS) ../CmdLineSrc/demo.cpp ../../Api/band_if.cpp  ../../../Band/src/shared.c -lusb-1.0 -DUSBLIB1 -o demo
else
demo: ../CmdLineSrc/demo.cpp ../../Api/band_if.cpp ../../../Band/src/shared.c
	$(CC) $(CFLAGS) ../CmdLineSrc/demo.cpp ../../Api/band_if.cpp  ../../../Band/src/shared.c -lusb -o demo
endif

clean:
	rm -f demo demo.o

