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
 */

#include "stm32f10x.h"
#include "stm32f10x_it.h"
#include "usb_lib.h"
#include "usb_prop.h"
#include "usb_desc.h"
#include "hw_config.h"
#include "usb_pwr.h"
#include "shared.h"

ErrorStatus HSEStartUpStatus;

#define ENABLE_WAKEUP

void setUpClocks(void)
{
	// Enable HSE
	RCC_HSEConfig(RCC_HSE_ON);

	// Wait till HSE is ready
	HSEStartUpStatus = RCC_WaitForHSEStartUp();

	if(HSEStartUpStatus == SUCCESS) {
		
		// Enable PLL 
		RCC_PLLCmd(ENABLE);

		// Wait till PLL is ready
		while(RCC_GetFlagStatus(RCC_FLAG_PLLRDY) == RESET) {
		}

		// Select PLL as system clock source
		RCC_SYSCLKConfig(RCC_SYSCLKSource_PLLCLK);

		// Wait till PLL is used as system clock source
		while(RCC_GetSYSCLKSource() != 0x08) {
		}
	}
}

void clocksNormal() {
#ifdef CWA_USART_DEBUG
	RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOB |
		RCC_APB2Periph_GPIOC | RCC_APB2Periph_USART1 | RCC_APB2Periph_SPI1 |
		RCC_APB2Periph_AFIO, ENABLE);
#else
	RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOB |
		RCC_APB2Periph_GPIOC | RCC_APB2Periph_SPI1 | RCC_APB2Periph_AFIO,
		ENABLE);
#endif

	// Enable PWR and BKP clock
	RCC_APB1PeriphClockCmd(RCC_APB1Periph_PWR | RCC_APB1Periph_BKP, ENABLE);
}

void Set_System(void)
{
	/* Setup the microcontroller system. Initialize the Embedded Flash
	 * Interface, initialize the PLL and update the SystemFrequency variable. */
	SystemInit();

	clocksNormal();
	
	// Enable write access to Backup domain
	PWR_BackupAccessCmd(ENABLE);
	
	// ensure nothing being output on Tamper pin / PC13
	BKP_RTCOutputConfig(BKP_RTCOutputSource_None);

    // Disable Tamper pin
    BKP_TamperPinCmd(DISABLE);

	// Clear Tamper pin Event(TE) pending flag
	BKP_ClearFlag();

    // Disable Tamper interrupt
    BKP_ITConfig(DISABLE);

}

void clocksMinimumPower() {
#ifdef CWA_USART_DEBUG
	RCC_APB2PeriphClockCmd(RCC_APB2Periph_USART1, DISABLE);
#endif
}

void BlueLed(bool state)
{
#if CWA_VERSION == 10
	// In the first version, Blue is opposite due to charge pump arch
	GPIO_WriteBit(GPIOA,GPIO_Pin_8, state?Bit_SET:Bit_RESET);
#endif
#if CWA_VERSION == 11
	GPIO_WriteBit(GPIOA,GPIO_Pin_8, state?Bit_RESET:Bit_SET);
#endif
#if CWA_VERSION == 12
#ifdef CWA_USART_DEBUG
	/* With v1.2 & usart configured, the blue led would show serial tx (if
	 * serial worked) */
#else
	GPIO_WriteBit(GPIOA,GPIO_Pin_9, state?Bit_RESET:Bit_SET);
#endif
#endif
}

void GreenLed(bool state)
{
#if CWA_VERSION == 12
	// Note on v1.2 this led is also used by hw to showing charging
	GPIO_WriteBit(GPIOA,GPIO_Pin_8, state?Bit_RESET:Bit_SET);
#else
#ifdef CWA_USART_DEBUG
	// With <v1.2 & usart configured, the green led shows serial tx
#else
	GPIO_WriteBit(GPIOA,GPIO_Pin_9, state?Bit_RESET:Bit_SET);
#endif
#endif
}

void RedLed(bool state)
{
	GPIO_WriteBit(GPIOA,GPIO_Pin_10, state?Bit_RESET:Bit_SET);
}

void gpioMinimumPower() {
	
	GPIO_InitTypeDef GPIO_InitStructure;

	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;

#if CWA_VERSION == 10
	// V1.0 uses push-pull drive to the LEDs on GPIOA8/9/10
	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
	GPIO_InitStructure.GPIO_Pin =
		GPIO_Pin_2 | GPIO_Pin_4 | GPIO_Pin_5 | GPIO_Pin_7 | GPIO_Pin_8 |
		GPIO_Pin_9 | GPIO_Pin_10 | GPIO_Pin_11 | GPIO_Pin_12;
	GPIO_Init(GPIOA,&GPIO_InitStructure);
	GPIO_SetBits(GPIOA, GPIO_Pin_2 | GPIO_Pin_4 | GPIO_Pin_5 | GPIO_Pin_7 |
		GPIO_Pin_10);
#ifndef CWA_USART_DEBUG
	GPIO_SetBits(GPIOA, GPIO_Pin_9);
#endif
	GPIO_ResetBits(GPIOA, GPIO_Pin_8);
#endif // VERSION == 10

#if CWA_VERSION == 11
	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
	GPIO_InitStructure.GPIO_Pin =
		GPIO_Pin_2 | GPIO_Pin_4 | GPIO_Pin_5 | GPIO_Pin_7 |
		GPIO_Pin_11 | GPIO_Pin_12;
	GPIO_Init(GPIOA,&GPIO_InitStructure);
	// LEDs are open-drain
	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_OD;
	GPIO_InitStructure.GPIO_Pin = GPIO_Pin_8 | GPIO_Pin_9 | GPIO_Pin_10;
	GPIO_Init(GPIOA,&GPIO_InitStructure);
	GPIO_SetBits(GPIOA, GPIO_Pin_2 | GPIO_Pin_4 | GPIO_Pin_5 | GPIO_Pin_7 |
		GPIO_Pin_8 | GPIO_Pin_10);
#ifndef CWA_USART_DEBUG
	GPIO_SetBits(GPIOA, GPIO_Pin_9);
#endif
#endif // VERSION == 11

#if CWA_VERSION == 12
	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
	GPIO_InitStructure.GPIO_Pin =
		GPIO_Pin_2 | GPIO_Pin_3 | GPIO_Pin_4 | GPIO_Pin_5 | GPIO_Pin_7 |
		GPIO_Pin_9 | GPIO_Pin_10 | GPIO_Pin_11 | GPIO_Pin_12;
	GPIO_Init(GPIOA,&GPIO_InitStructure);
	// green LED is open-drain because it's shared with NCHG
	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_OD;
	GPIO_InitStructure.GPIO_Pin = GPIO_Pin_8;
	GPIO_Init(GPIOA,&GPIO_InitStructure);
	GPIO_SetBits(GPIOA, GPIO_Pin_2 | GPIO_Pin_3 | GPIO_Pin_4 | GPIO_Pin_5 |
		GPIO_Pin_7 | GPIO_Pin_8 | GPIO_Pin_10);
#ifndef CWA_USART_DEBUG
	GPIO_SetBits(GPIOA, GPIO_Pin_9);
#endif
#endif // VERSION == 12

	GPIO_ResetBits(GPIOA, GPIO_Pin_1 | GPIO_Pin_11 | GPIO_Pin_12);

	// Make MISO a pullup
	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPU;
	GPIO_InitStructure.GPIO_Pin = GPIO_Pin_6;
	GPIO_Init(GPIOA,&GPIO_InitStructure);
	
	// Configure wakeup as input floating
	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN_FLOATING;
	GPIO_InitStructure.GPIO_Pin = GPIO_Pin_0;
	GPIO_Init(GPIOA, &GPIO_InitStructure);

	// Set GPIO1 to be analogue input for battery voltage
	// reducing drain due to potential divider
	GPIO_InitStructure.GPIO_Mode=GPIO_Mode_AIN;
	GPIO_InitStructure.GPIO_Pin=GPIO_Pin_1;
	GPIO_Init(GPIOA,&GPIO_InitStructure);

	// Configure USB pull-up pin
	GPIO_InitStructure.GPIO_Pin = GPIO_Pin_2;
	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_OD;
	GPIO_Init(GPIOA, &GPIO_InitStructure);
}

void gpioEnableLeds() {
	
	GPIO_InitTypeDef GPIO_InitStructure;

	// Configure LEDs
#if CWA_VERSION == 10
	// V1.0 requires push-pull drive, at least to the blue LED
	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;

#ifdef CWA_USART_DEBUG
	// Don't trample on the USART tx pin if we're doing debug
	GPIO_InitStructure.GPIO_Pin = GPIO_Pin_8 | GPIO_Pin_10;
#else
	GPIO_InitStructure.GPIO_Pin = GPIO_Pin_8 | GPIO_Pin_9 | GPIO_Pin_10;
#endif // CWA_USART_DEBUG

	GPIO_Init(GPIOA, &GPIO_InitStructure);
#endif // CWA_VERSION == 10

#if CWA_VERSION == 11
	// V1.1 has open-drain LEDs
	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_OD;

#ifdef CWA_USART_DEBUG
	// Don't trample on the USART tx pin if we're doing debug
	GPIO_InitStructure.GPIO_Pin = GPIO_Pin_8 | GPIO_Pin_10;
#else
	GPIO_InitStructure.GPIO_Pin = GPIO_Pin_8 | GPIO_Pin_9 | GPIO_Pin_10;
#endif // CWA_USART_DEBUG

	GPIO_Init(GPIOA, &GPIO_InitStructure);
#endif // CWA_VERSION == 11

#if CWA_VERSION == 12
	// V1.2 has open-drain green LED on GPIO8, others are push-pull to minimise
	// leakage
	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_OD;
	GPIO_InitStructure.GPIO_Pin = GPIO_Pin_8;
	GPIO_Init(GPIOA, &GPIO_InitStructure);

#ifdef CWA_USART_DEBUG
	// Don't trample on the USART tx pin if we're doing debug
	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
	GPIO_InitStructure.GPIO_Pin = GPIO_Pin_10;
	GPIO_Init(GPIOA, &GPIO_InitStructure);
#else
	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
	GPIO_InitStructure.GPIO_Pin = GPIO_Pin_9 | GPIO_Pin_10;
	GPIO_Init(GPIOA, &GPIO_InitStructure);
#endif // CWA_USART_DEBUG

#endif // CWA_VERSION == 12

	// Initialise all LEDs off
	BlueLed(0);
	GreenLed(0);
	RedLed(0);
}

void gpioEnableSpi() {
	
	GPIO_InitTypeDef GPIO_InitStructure;
	
	// Configure SPI1 pins: SCK, MISO and MOSI
	GPIO_InitStructure.GPIO_Pin = GPIO_Pin_5 | GPIO_Pin_6 | GPIO_Pin_7;
	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;
	GPIO_Init(GPIOA, &GPIO_InitStructure);

	// Pin 4 is NSS, manually controlled
	GPIO_InitStructure.GPIO_Pin = GPIO_Pin_4;
	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
	GPIO_Init(GPIOA, &GPIO_InitStructure);

#if CWA_VERSION == 11
	// Port B pin 0 is nCS for temp sensor (as well as for NAND)
	GPIO_InitStructure.GPIO_Pin = GPIO_Pin_0;
	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
	GPIO_Init(GPIOB, &GPIO_InitStructure);
#endif // CWA_VERSION == 11

#if CWA_VERSION == 12
	// Port A pin 3 is nCS for temp sensor
	GPIO_SetBits(GPIOA, GPIO_Pin_3);
	GPIO_InitStructure.GPIO_Pin = GPIO_Pin_3;
	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
	GPIO_Init(GPIOA, &GPIO_InitStructure);
#endif // CWA_VERSION == 12
}

void gpioEnableAccelerometer() {
	
	GPIO_InitTypeDef GPIO_InitStructure;
	
	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN_FLOATING;
	GPIO_InitStructure.GPIO_Pin = GPIO_Pin_13;
	GPIO_Init(GPIOC,&GPIO_InitStructure);
}

void GPIO_Configuration(void)
{
	gpioMinimumPower();
	gpioEnableLeds();
	gpioEnableAccelerometer();
}

void EXTI_Configuration(void)
{
	EXTI_InitTypeDef EXTI_InitStructure;

#ifdef ENABLE_WAKEUP
	// Connect EXTI Line to the wakeup GPIO Pin
	GPIO_EXTILineConfig(GPIO_PortSourceGPIOA, GPIO_PinSource0);  
	
	// Configure EXTI line as wakeup
	EXTI_ClearITPendingBit(EXTI_Line0);
	EXTI_InitStructure.EXTI_Line = EXTI_Line0;
	EXTI_InitStructure.EXTI_Mode = EXTI_Mode_Interrupt;
	EXTI_InitStructure.EXTI_Trigger = EXTI_Trigger_Rising;  
	EXTI_InitStructure.EXTI_LineCmd = ENABLE;
	EXTI_Init(&EXTI_InitStructure);
#endif

	// Accelerometer interrupt
	GPIO_EXTILineConfig(GPIO_PortSourceGPIOC, GPIO_PinSource13);  
	
	EXTI_ClearITPendingBit(EXTI_Line13);
	EXTI_InitStructure.EXTI_Line = EXTI_Line13;
	EXTI_InitStructure.EXTI_Mode = EXTI_Mode_Interrupt;
	EXTI_InitStructure.EXTI_Trigger = EXTI_Trigger_Rising;  
	EXTI_InitStructure.EXTI_LineCmd = ENABLE;
	EXTI_Init(&EXTI_InitStructure);
	
	// Configure EXTI Line17(RTC Alarm) to generate an interrupt on rising edge
	EXTI_ClearITPendingBit(EXTI_Line17);
	EXTI_InitStructure.EXTI_Line = EXTI_Line17;
	EXTI_InitStructure.EXTI_Mode = EXTI_Mode_Interrupt;
	EXTI_InitStructure.EXTI_Trigger = EXTI_Trigger_Rising;
	EXTI_InitStructure.EXTI_LineCmd = ENABLE;
	EXTI_Init(&EXTI_InitStructure);
}

// The parameter fast_clock indicates whether the SPI interface should be
// configured for a 48MHz system clock or 8MHz (HSI) clock
void SPI_Configuration_Accelerometer(int fast_clock)
{
	SPI_InitTypeDef  SPI_InitStructure;

	// disable the peripheral before fiddling with settings
	SPI_Cmd(SPI1, DISABLE);

	// SPI1 configuration
	SPI_InitStructure.SPI_Direction = SPI_Direction_2Lines_FullDuplex;
	SPI_InitStructure.SPI_Mode = SPI_Mode_Master;
	SPI_InitStructure.SPI_DataSize = SPI_DataSize_8b;
	SPI_InitStructure.SPI_CPOL = SPI_CPOL_High;
	SPI_InitStructure.SPI_CPHA = SPI_CPHA_2Edge;
	SPI_InitStructure.SPI_NSS = SPI_NSS_Soft;
	SPI_InitStructure.SPI_BaudRatePrescaler =
		fast_clock?SPI_BaudRatePrescaler_8:SPI_BaudRatePrescaler_2;
	SPI_InitStructure.SPI_FirstBit = SPI_FirstBit_MSB;
	// SPI_InitStructure.SPI_CRCPolynomial = 7;
	SPI_Init(SPI1, &SPI_InitStructure);

	// Disable SPI1 CRC calculation
	SPI_CalculateCRC(SPI1, DISABLE);

	// Enable SPI1
	SPI_Cmd(SPI1, ENABLE);
}

void SPI_Configuration_TempSensor(void)
{
	SPI_InitTypeDef  SPI_InitStructure;

	// Disable the peripheral before fiddling with settings
	SPI_Cmd(SPI1, DISABLE);

	// SPI1 configuration
	// single line, starting in Tx mode otherwise the clock starts before we've
	// enabled chip select
	SPI_InitStructure.SPI_Direction = SPI_Direction_1Line_Tx;
	SPI_InitStructure.SPI_Mode = SPI_Mode_Master;
	SPI_InitStructure.SPI_DataSize = SPI_DataSize_16b;
	SPI_InitStructure.SPI_CPOL = SPI_CPOL_Low;
	SPI_InitStructure.SPI_CPHA = SPI_CPHA_1Edge;
	SPI_InitStructure.SPI_NSS = SPI_NSS_Soft;
	// Slightly slower clock speed to meet temp sensor's timing requirements
	SPI_InitStructure.SPI_BaudRatePrescaler = SPI_BaudRatePrescaler_16;
	SPI_InitStructure.SPI_FirstBit = SPI_FirstBit_MSB;
	// SPI_InitStructure.SPI_CRCPolynomial = 7;
	SPI_Init(SPI1, &SPI_InitStructure);

	// Disable SPI1 CRC calculation
	SPI_CalculateCRC(SPI1, DISABLE);

	// Enable SPI1
	SPI_Cmd(SPI1, ENABLE);
}

void RTC_Configuration(void)
{
	// RTC clock source configuration

	// Enable the LSE OSC
	RCC_LSEConfig(RCC_LSE_ON);
	
	// Wait till LSE is ready
	while(RCC_GetFlagStatus(RCC_FLAG_LSERDY) == RESET) {}

	// Select the RTC Clock Source
	RCC_RTCCLKConfig(RCC_RTCCLKSource_LSE);

	// Enable the RTC Clock
	RCC_RTCCLKCmd(ENABLE);

	// RTC configuration
	// Wait for RTC APB registers synchronisation
	RTC_WaitForSynchro();
	RTC_WaitForLastTask();
}

void NVIC_Configuration(void)
{
	NVIC_InitTypeDef NVIC_InitStructure;

	// 2 bits for Preemption Priority and 2 bits for Sub Priority
	NVIC_PriorityGroupConfig(NVIC_PriorityGroup_2);

#ifdef ENABLE_WAKEUP
    // Enable and set EXTI Interrupt for wakeup to the lowest priority
    NVIC_InitStructure.NVIC_IRQChannel = EXTI0_IRQn;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 1;
    NVIC_InitStructure.NVIC_IRQChannelSubPriority = 0;
    NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&NVIC_InitStructure); 
#endif

    // Enable the EXTI13 Interrupt
    NVIC_InitStructure.NVIC_IRQChannel = EXTI15_10_IRQn;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 1;
    NVIC_InitStructure.NVIC_IRQChannelSubPriority = 0;
    NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&NVIC_InitStructure); 
    
	// Enable the RTC SEC Interrupt
	NVIC_InitStructure.NVIC_IRQChannel = RTC_IRQn;
	NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 0;
	NVIC_InitStructure.NVIC_IRQChannelSubPriority = 0;
	NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
	NVIC_Init(&NVIC_InitStructure);

	// Enable the RTC Alarm Interrupt
	NVIC_InitStructure.NVIC_IRQChannel = RTCAlarm_IRQn;
	NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 0;
	NVIC_InitStructure.NVIC_IRQChannelSubPriority = 0;
	NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
	NVIC_Init(&NVIC_InitStructure);
}

void ADC1_Configuration() {
	
	GPIO_InitTypeDef GPIO_InitStructure;
	ADC_InitTypeDef   ADC_InitStructure;

	// ADC init
	// ADC Deinit
	RCC_APB2PeriphClockCmd(RCC_APB2Periph_ADC1, ENABLE);
	ADC_DeInit(ADC1);

	// PA1 - analog input
	GPIO_InitStructure.GPIO_Pin = GPIO_Pin_1;
	GPIO_InitStructure.GPIO_Speed = (GPIOSpeed_TypeDef)0;
	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AIN;
	GPIO_Init (GPIOA, &GPIO_InitStructure);

	// ADC Structure Initialization
	ADC_StructInit(&ADC_InitStructure);

	ADC_InitStructure.ADC_Mode = ADC_Mode_Independent;
	ADC_InitStructure.ADC_ScanConvMode = DISABLE;
	ADC_InitStructure.ADC_ContinuousConvMode = DISABLE;
	ADC_InitStructure.ADC_ExternalTrigConv = ADC_ExternalTrigConv_None;
	ADC_InitStructure.ADC_DataAlign = ADC_DataAlign_Right;
	ADC_InitStructure.ADC_NbrOfChannel = 1;
	ADC_Init(ADC1, &ADC_InitStructure);

	// Enable the ADC
	ADC_Cmd(ADC1, ENABLE);

	// ADC calibration
	// Enable ADC1 reset calibration register
	ADC_ResetCalibration(ADC1);

	// Check the end of ADC1 reset calibration register
	while(ADC_GetResetCalibrationStatus(ADC1) == SET);

	// Start ADC1 calibaration
	ADC_StartCalibration(ADC1);

	// Check the end of ADC1 calibration
	while(ADC_GetCalibrationStatus(ADC1) == SET);
}

void  ADC1_Shutdown() {
	
	// Disable the ADC
	ADC_Cmd(ADC1, DISABLE);
	RCC_APB2PeriphClockCmd(RCC_APB2Periph_ADC1, DISABLE);

}

uint16_t getADC1Channel() {
	
	uint16_t value;
	// Configure channel
	ADC_RegularChannelConfig(ADC1, ADC_Channel_1, 1, ADC_SampleTime_55Cycles5);

	// Start the conversion
	ADC_SoftwareStartConvCmd(ADC1, ENABLE);

	// Wait until conversion completion
	while(ADC_GetFlagStatus(ADC1, ADC_FLAG_EOC) == RESET);

	// Get the conversion value
	value = ADC_GetConversionValue(ADC1);

	// ADC_DeInit(ADC1);

	return value;
}

// Simple fns to save/restore uint32_t to BKP domain

void saveValue(uint16_t bkp_reg, uint32_t data) {

	if (bkp_reg < BKP_DR10) {
		uint16_t p1 = data & 0xffff;
		uint16_t p2 = (data & 0xffff0000) >> 16;
		BKP_WriteBackupRegister(bkp_reg, p2);
		BKP_WriteBackupRegister(bkp_reg + 0x0004, p1);
	}
}

uint32_t restoreValue(uint16_t bkp_reg) {
	
	uint32_t data = 0;
	if (bkp_reg < BKP_DR10) {
		uint32_t v = BKP_ReadBackupRegister(bkp_reg);
		data = (uint32_t)(v << 16);
		v = BKP_ReadBackupRegister(bkp_reg + 0x0004);
		data += (uint32_t)(v & 0xFFFF);
	}
	return data;
}

void Set_USBClock(void)
{
	// Select USBCLK source
	RCC_USBCLKConfig(RCC_USBCLKSource_PLLCLK_Div1);

	// Enable the USB clock
	RCC_APB1PeriphClockCmd(RCC_APB1Periph_USB, ENABLE);
}

void Enter_LowPowerMode(void)
{
	// Set the device state to suspend
	bDeviceState = SUSPENDED;
}

void Leave_LowPowerMode(void)
{
	DEVICE_INFO *pInfo = &Device_Info;

	// Set the device state to the correct state
	if (pInfo->Current_Configuration != 0) {
		// Device configured
		bDeviceState = CONFIGURED;
	} else {
		bDeviceState = ATTACHED;
	}
}

void USB_Interrupts_Config(void)
{
	NVIC_InitTypeDef NVIC_InitStructure;

	NVIC_PriorityGroupConfig(NVIC_PriorityGroup_1);

	NVIC_InitStructure.NVIC_IRQChannel = USB_LP_CAN1_RX0_IRQn;
	NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 0;
	NVIC_InitStructure.NVIC_IRQChannelSubPriority = 0;
	NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
	NVIC_Init(&NVIC_InitStructure);
}

void USB_Interrupts_Disable(void)
{
	NVIC_InitTypeDef NVIC_InitStructure;

	NVIC_InitStructure.NVIC_IRQChannel = USB_LP_CAN1_RX0_IRQn;
	NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 0;
	NVIC_InitStructure.NVIC_IRQChannelSubPriority = 0;
	NVIC_InitStructure.NVIC_IRQChannelCmd = DISABLE;
	NVIC_Init(&NVIC_InitStructure);
}

void USB_Cable_Config(FunctionalState NewState)
{
	if (NewState != DISABLE) {
		GPIO_ResetBits(GPIOA, GPIO_Pin_2);
	} else {
		GPIO_SetBits(GPIOA, GPIO_Pin_2);
	}
}

void USB_Send_Data(uint8_t* data_buffer, uint8_t Nb_bytes)
{
//#ifdef CWA_USART_DEBUG
#if 0
	int n, c;
	SendCharUart1('|');
	for(n=0;n<Nb_bytes;n++) {
		c=data_buffer[n];
		if(c>31 && c<127)
			SendCharUart1(c);
		else
			SendCharUart1('.');
	}
#endif

	USB_SIL_Write(EP1_IN, data_buffer, Nb_bytes);

	SetEPTxValid(ENDP1);
}

void USB_ConfigSerialNum(uint8_t* tag_id_ptr) {
	uint32_t Device_Serial0;
	uint8_t index = 2;
	uint8_t loop;
	
/*
	The following byte values are not valid for USB serial numbers:
	Value 0x2C i.e ','
	Values less than 0x20.
	Values greater than 0x7F.
	For additional details on the iSerialNumber value, see section 9.6.1 of the
	USB 2.0 specification.
*/

	// Use tag id as serial number
	if (tag_id_ptr && *tag_id_ptr) {
		uint8_t* ptr = tag_id_ptr;
		for (loop = 0; loop < MAX_ID_LEN; loop++) {
			Simple_StringSerial[index] = *ptr++;
			index += 2;
			if (!*ptr)
				break;
		}
	} else {
		// Default to BIOBAND followed by unique id
		index = 16;
	}

	Device_Serial0 = *(__IO uint32_t*)(0x1FFFF7E8);

	// Add unique id in case 2 or more tags with same tag id
	if (Device_Serial0 != 0) {
		uint32_t Device_Serial2 = *(__IO uint32_t*)(0x1FFFF7F0);;
#if 0
		uint32_t Device_Serial1 = *(__IO uint32_t*)(0x1FFFF7EC);
		Simple_StringSerial[index] =
			(uint8_t)(Device_Serial1 & 0x000000FF);
		index += 2;
		Simple_StringSerial[index] =
			(uint8_t)((Device_Serial1 & 0x0000FF00) >> 8);
		index += 2;
		Simple_StringSerial[index] =
			(uint8_t)((Device_Serial1 & 0x00FF0000) >> 16);
		index += 2;
		Simple_StringSerial[index] =
			(uint8_t)((Device_Serial1 & 0xFF000000) >> 24);
		index += 2;
#endif
		Simple_StringSerial[index] = '_';
		index += 2;
		Simple_StringSerial[index] =
			(uint8_t)(Device_Serial2 & 0x000000FF);
		index += 2;
		Simple_StringSerial[index] =
			(uint8_t)((Device_Serial2 & 0x0000FF00) >> 8);
	} else {
		Simple_StringSerial[index] = 0;
	}
	
	// Check is alphanum (otherwise substitute in valid char)
	for (loop = 2; loop <= index; loop += 2) {
		uint8_t val = Simple_StringSerial[loop];
		if (val < 0x30) {
			Simple_StringSerial[loop] = 0x77;
		} else if (val > 0x39) {
			if (val < 0x41) {
				Simple_StringSerial[loop] = 0x78;
			} else if (val > 0x5A && val != 0x5F) {
				if (val < 0x61) {
					Simple_StringSerial[loop] = 0x79;
				} else if (val > 0x7A) {
					Simple_StringSerial[loop] = 0x7A;
				}
			}
		}
	}
}


#ifdef CWA_USART_DEBUG
// USART operations for debug purposes
void USART_Configuration1(void)
{
	GPIO_InitTypeDef  GPIO_InitStructure;
	USART_InitTypeDef USART_InitStructure;

	// Enable GPIOA and USART1 clock
	RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA |
						 RCC_APB2Periph_USART1, ENABLE);

	// Configure USART1 Tx (PA9) as alternate function push-pull
	GPIO_InitStructure.GPIO_Pin   = GPIO_Pin_9;
	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
	GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_AF_PP;
	GPIO_Init(GPIOA, &GPIO_InitStructure);

	/* USART1 configured as follow:
		- BaudRate = 115200 baud  
		- Word Length = 8 Bits
		- One Stop Bit
		- No parity
		- Hardware flow control disabled (RTS and CTS signals)
		- Receive and transmit enabled
		- USART Clock disabled
		- USART CPOL: Clock is active low
		- USART CPHA: Data is captured on the middle 
		- USART LastBit: The clock pulse of the last data bit is not output to 
						 the SCLK pin
	*/
	USART_InitStructure.USART_BaudRate            = 115200;
	USART_InitStructure.USART_WordLength          = USART_WordLength_8b;
	USART_InitStructure.USART_StopBits            = USART_StopBits_1;
	USART_InitStructure.USART_Parity              = USART_Parity_No;
	USART_InitStructure.USART_HardwareFlowControl =
		USART_HardwareFlowControl_None;
	USART_InitStructure.USART_Mode                = USART_Mode_Tx;  
	USART_Init(USART1, &USART_InitStructure);

	// Enable USART1
	USART_Cmd(USART1, ENABLE);        
}
#endif

int SendCharUart1 (int ch)  
{
#ifdef CWA_USART_DEBUG
	while (!(USART1->SR & USART_FLAG_TXE));
	USART1->DR = (ch & 0x1FF);
#endif
	return (ch);
}

void SendStringUart1(unsigned char *s) {
#ifdef CWA_USART_DEBUG
	while(s && *s)
		SendCharUart1(*s++);
#endif
}

int GetKeyUart1(void) {
	while (!(USART1->SR & USART_FLAG_RXNE));
	return ((int)(USART1->DR & 0x1FF));
}

static void SendHexCharUart1(uint8_t n) {
#ifdef CWA_USART_DEBUG
	n&=0x0f;
	SendCharUart1((n>9)?n+55:n+'0');
#endif
}

void SendHex8Uart1(uint8_t n) {
#ifdef CWA_USART_DEBUG
	SendHexCharUart1(n>>4);
	SendHexCharUart1(n);
#endif
}

void SendHex16Uart1(uint16_t n) {
#ifdef CWA_USART_DEBUG
	SendHexCharUart1(n>>12);
	SendHexCharUart1(n>>8);
	SendHexCharUart1(n>>4);
	SendHexCharUart1(n);
#endif
}

void SendHex32Uart1(uint32_t n) {
#ifdef CWA_USART_DEBUG
	SendHexCharUart1(n>>28);
	SendHexCharUart1(n>>24);
	SendHexCharUart1(n>>20);
	SendHexCharUart1(n>>16);
	SendHexCharUart1(n>>12);
	SendHexCharUart1(n>>8);
	SendHexCharUart1(n>>4);
	SendHexCharUart1(n);
#endif
}

void TIM2_Configuration(void) {
	TIM_TimeBaseInitTypeDef init;
	
	// Enable clock to timer 2
	RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM2, ENABLE);
		
	// Prescaler so timer ticks once per SPI bit for temp sensor
	init.TIM_Prescaler=8;
	init.TIM_CounterMode=TIM_CounterMode_Down;
	init.TIM_Period=0xffff;
	init.TIM_ClockDivision=TIM_CKD_DIV1;
	init.TIM_RepetitionCounter=0;
	TIM_TimeBaseInit(TIM2,&init);
	TIM_SetAutoreload(TIM2,0xffff);
	TIM_InternalClockConfig(TIM2);
	TIM_SelectOnePulseMode(TIM2,TIM_OPMode_Single);
	TIM_UpdateDisableConfig(TIM2,DISABLE);
	TIM_Cmd(TIM2,ENABLE);
}

// EOF
