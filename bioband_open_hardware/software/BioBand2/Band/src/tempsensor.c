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
 * Functions for accessing LM95071 temp sensor
 * These assume that the SPI interface has been initialised correctly
 * by calling SPI_Configuration_TempSensor() software control of nCS
 */

#include "tempsensor.h"

#define LM95071_CMD_SHUTDOWN 0x00ff
#define LM95071_CMD_CONTINUOUS_CONVERSION 0x0000
#define LM95071_ID 0x800f

#define CYCLE_WAIT_TIME 10

static inline void tempAssertnCS() {
#if CWA_VERSION == 11
	GPIO_WriteBit(GPIOB,GPIO_Pin_0, Bit_RESET);
#endif // CWA_VERSION == 11
#if CWA_VERSION == 12
	GPIO_WriteBit(GPIOA,GPIO_Pin_3, Bit_RESET);
#endif // CWA_VERSION == 12
}

static inline void tempDeassertnCS() {
#if CWA_VERSION == 11
	GPIO_WriteBit(GPIOB,GPIO_Pin_0, Bit_SET);
#endif // CWA_VERSION == 11
#if CWA_VERSION == 12
	GPIO_WriteBit(GPIOA,GPIO_Pin_3, Bit_SET);
#endif // CWA_VERSION == 12
}

static uint16_t tempSpiCycle(uint16_t data) {
	volatile uint16_t value;
	// Put SPI port into receive mode, this starts SCK running continuously
	SPI_BiDirectionalLineConfig(SPI1,SPI_Direction_Rx);
	SPI_I2S_SendData(SPI1,data);
	// Wait for Rx flag to be set
	while (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_RXNE) == RESET);
	// Stop the clock! Put SPI port into transmit mode
	SPI_BiDirectionalLineConfig(SPI1,SPI_Direction_Tx);
	// Read data
	value=SPI_I2S_ReceiveData(SPI1);
	// Arbitrary delay because there's no way of telling when the SPI has
	// finished
	TIM_Cmd(TIM2,DISABLE);
	TIM_SetCounter(TIM2,CYCLE_WAIT_TIME);
	TIM_ClearFlag(TIM2,TIM_FLAG_Update);
	TIM_Cmd(TIM2,ENABLE);
	while(!TIM_GetFlagStatus(TIM2,TIM_FLAG_Update));
	return value;
}

uint8_t tempCheckWhoAmI() {
	volatile uint16_t value;
	
	tempAssertnCS();
	// Send shutdown command
	tempSpiCycle(LM95071_CMD_SHUTDOWN);
	tempDeassertnCS();
	tempAssertnCS();
	// Now read ID
	value=tempSpiCycle(LM95071_CMD_SHUTDOWN);
	tempDeassertnCS();
	// Check it
	return(LM95071_ID == value);
}

void tempEnable() {
	volatile uint16_t value;

	tempAssertnCS();
	// Send continuous conversion command
	tempSpiCycle(LM95071_CMD_CONTINUOUS_CONVERSION);
	tempDeassertnCS();
}

void tempDisable() {
	int n;
	
	// Try 10 times to shut down temp sensor
	// our battery life may depend on it
	for(n=0; n<10; n++) {
		if(tempCheckWhoAmI())
			break;
	}
}

uint16_t tempReading() {
	uint16_t value;
	tempAssertnCS();
	// Read data
	value=tempSpiCycle(LM95071_CMD_CONTINUOUS_CONVERSION);
	tempDeassertnCS();
	return value;
}

// EOF
