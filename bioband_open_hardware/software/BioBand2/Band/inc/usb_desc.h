/******************** (C) COPYRIGHT 2009 STMicroelectronics ********************
* File Name          : usb_desc.h
* Author             : MCD Application Team
* Version            : V3.1.0
* Date               : 10/30/2009
* Description        : BioBand Descriptors
********************************************************************************
* THE PRESENT FIRMWARE WHICH IS FOR GUIDANCE ONLY AIMS AT PROVIDING CUSTOMERS
* WITH CODING INFORMATION REGARDING THEIR PRODUCTS IN ORDER FOR THEM TO SAVE TIME.
* AS A RESULT, STMICROELECTRONICS SHALL NOT BE HELD LIABLE FOR ANY DIRECT,
* INDIRECT OR CONSEQUENTIAL DAMAGES WITH RESPECT TO ANY CLAIMS ARISING FROM THE
* CONTENT OF SUCH FIRMWARE AND/OR THE USE MADE BY CUSTOMERS OF THE CODING
* INFORMATION CONTAINED HEREIN IN CONNECTION WITH THEIR PRODUCTS.
*******************************************************************************/

#ifndef __USB_DESC_H
#define __USB_DESC_H

#define USB_CONTROL_PACKET_SIZE 	0x40

#define SIMPLE_TX_DATA_SIZE         255
#define SIMPLE_RX_DATA_SIZE			64

#define SIMPLE_BULK_DEVICE_DESC_SIZE 18
#define SIMPLE_CONFIG_DESC        67
#define SIMPLE_STRING_LANGID      4
#define SIMPLE_STRING_VENDOR      38
#define SIMPLE_STRING_PRODUCT     50
#define SIMPLE_STRING_SERIAL      38
#define SIMPLE_STRING_INTERFACE   16

#define STANDARD_ENDPOINT_DESC_SIZE             0x09

extern const uint8_t Simple_Bulk_DeviceDescriptor[18];
extern const uint8_t Simple_Bulk_ConfigDescriptor[SIMPLE_CONFIG_DESC];

extern const uint8_t Simple_StringLangID[SIMPLE_STRING_LANGID];
extern const uint8_t Simple_StringVendor[SIMPLE_STRING_VENDOR];
extern const uint8_t Simple_StringProduct[SIMPLE_STRING_PRODUCT];
extern uint8_t Simple_StringSerial[SIMPLE_STRING_SERIAL];
extern const uint8_t Simple_StringInterface[SIMPLE_STRING_INTERFACE];

#endif /* __USB_DESC_H */
