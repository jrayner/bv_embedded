NAME   = bioband

CC      = arm-none-eabi-gcc
LD      = arm-none-eabi-ld -v
AR      = arm-none-eabi-ar
AS      = arm-none-eabi-as
CP      = arm-none-eabi-objcopy
OD		= arm-none-eabi-objdump
  
CFLAGS  =  -I./ -I../inc -I../../../Libraries/STM32F10x_StdPeriph_Driver/inc -I../../../Libraries/CMSIS/Core/CM3 -I../../../Libraries/STM32_USB-FS-Device_Driver/inc -c -fno-common -O2 -g -mcpu=cortex-m3 -mthumb -DSTM32F10X_LD -DUSE_STDPERIPH_DRIVER -DUSE_STM3210B_EVAL -DHSE_VALUE=16000000 -DMRC_CWA
##CFLAGS  =  -I./ -I../inc -I../../../Libraries/STM32F10x_StdPeriph_Driver/inc -I../../../Libraries/CMSIS/Core/CM3 -I../../../Libraries/STM32_USB-FS-Device_Driver/inc -c -fno-common -O2 -g -mcpu=cortex-m3 -mthumb -DSTM32F10X_LD -DUSE_STDPERIPH_DRIVER -DUSE_STM3210B_EVAL -DHSE_VALUE=16000000 -DMRC_CWA -DCWA_USART_DEBUG
AFLAGS  = -ahls -mapcs-32
LFLAGS  = -Tlinker.cmd -nostartfiles
CPFLAGS = -Obinary
ODFLAGS	= -S

all: test

clean:
	-rm main.list main.out main.bin stm32f10x_rcc.o stm32f10x_gpio.o stm32f10x_spi.o stm32f10x_rtc.o stm32f10x_bkp.o stm32f10x_pwr.o stm32f10x_exti.o stm32f10x_adc.o stm32f10x_usart.o stm32f10x_tim.o misc.o system_stm32f10x.o core_cm3.o startup_stm32f10x_ld.o stm32f10x_it.o usb_core.o usb_init.o usb_int.o usb_mem.o usb_regs.o usb_sil.o hw_config.o usb_desc.o usb_endp.o usb_istr.o usb_prop.o usb_pwr.o nand_cwa.o accel.o tempsensor.o debug.o shared.o main.o 

test: main.out
	@ echo "...copying"
	$(CP) $(CPFLAGS) main.out main.bin
	$(OD) $(ODFLAGS) main.out > main.list

main.out: stm32f10x_rcc.o stm32f10x_gpio.o stm32f10x_spi.o stm32f10x_rtc.o stm32f10x_bkp.o stm32f10x_pwr.o stm32f10x_exti.o stm32f10x_adc.o stm32f10x_usart.o stm32f10x_tim.o misc.o system_stm32f10x.o core_cm3.o startup_stm32f10x_ld.o stm32f10x_it.o usb_core.o usb_init.o usb_int.o usb_mem.o usb_regs.o usb_sil.o  hw_config.o usb_desc.o usb_endp.o usb_istr.o usb_prop.o usb_pwr.o nand_cwa.o accel.o tempsensor.o debug.o shared.o main.o linker.cmd 
	$(LD) $(LFLAGS) -o main.out  stm32f10x_rcc.o stm32f10x_gpio.o stm32f10x_spi.o stm32f10x_rtc.o stm32f10x_bkp.o stm32f10x_pwr.o stm32f10x_exti.o stm32f10x_adc.o stm32f10x_usart.o stm32f10x_tim.o misc.o system_stm32f10x.o core_cm3.o startup_stm32f10x_ld.o stm32f10x_it.o usb_core.o usb_init.o usb_int.o usb_mem.o usb_regs.o usb_sil.o hw_config.o usb_desc.o usb_endp.o usb_istr.o usb_prop.o usb_pwr.o nand_cwa.o accel.o tempsensor.o debug.o shared.o main.o

stm32f10x_rcc.o: ../../../Libraries/STM32F10x_StdPeriph_Driver/src/stm32f10x_rcc.c 
	 $(CC) $(CFLAGS) ../../../Libraries/STM32F10x_StdPeriph_Driver/src/stm32f10x_rcc.c 
	 
stm32f10x_gpio.o: ../../../Libraries/STM32F10x_StdPeriph_Driver/src/stm32f10x_gpio.c
	 $(CC) $(CFLAGS) ../../../Libraries/STM32F10x_StdPeriph_Driver/src/stm32f10x_gpio.c 
	 
stm32f10x_spi.o: ../../../Libraries/STM32F10x_StdPeriph_Driver/src/stm32f10x_spi.c
	 $(CC) $(CFLAGS) ../../../Libraries/STM32F10x_StdPeriph_Driver/src/stm32f10x_spi.c 

stm32f10x_rtc.o: ../../../Libraries/STM32F10x_StdPeriph_Driver/src/stm32f10x_rtc.c
	 $(CC) $(CFLAGS) ../../../Libraries/STM32F10x_StdPeriph_Driver/src/stm32f10x_rtc.c 
	 
stm32f10x_bkp.o: ../../../Libraries/STM32F10x_StdPeriph_Driver/src/stm32f10x_bkp.c
	 $(CC) $(CFLAGS) ../../../Libraries/STM32F10x_StdPeriph_Driver/src/stm32f10x_bkp.c 
	 
stm32f10x_pwr.o: ../../../Libraries/STM32F10x_StdPeriph_Driver/src/stm32f10x_pwr.c
	 $(CC) $(CFLAGS) ../../../Libraries/STM32F10x_StdPeriph_Driver/src/stm32f10x_pwr.c 
	 
stm32f10x_exti.o: ../../../Libraries/STM32F10x_StdPeriph_Driver/src/stm32f10x_exti.c
	 $(CC) $(CFLAGS) ../../../Libraries/STM32F10x_StdPeriph_Driver/src/stm32f10x_exti.c 
	 
stm32f10x_adc.o: ../../../Libraries/STM32F10x_StdPeriph_Driver/src/stm32f10x_adc.c
	 $(CC) $(CFLAGS) ../../../Libraries/STM32F10x_StdPeriph_Driver/src/stm32f10x_adc.c 
	 
stm32f10x_usart.o: ../../../Libraries/STM32F10x_StdPeriph_Driver/src/stm32f10x_usart.c
	 $(CC) $(CFLAGS) ../../../Libraries/STM32F10x_StdPeriph_Driver/src/stm32f10x_usart.c 
	 
stm32f10x_tim.o: ../../../Libraries/STM32F10x_StdPeriph_Driver/src/stm32f10x_tim.c
	 $(CC) $(CFLAGS) ../../../Libraries/STM32F10x_StdPeriph_Driver/src/stm32f10x_tim.c 
	 
misc.o: ../../../Libraries/STM32F10x_StdPeriph_Driver/src/misc.c
	 $(CC) $(CFLAGS) ../../../Libraries/STM32F10x_StdPeriph_Driver/src/misc.c 
	 
system_stm32f10x.o: ../../../Libraries/CMSIS/Core/CM3/system_stm32f10x.c
	 $(CC) $(CFLAGS) ../../../Libraries/CMSIS/Core/CM3/system_stm32f10x.c 
	 
core_cm3.o: ../../../Libraries/CMSIS/Core/CM3/core_cm3.c
	 $(CC) $(CFLAGS) ../../../Libraries/CMSIS/Core/CM3/core_cm3.c 
	 
startup_stm32f10x_ld.o: ../../../Libraries/CMSIS/Core/CM3/startup/gcc/startup_stm32f10x_ld.s
	 $(CC) $(CFLAGS) ../../../Libraries/CMSIS/Core/CM3/startup/gcc/startup_stm32f10x_ld.s 
	 

usb_core.o: ../../../Libraries/STM32_USB-FS-Device_Driver/src/usb_core.c
	 $(CC) $(CFLAGS) ../../../Libraries/STM32_USB-FS-Device_Driver/src/usb_core.c
	 
usb_init.o: ../../../Libraries/STM32_USB-FS-Device_Driver/src/usb_init.c
	 $(CC) $(CFLAGS) ../../../Libraries/STM32_USB-FS-Device_Driver/src/usb_init.c
	 
usb_int.o: ../../../Libraries/STM32_USB-FS-Device_Driver/src/usb_int.c
	 $(CC) $(CFLAGS) ../../../Libraries/STM32_USB-FS-Device_Driver/src/usb_int.c
	 
usb_mem.o: ../../../Libraries/STM32_USB-FS-Device_Driver/src/usb_mem.c
	 $(CC) $(CFLAGS) ../../../Libraries/STM32_USB-FS-Device_Driver/src/usb_mem.c
	 
usb_regs.o: ../../../Libraries/STM32_USB-FS-Device_Driver/src/usb_regs.c
	 $(CC) $(CFLAGS) ../../../Libraries/STM32_USB-FS-Device_Driver/src/usb_regs.c

usb_sil.o: ../../../Libraries/STM32_USB-FS-Device_Driver/src/usb_sil.c
	 $(CC) $(CFLAGS) ../../../Libraries/STM32_USB-FS-Device_Driver/src/usb_sil.c

hw_config.o: ../src/hw_config.c
	 $(CC) $(CFLAGS) ../src/hw_config.c
	 
usb_desc.o: ../src/usb_desc.c
	 $(CC) $(CFLAGS) ../src/usb_desc.c
	 
usb_endp.o: ../src/usb_endp.c
	 $(CC) $(CFLAGS) ../src/usb_endp.c
	 
usb_istr.o: ../src/usb_istr.c
	 $(CC) $(CFLAGS) ../src/usb_istr.c
	 
usb_prop.o: ../src/usb_prop.c
	 $(CC) $(CFLAGS) ../src/usb_prop.c
	 
usb_pwr.o: ../src/usb_pwr.c
	 $(CC) $(CFLAGS) ../src/usb_pwr.c
	 
stm32f10x_it.o: ../src/stm32f10x_it.c
	 $(CC) $(CFLAGS) ../src/stm32f10x_it.c 
	 
nand_cwa.o: ../src/nand_cwa.c
	 $(CC) $(CFLAGS) ../src/nand_cwa.c

accel.o: ../src/accel.c
	 $(CC) $(CFLAGS) ../src/accel.c

tempsensor.o: ../src/tempsensor.c
	 $(CC) $(CFLAGS) ../src/tempsensor.c

debug.o: ../src/debug.c
	 $(CC) $(CFLAGS) ../src/debug.c

shared.o: ../src/shared.c
	 $(CC) $(CFLAGS) ../src/shared.c

main.o: ../src/main.c
	 $(CC) $(CFLAGS) ../src/main.c
	 
flash: all
	 do_flash.pl $(CURDIR)/main.bin
