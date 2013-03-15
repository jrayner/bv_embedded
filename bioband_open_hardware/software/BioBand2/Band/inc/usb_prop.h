/******************** (C) COPYRIGHT 2009 STMicroelectronics ********************
* File Name          : usb_prop.h
* Author             : MCD Application Team
* Version            : V3.1.0
* Date               : 10/30/2009
* Description        : BioBand Properties
********************************************************************************
* THE PRESENT FIRMWARE WHICH IS FOR GUIDANCE ONLY AIMS AT PROVIDING CUSTOMERS
* WITH CODING INFORMATION REGARDING THEIR PRODUCTS IN ORDER FOR THEM TO SAVE TIME.
* AS A RESULT, STMICROELECTRONICS SHALL NOT BE HELD LIABLE FOR ANY DIRECT,
* INDIRECT OR CONSEQUENTIAL DAMAGES WITH RESPECT TO ANY CLAIMS ARISING FROM THE
* CONTENT OF SUCH FIRMWARE AND/OR THE USE MADE BY CUSTOMERS OF THE CODING
* INFORMATION CONTAINED HEREIN IN CONNECTION WITH THEIR PRODUCTS.
*******************************************************************************/

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __usb_prop_H
#define __usb_prop_H

#define Simple_GetConfiguration          NOP_Process
//#define Simple_SetConfiguration          NOP_Process
#define Simple_GetInterface              NOP_Process
#define Simple_SetInterface              NOP_Process
#define Simple_GetStatus                 NOP_Process
#define Simple_ClearFeature              NOP_Process
#define Simple_SetEndPointFeature        NOP_Process
#define Simple_SetDeviceFeature          NOP_Process
//#define Simple_SetDeviceAddress          NOP_Process

/* Exported functions ------------------------------------------------------- */
void Simple_init(void);
void Simple_Reset(void);
void Simple_SetConfiguration(void);
void Simple_SetDeviceAddress (void);
void Simple_Status_In (void);
void Simple_Status_Out (void);
RESULT Simple_Data_Setup(uint8_t);
RESULT Simple_NoData_Setup(uint8_t);
RESULT Simple_Get_Interface_Setting(uint8_t Interface, uint8_t AlternateSetting);
uint8_t *Simple_GetDeviceDescriptor(uint16_t );
uint8_t *Simple_GetConfigDescriptor(uint16_t);
uint8_t *Simple_GetStringDescriptor(uint16_t);

#endif /* __usb_prop_H */

/******************* (C) COPYRIGHT 2009 STMicroelectronics *****END OF FILE****/

