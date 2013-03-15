/*
 * Copyright (c) 2011, Medical Research Council
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 
 *    * Redistributions of source code must retain the above copyright notice,
 * 		this list of conditions and the following disclaimer.
 *    * Redistributions in binary form must reproduce the above copyright
 * 		notice, this list of conditions and the following disclaimer in the
 * 		documentation and/or other materials provided with the distribution.
 *    * Neither the name of the MRC Epidemiology Unit nor the names of its
 * 		contributors may be used to endorse or promote products derived from
 * 		this software without specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 * 
 * 
 * Functions for accessing LIS331DLH accelerometer
 * These assume that the SPI interface has been initialised correctly
 * for 8-bit transfers, software control of NSS
 */

#include "accel.h"
#include "shared.h"

#define LIS331DLH_REG_WHO_AM_I 0x0f
#define LIS331DLH_REG_CTRL_REG1 0x20
#define LIS331DLH_REG_CTRL_REG2 0x21
#define LIS331DLH_REG_CTRL_REG3 0x22
#define LIS331DLH_REG_CTRL_REG4 0x23
#define LIS331DLH_REG_CTRL_REG5 0x24
#define LIS331DLH_REG_HP_FILTER_RESET 0x25
#define LIS331DLH_REG_REFERENCE 0x26
#define LIS331DLH_REG_STATUS_REG 0x27
#define LIS331DLH_REG_OUT_X_L 0x28
#define LIS331DLH_REG_OUT_X_H 0x29
#define LIS331DLH_REG_OUT_Y_L 0x2a
#define LIS331DLH_REG_OUT_Y_H 0x2b
#define LIS331DLH_REG_OUT_Z_L 0x2c
#define LIS331DLH_REG_OUT_Z_H 0x2d
#define LIS331DLH_REG_INT1_CFG 0x30
#define LIS331DLH_REG_INT1_SOURCE 0x31
#define LIS331DLH_REG_INT1_THS 0x32
#define LIS331DLH_REG_INT1_DURATION 0x33
#define LIS331DLH_REG_INT2_CFG 0x34
#define LIS331DLH_REG_INT2_SOURCE 0x35
#define LIS331DLH_REG_INT2_THS 0x36
#define LIS331DLH_REG_INT2_DURATION 0x37

#define LIS331DLH_SPI_WRITE 0x00
#define LIS331DLH_SPI_READ 0x80
#define LIS331DLH_AUTOINCREMENT 0x40

#define LIS331DLH_WHO_AM_I_VALUE 0x32

#define LIS331DLH_CR1_XYZ_ENABLE 0x07
#define LIS331DLH_CR1_DR_50 0x00
#define LIS331DLH_CR1_DR_100 0x08
#define LIS331DLH_CR1_DR_400 0x10
#define LIS331DLH_CR1_DR_1000 0x18
#define LIS331DLH_CR1_PM_POWERDOWN 0x00
#define LIS331DLH_CR1_PM_NORMAL 0x20
#define LIS331DLH_CR1_PM_LOW_05 0x40
#define LIS331DLH_CR1_PM_LOW_1 0x60
#define LIS331DLH_CR1_PM_LOW_2 0x80
#define LIS331DLH_CR1_PM_LOW_5 0xa0
#define LIS331DLH_CR1_PM_LOW_10 0xc0

#define LIS331DLH_CR3_I1_CFG_DATAREADY 0x02

#define LIS331DLH_CR4_BDU 0x80
#define LIS331DLH_CR4_BIG_ENDIAN 0x40
#define LIS331DLH_CR4_FS_2G 0x00
#define LIS331DLH_CR4_FS_4G 0x10
#define LIS331DLH_CR4_FS_8G 0x30

uint8_t accel_rate_and_g_scale = CWA_8G | CWA_50HZ;
volatile accel_data_rate current_accel_rate = CWA_50HZ;

static uint8_t getAccelRate() {
	uint8_t ar = accel_rate_and_g_scale & 0x0F;
	uint8_t retval;
	current_accel_rate = ar;
	switch (current_accel_rate) {
		case CWA_100HZ: retval = LIS331DLH_CR1_DR_100; break;
		case CWA_400HZ: retval = LIS331DLH_CR1_DR_400; break;
		case CWA_1000HZ: retval = LIS331DLH_CR1_DR_1000; break;
		default: retval = LIS331DLH_CR1_DR_50; break;
	}
	return retval;
}

static uint8_t getGScale() {
	uint8_t gs = accel_rate_and_g_scale & 0xF0;
	uint8_t retval;
	switch (gs) {
		case CWA_2G: retval = LIS331DLH_CR4_FS_2G; break;
		case CWA_4G: retval = LIS331DLH_CR4_FS_4G; break;
		default: retval = LIS331DLH_CR4_FS_8G; break;
	}
	return retval;
}

static inline void accelAssertNSS() {
	GPIO_WriteBit(GPIOA,GPIO_Pin_4, Bit_RESET);
}

static inline void accelDeassertNSS() {
	GPIO_WriteBit(GPIOA,GPIO_Pin_4, Bit_SET);
}

static inline uint8_t accelSpiCycle(uint8_t data) {
	volatile uint8_t dummy;
	// First, dummy read to clear Rx flag
	dummy=SPI_I2S_ReceiveData(SPI1);
	// Wait till transmit is ready
	while (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_TXE) == RESET);
	// Send data
	SPI_I2S_SendData(SPI1,data);
	// Wait till receive is ready
	while (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_RXNE) == RESET);
	// Retrieve data
	return SPI_I2S_ReceiveData(SPI1);
}

uint8_t accelCheckWhoAmI() {
	uint8_t value;
	
	accelAssertNSS();
	// Send address
	accelSpiCycle(LIS331DLH_REG_WHO_AM_I | LIS331DLH_SPI_READ);
	// Read data
	value=accelSpiCycle(0);
	accelDeassertNSS();
	// Check it
	return(LIS331DLH_WHO_AM_I_VALUE == value);
	
}

void accelEnable() {
	accelAssertNSS();
	// Send address
	accelSpiCycle(LIS331DLH_REG_CTRL_REG1);
	// Send config value	
	accelSpiCycle(LIS331DLH_CR1_XYZ_ENABLE | getAccelRate() |
		LIS331DLH_CR1_PM_NORMAL);
	accelDeassertNSS();
	// Enable data ready interrupt output
	accelAssertNSS();
	// Send address
	accelSpiCycle(LIS331DLH_REG_CTRL_REG3);
	// Send config value	
	accelSpiCycle(LIS331DLH_CR3_I1_CFG_DATAREADY);
	accelDeassertNSS();
	// Now configure for high-byte first output
	accelAssertNSS();
	// Send address
	accelSpiCycle(LIS331DLH_REG_CTRL_REG4);
	// Send config value	
	accelSpiCycle(LIS331DLH_CR4_BDU | LIS331DLH_CR4_BIG_ENDIAN | getGScale());
	accelDeassertNSS();
}

void accelDisable() {
	uint8_t value;
	int n;
	
	// Try 10 times to shutdown to make sure battery doesn't get drained	
	for(n=0;n<10;n++)
	{
		accelAssertNSS();
		// Send address
		accelSpiCycle(LIS331DLH_REG_CTRL_REG1);
		// Send config value	
		accelSpiCycle(LIS331DLH_CR1_PM_POWERDOWN);
		accelDeassertNSS();
		// Now try to read back the bits
		accelAssertNSS();
		accelSpiCycle(LIS331DLH_REG_CTRL_REG1 | LIS331DLH_SPI_READ);
		value=accelSpiCycle(0);
		accelDeassertNSS();
		if((value&0xe0)==0)
			break;
	}
#if 0
	if (value&0xe0) {
		writeStr("!aD");
	}
#endif
}

void accelReading(uint16_t *x, uint16_t *y, uint16_t *z) {
	accelAssertNSS();
	// Send address with autoincrement set
	accelSpiCycle(LIS331DLH_REG_OUT_X_L | LIS331DLH_SPI_READ |
		LIS331DLH_AUTOINCREMENT);
	// Read 6 bytes of data and shuffle them into the right variables
	*x=accelSpiCycle(0);
	*x<<=8;
	*x+=accelSpiCycle(0);
	*y=accelSpiCycle(0);
	*y<<=8;
	*y+=accelSpiCycle(0);
	*z=accelSpiCycle(0);
	*z<<=8;
	*z+=accelSpiCycle(0);
	accelDeassertNSS();
}

void accelReadingBuf(uint8_t *buf) {
	int n;
	accelAssertNSS();
	// Send address with autoincrement set
	accelSpiCycle(LIS331DLH_REG_OUT_X_L | LIS331DLH_SPI_READ |
		LIS331DLH_AUTOINCREMENT);
	// Read 6 bytes of data and shuffle them into the right variables
	for(n=0;n<6;n++) {
		*buf++=accelSpiCycle(0);
	}
	accelDeassertNSS();
}

// EOF
