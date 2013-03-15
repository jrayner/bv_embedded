/******************** (C) COPYRIGHT 2009 STMicroelectronics ********************
* File Name          : usb_desc.c
* Author             : MCD Application Team / MRC
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

/* Includes ------------------------------------------------------------------*/
#include "usb_lib.h"
#include "usb_desc.h"
#include "shared.h"

const uint8_t Simple_Bulk_DeviceDescriptor[] =
  {
    18,   	/* bLength */
    0x01,	/* bDescriptorType */
    0x00,
    0x02,   /* bcdUSB = 2.00 */
    0xff,   /* vendor specific - *** our own bulk device */
    0x00,   /* bDeviceSubClass */
    0x00,   /* bDeviceProtocol */
    0x40,   /* bMaxPacketSize0 endpoint0 = 64 */
    VENDOR_LO,
    VENDOR_HI,
    PRODUCT_LO,
    PRODUCT_HI,
    0x00,   /* bcdDevice 2.00*/
    0x02,
    1,      /* Index of string descriptor describing manufacturer */
    2,      /* Index of string descriptor describing product */
    3,      /* Index of string descriptor describing the device's serial num */
    0x01    /* bNumConfigurations */
  };

const uint8_t Simple_Bulk_ConfigDescriptor[] =
  {

    0x09,   /* bLength: Configuation Descriptor size */
    0x02,	/* bDescriptorType: Configuration */
    32,		/* wTotalLength (2 bytes) 32 */
    0x00,
    0x01,   /* bNumInterfaces: 1 interface */
    0x01,   /* bConfigurationValue: Configuration value */
    0x00,   /* iConfiguration:  Index of string descriptor describing */
    0xC0,   /* bmAttributes: bus powered */
    0x32,   /* MaxPower 100 mA */

    /* 09 */
    0x09,   /* bLength: Interface Descriptor size */
    0x04,   /* bDescriptorType: Interface descriptor type */
    0x00,   /* bInterfaceNumber: Number of Interface */
    0x00,   /* bAlternateSetting: Alternate setting */
    0x02,   /* bNumEndpoints*/
    0xff,   /* bInterfaceClass: Vendor specific */
    0x00,   /* bInterfaceSubClass */
    0x00,   /* nInterfaceProtocol */
    4,      /* iInterface: Index of string descriptor for the interface */
    /* 18 */
    0x07,   /*Endpoint descriptor length = 7*/
    0x05,   /*Endpoint descriptor type */
    0x81,   /*Endpoint address (address 1) */
    0x02,   /*Bulk endpoint type */
    SIMPLE_TX_DATA_SIZE,   /*Maximum packet size */
    0x00,
    0x00,   /*Polling interval in milliseconds */
    /* 25 */
    0x07,   /*Endpoint descriptor length = 7 */
    0x05,   /*Endpoint descriptor type */
    0x02,   /*Endpoint address (address 2) */
    0x02,   /*Bulk endpoint type */
    SIMPLE_RX_DATA_SIZE,   /*Maximum packet size */
    0x00,
    0x00     /*Polling interval in milliseconds*/
    /*32*/
  };

/* USB String Descriptors */
const uint8_t Simple_StringLangID[SIMPLE_STRING_LANGID] =
  {
    SIMPLE_STRING_LANGID,
    0x03, /* bDescriptorType*/
    0x09,
    0x04 /* LangID = 0x0409: U.S. English */
  };

const uint8_t Simple_StringVendor[SIMPLE_STRING_VENDOR] =
  {
    SIMPLE_STRING_VENDOR,     /* Size of Vendor string */
    0x03, /* bDescriptorType*/
    /* Manufacturer: */
    'M', 0, 'R', 0, 'C', 0, '0', 0, '1', 0, ' ', 0, 'C', 0, 'W', 0,
    'A', 0, ' ', 0, 'B', 0, 'i', 0, 'o', 0, 'b', 0, 'a', 0, 'n', 0,
    'k', 0, ' ', 0
  };

const uint8_t Simple_StringProduct[SIMPLE_STRING_PRODUCT] =
  {
    SIMPLE_STRING_PRODUCT,          /* bLength */
    0x03, /* bDescriptorType*/
    /* Product name: */
    'M', 0, 'R', 0, 'C', 0, '0', 0, '1', 0, ' ', 0, 'C', 0, 'W', 0,
    'A', 0, ' ', 0, 'B', 0, 'i', 0, 'o', 0, 'b', 0, 'a', 0, 'n', 0,
    'k', 0, ' ', 0, '0', 0, '0', 0, '1', 0, '.', 0, '0', 0, ' ', 0
  };

// see USB_ConfigSerialNum
uint8_t Simple_StringSerial[SIMPLE_STRING_SERIAL] =
  {
    SIMPLE_STRING_SERIAL,           /* bLength */
    0x03, /* bDescriptorType*/
    'B', 0, 'I', 0, 'O', 0, 'B', 0, 'A', 0, 'N', 0, 'D', 0, 'c', 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
  };
  
const uint8_t Simple_StringInterface[SIMPLE_STRING_INTERFACE] =
  {
    SIMPLE_STRING_INTERFACE,
    0x03, /* bDescriptorType*/
    'M', 0, 'R', 0, 'C', 0, ' ', 0, 'C', 0, 'W', 0, 'A', 0
  };

