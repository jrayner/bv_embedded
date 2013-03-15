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
 */

#ifndef __HW_CONFIG_H
#define __HW_CONFIG_H

#include "usb_type.h"

// version of hardware
// 1.0 has no temperature sensor, inverted blue LED
//#define CWA_VERSION 10
// 1.1 has temp sensor with NCS on NAND_NCE, blue LED same as others
//#define CWA_VERSION 11
// 1.2 has GPIO3 used for TEMP_NCS, blue LED on GPIO9, green LED on GPIO8 but
// open-drain or'ed with NCHG
#define CWA_VERSION 12

#if CWA_VERSION == 12
#define ENABLE_BATTERY_LEVEL
#define ENABLE_TEMPERATURE
#undef CWA_USART_DEBUG  // serial debug does not work on v1.2 bands
#else
#define ENABLE_BATTERY_LEVEL
#endif

// NAND GPIOs
#define NAND_CONTROL_PORT GPIOB
#define NAND_RCC_CONTROL_PORT RCC_APB2Periph_GPIOB
#define NAND_NCE GPIO_Pin_0
#define NAND_NWE GPIO_Pin_1
#define NAND_NRE GPIO_Pin_2
#define NAND_CLE GPIO_Pin_5
#define NAND_ALE GPIO_Pin_6
#define NAND_RNB GPIO_Pin_7

#define NAND_DATA_PORT GPIOB
#define NAND_RCC_DATA_PORT RCC_APB2Periph_GPIOB
#define NAND_D0 GPIO_Pin_8
#define NAND_D1 GPIO_Pin_9
#define NAND_D2 GPIO_Pin_10
#define NAND_D3 GPIO_Pin_11
#define NAND_D4 GPIO_Pin_12
#define NAND_D5 GPIO_Pin_13
#define NAND_D6 GPIO_Pin_14
#define NAND_D7 GPIO_Pin_15

#define RTC_TICKS_PER_SECOND 32767

void setUpClocks();
void Set_System(void);
void GPIO_Configuration(void);
void SPI_Configuration_Accelerometer(int fast_clock);
void SPI_Configuration_TempSensor(void);
void RTC_Configuration(void);
void NVIC_Configuration(void);

void  ADC1_Configuration();
uint16_t getADC1Channel();

void saveValue(uint16_t bkp_reg, uint32_t data);
uint32_t restoreValue(uint16_t bkp_reg);

void Set_USBClock(void);
void Enter_LowPowerMode(void);
void Leave_LowPowerMode(void);
void USB_Interrupts_Config(void);
void USB_Interrupts_Disable(void);
void USB_Cable_Config (FunctionalState NewState);
void USB_ConfigSerialNum(uint8_t* tag_id_ptr);

void USART_Configuration1(void);
int SendCharUart1(int ch);  
int GetKeyUart1(void); 
void SendHex8Uart1(uint8_t n);
void SendHex16Uart1(uint16_t n);
void SendHex32Uart1(uint32_t n);

void BlueLed(bool state);
void GreenLed(bool state);
void RedLed(bool state);

void gpioMinimumPower();
void gpioEnableLeds();
void gpioEnableSpi();
void gpioEnableJTAG();

void ready_for_tx_cb();

void TIM2_Configuration(void);

#endif  // __HW_CONFIG_H
