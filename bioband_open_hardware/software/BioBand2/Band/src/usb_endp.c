/******************** (C) COPYRIGHT 2009 STMicroelectronics ********************
* File Name          : usb_endp.c
* Author             : MCD Application Team / MRC
* Version            : V3.1.0
* Date               : 10/30/2009
* Description        : BioBand Endpoints
********************************************************************************
* THE PRESENT FIRMWARE WHICH IS FOR GUIDANCE ONLY AIMS AT PROVIDING CUSTOMERS
* WITH CODING INFORMATION REGARDING THEIR PRODUCTS IN ORDER FOR THEM TO SAVE TIME.
* AS A RESULT, STMICROELECTRONICS SHALL NOT BE HELD LIABLE FOR ANY DIRECT,
* INDIRECT OR CONSEQUENTIAL DAMAGES WITH RESPECT TO ANY CLAIMS ARISING FROM THE
* CONTENT OF SUCH FIRMWARE AND/OR THE USE MADE BY CUSTOMERS OF THE CODING
* INFORMATION CONTAINED HEREIN IN CONNECTION WITH THEIR PRODUCTS.
*******************************************************************************/

#include "usb_lib.h"
#include "usb_desc.h"
#include "usb_mem.h"
#include "hw_config.h"
#include "usb_istr.h"

uint8_t buffer_in[SIMPLE_RX_DATA_SIZE];
__IO uint32_t count_in = 0;

void EP1_IN_Callback(void)
{
	ready_for_tx_cb();
}

void EP2_OUT_Callback(void)
{
#ifdef CWA_USART_DEBUG
	int n;
	int c;
#endif

	/* Get the received data buffer and update the counter */
	count_in = USB_SIL_Read(EP2_OUT, buffer_in);

#ifdef CWA_USART_DEBUG
	SendCharUart1('/');
	for(n=0;n<count_in;n++) {
		c=buffer_in[n];
		if(c>31 && c<127)
			SendCharUart1(c);
		else
			SendCharUart1('.');
	}
#endif

	/* Enable the receive of data on EP2 */
	SetEPRxValid(ENDP2);
}

