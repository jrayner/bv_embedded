/* Convert raw PDUs to and from gsmd format.
 *
 * Copyright (C) 2007 OpenMoko, Inc.
 * Written by Andrzej Zaborowski <andrew@openedhand.com>
 *
 * Copyright (C) 2007-2009 Jim Rayner <jimr@beyondvoice.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston,
 * MA 02111-1307 USA
 */

#include <stdio.h>
#include <string.h>

#include "gsmd.h"

#include <gsmd/gsmd.h>
#include <gsmd/usock.h>
#include <gsmd/sms.h>

/* forward decl */
u_int8_t sms_pdu_encode_dcs(struct gsmd_sms_datacodingscheme *dcs);

static int sms_number_bytelen(u_int8_t type, u_int8_t len)
{
	switch (type & __GSMD_TOA_TON_MASK) {
	case GSMD_TOA_TON_ALPHANUMERIC:
		return (len + 1) >> 1;
	default:
		return (len + 1) >> 1;
	}
}

static int sms_address2ascii(struct gsmd_addr *dst, const u_int8_t *src)
{
	int i;

	if (src[0] > GSMD_ADDR_MAXLEN)
		return 1;

	/* The Type-of-address field */
	dst->type = src[1];

	switch (dst->type & __GSMD_TOA_TON_MASK) {
	case GSMD_TOA_TON_ALPHANUMERIC:
		for (i = 0; ((i * 7 + 3) >> 2) < src[0]; i ++)
			dst->number[i] =
				((src[2 + ((i * 7 + 7) >> 3)] <<
				  (7 - ((i * 7 + 7) & 7))) |
				 (src[2 + ((i * 7) >> 3)] >>
				  ((i * 7) & 7))) & 0x7f;
		break;
	default:
		for (i = 0; i < src[0]; i ++)
			dst->number[i] = '0' +
				((src[2 + (i >> 1)] >> ((i << 2) & 4)) & 0xf);
	}
	dst->number[i] = 0;

	return 0;
}

void convert_pdu_to_text(char* dest, const char* src, int length)
{
	int i = 0;
	int index = 0;
	for (i = 0; i < length; i++)
	{
		index = i * 2;
		unsigned char nibble = (unsigned char) ((src[i] >> 4) & 0xF);
		if (nibble < 10)
			dest[index] = (unsigned char) (nibble + 0x30); /* 0 */
		else
			dest[index] = (unsigned char) (nibble + 0x37); /* 'A' - 10 */

		nibble = (unsigned char) (src[i] & 0xF);
		if (nibble < 10)
			dest[index+1] = (unsigned char) (nibble + 0x30);
		else
			dest[index+1] = (unsigned char) (nibble + 0x37);
	}
}

int sms_pdu_to_msg(struct gsmd_sms_list *dst,
		const u_int8_t *src, int pdulen, int len)
{
	int i, vpf;
	if (len < 1 || len < 1 + src[0] + pdulen || pdulen < 1) {
		DEBUGP("Incorrect len\n");
		return 1;
	}

	/* init voicemail is false */
	dst->payload.is_voicemail = 0;
	dst->payload.is_statusreport = 0;

	/* Skip SMSC number and its Type-of-address */
	len -= 1 + src[0];
	src += 1 + src[0];

	/* TP-UDHI */
	dst->payload.has_header = !!(src[0] & GSMD_SMS_TP_UDHI_WITH_HEADER);

	/* TP-VPF */
	vpf = (src[0] >> 3) & 3;

	/* TP-MTI */
	switch (src[0] & 3) {
	case GSMD_SMS_TP_MTI_DELIVER:
		if (len < 3) {
			DEBUGP("Too small\n");
			return 1;
		}
		i = sms_number_bytelen(src[2], src[1]);
		if (len < 13 + i) {
			DEBUGP("Bad len (len %d < %d)\n",len,13+i);
			return 1;
		}

		if (sms_address2ascii(&dst->addr, src + 1)) {
			DEBUGP("Bad address\n");
			return 1;
		}

		len -= 3 + i;
		src += 3 + i;

		/* check voicemail by TP-PID */
		if(src[0] == 0x5f)  /* return call message */
			dst->payload.is_voicemail = 1;

		/* decode TP-DCS */
		if(sms_pdu_decode_dcs(&dst->payload.dcs,src+1)) {
			DEBUGP("Bad dcs\n");
			return 1;
		}

		/* check voicemail by MWI */
		if(dst->payload.dcs.mwi_kind == MESSAGE_WAITING_VOICEMAIL &&
			(dst->payload.dcs.mwi_group == MESSAGE_WAITING_DISCARD ||
			dst->payload.dcs.mwi_group == MESSAGE_WAITING_STORE))
			dst->payload.is_voicemail = 1;

		/* TP-SCTS */
		memcpy(dst->time_stamp, src + 2, 7);

		/* Skip TP-PID */
		len -= 9;
		src += 9;

		/* TP-UDL */
		if (SMS_ALPHABET_7_BIT_DEFAULT == dst->payload.dcs.alphabet) {
			dst->payload.size_encoded_userdata = src[0];
			dst->payload.physical_byte_length = len - 1;
		} else if (SMS_ALPHABET_8_BIT == dst->payload.dcs.alphabet) {
			dst->payload.size_encoded_userdata = src[0];
			dst->payload.physical_byte_length = src[0];
		} else if (SMS_ALPHABET_UCS2 == dst->payload.dcs.alphabet) {
			dst->payload.size_encoded_userdata = src[0] / 2;
			dst->payload.physical_byte_length = src[0];
		}

		/* TP-UD */
		if (len < 1 + dst->payload.physical_byte_length ||
			dst->payload.physical_byte_length > GSMD_SMS_DATA_MAXLEN) {
			DEBUGP("Size error - len %d pl %d\n",
				len,dst->payload.physical_byte_length);
			return 1;
		}
		memcpy(dst->payload.data, src + 1, dst->payload.physical_byte_length);
		dst->payload.data[dst->payload.physical_byte_length] = 0;

		break;
	case GSMD_SMS_TP_MTI_SUBMIT:
		if (len < 4) {
			DEBUGP("Too small\n");
			return 1;
		}
		i = sms_number_bytelen(src[3], src[2]);
		if (len < 7 + i) {
			DEBUGP("Bad len (%d < %d)\n",len,7+i);
			return 1;
		}

		if (sms_address2ascii(&dst->addr, src + 2)) {
			DEBUGP("Incorrect address\n");
			return 1;
		}

		len -= 4 + i;
		src += 4 + i;

		/* decode TP-DCS */
		if(sms_pdu_decode_dcs(&dst->payload.dcs,src+1)) {
			DEBUGP("Invalid dcs\n");
			return 1;
		}

		/* Skip TP-PID and TP-Validity-Period */
		len -= vpf ? 3 : 2;
		src += vpf ? 3 : 2;

		memset(dst->time_stamp, 0, 7);

		/* TP-UDL */
		dst->payload.size_encoded_userdata = src[0];
		dst->payload.physical_byte_length = src[0];

		/* TP-UD */
		if (SMS_ALPHABET_7_BIT_DEFAULT == dst->payload.dcs.alphabet) {
			memcpy(dst->payload.data, src + 1, dst->payload.size_encoded_userdata);
			dst->payload.data[dst->payload.size_encoded_userdata] = 0;
		} else {
			if (len < 1 + dst->payload.physical_byte_length ||
				dst->payload.physical_byte_length > GSMD_SMS_DATA_MAXLEN) {
				DEBUGP("Incorrect len %d %d\n",len,dst->payload.physical_byte_length);
				return 1;
			}
			memcpy(dst->payload.data, src + 1, dst->payload.physical_byte_length);
			dst->payload.data[dst->payload.physical_byte_length] = 0;
		}
		break;
	case GSMD_SMS_TP_MTI_STATUS_REPORT:
		{
			int status = 0;
			DEBUGP("Status report (len %d)\n",len);
			if (len < 19) {
				DEBUGP("Too small\n");
				return 1;
			}
	
			/* TP-MR set it gsmd_sms_list.index*/
			dst->payload.is_statusreport = 1;
	
			/* msg ref */
			dst->payload.data[GSMD_STATUS_REP_INDEX] = (int) src[1];
	
			/* TP-RA */
			i = sms_number_bytelen(src[3], src[2]);
			if (len < 17 + i) {
				DEBUGP("Bad len\n");
				return 1;
			}
			if (sms_address2ascii(&dst->addr, src + 2)) {
				DEBUGP("Incorrect address\n");
				return 1;
			}
			DEBUGP("number len %d\n",i);

			/* ignore TP-SCTS */
			len -= 11 + i;
			src += 11 + i;

			/* use TP-DT */
			memcpy(dst->time_stamp, src, 7);
			len -= 7;
	
			/* TP-UD  */
			/* TP-STATUS */
			DEBUGP("UD len %d\n",len);
			if (len)
				status = (int) src[7];
			dst->payload.data[GSMD_STATUS_REP_STATUS] = status;
	
			DEBUGP("status %d\n",status);
	
			dst->payload.physical_byte_length = 2;
		}
		break;
	default:
		DEBUGP("Unknown PDU type\n");
		return 1;
	}


	return 0;
}

/* Refer to GSM 03.40 subclause 9.2.3.3, for SMS-SUBMIT */
int sms_pdu_make_smssubmit(char *dest, const struct gsmd_sms_submit *src)
{
	/* FIXME: ALPHANUMERIC encoded addresses can be longer than 13B */
	u_int8_t header[15 + GSMD_ADDR_MAXLEN];
	int pos = 0, i, len;

	/* SMSC Length octet.  If omitted or zero, use SMSC stored in the
	 * phone.  One some phones this can/has to be omitted.  */
	header[pos ++] = 0x00;

	header[pos ++] =
		GSMD_SMS_TP_MTI_SUBMIT |
		(0 << 2) |		/* Reject Duplicates: 0 */
		GSMD_SMS_TP_VPF_RELATIVE |
		(src->ask_ds ? GSMD_SMS_TP_SRR_STATUS_REQUEST :
		 GSMD_SMS_TP_SRR_NOT_REQUEST) |
		(src->payload.has_header ? GSMD_SMS_TP_UDHI_WITH_HEADER :
		 GSMD_SMS_TP_UDHI_NO_HEADER) |
		GSMD_SMS_TP_RP_NOT_SET;

	/* TP-Message-Reference - 00 lets the phone set the number itself */
	header[pos ++] = 0x00;

	header[pos ++] = strlen(src->addr.number);
	header[pos ++] = src->addr.type;
	for (i = 0; src->addr.number[i]; i ++) {
		header[pos] = src->addr.number[i ++] - '0';
		if (src->addr.number[i])
			header[pos ++] |= (src->addr.number[i] - '0') << 4;
		else {
			header[pos ++] |= 0xf0;
			break;
		}
	}

	/* TP-Protocol-Identifier - 00 means implicit */
	header[pos ++] = 0x00;

	/* TP-Data-Coding-Scheme */
	header[pos ++] = sms_pdu_encode_dcs((struct gsmd_sms_datacodingscheme *) &src->payload.dcs);

	/* TP-Validity-Period */
	/* using relative VP since some Vodafone SIMs appear not to like it if VP is missing */
	header[pos ++] = 0xFF;

	if (src->payload.dcs.alphabet == SMS_ALPHABET_7_BIT_DEFAULT) {
		/* For 7bit, UDL gives number of septets */
		header[pos ++] = src->payload.size_encoded_userdata;
		DEBUGP("size_encoded_userdata %d\n",src->payload.size_encoded_userdata);
	} else {
		/* Otherwise, UDL gives number of octets */
		header[pos ++] = src->payload.physical_byte_length;
        DEBUGP("physical_byte_length %d\n",src->payload.physical_byte_length);
	}

	len = src->payload.physical_byte_length;

	if (dest) {
		for (i = 0; i < pos; i ++) {
			sprintf(dest, "%02X", header[i]);
			dest += 2;
		}

		convert_pdu_to_text(dest, src->payload.data, len);
	}

	return pos + len;
}

/* Refer to GSM 03.41 subclause 9.3 */
int cbs_pdu_to_msg(struct gsmd_cbm *dst, u_int8_t *src, int pdulen, int len)
{
	if (len != pdulen || len != CBM_MAX_PDU_SIZE)
		return 1;

	dst->serial.scope = (src[0] >> 6) & 3;
	dst->serial.msg_code = ((src[0] << 4) | (src[1] >> 4)) & 0x3ff;
	dst->serial.update_num = src[1] & 0xf;

	dst->msg_id = (src[2] << 8) | src[3];

	dst->language = src[4] & 0xf;
	dst->coding_scheme = ((src[4] >> 4) & 3) << 2;

	dst->pages = src[5] & 0xf;
	dst->page = src[5] >> 4;

	memcpy(dst->data, src + 6, len - 6);
	return 0;
}

/* Refer to GSM 03.38 Clause 4, for TP-DCS */

u_int8_t sms_pdu_encode_dcs(struct gsmd_sms_datacodingscheme *dcs)
{
	/* pattern 0001 xxxx */
	/* 0001 = Bits 1 and 0 have a message class meaning */
	return (u_int8_t) (0x10 + (dcs->alphabet<<2) + dcs->msg_class);
}

int sms_pdu_decode_dcs(struct gsmd_sms_datacodingscheme *dcs,
	const u_int8_t *data)
{
	int pos = 0, i;

	/* init dcs value */
	dcs->mwi_active		= NOT_ACTIVE;
	dcs->mwi_kind		= MESSAGE_WAITING_OTHER;

	/* bits 7-6 */
	i = ( data[pos] & 0xC0 ) >> 6;
	switch( i )
	{
	case 0: /* pattern 00xx xxxx */
		dcs->is_compressed = data[pos] & 0x20;
		if( data[pos] & 0x10 )
			dcs->msg_class = data[pos] & 0x03;
		else
			/* no class information */
			dcs->msg_class = MSG_CLASS_NONE;
		dcs->alphabet  = ( data[pos] & 0x0C ) >> 2;
		dcs->mwi_group 	= MESSAGE_WAITING_NONE;
		break;
	case 3: /* pattern 1111 xxxx */
		/* bits 5-4 */
		if( (data[pos] & 0x30) == 0x30 )
		{
			/* bit 3 is reserved */
			/* bit 2 */
			dcs->alphabet = (data[pos] & 0x04 ) ? SMS_ALPHABET_8_BIT:
					   SMS_ALPHABET_7_BIT_DEFAULT;
			/* bits 1-0 */
			dcs->msg_class = data[pos] & 0x03;
			/* set remaining fields */
			dcs->is_compressed  = NOT_COMPRESSED;
			dcs->mwi_group    = MESSAGE_WAITING_NONE_1111;
		}
		else
		{
			/* Message waiting groups */
			dcs->is_compressed  = NOT_COMPRESSED;
			dcs->msg_class      = MSG_CLASS_NONE;
			/* bits 5-4 */
			if( (data[pos] & 0x30) == 0x00 )
			{
				dcs->mwi_group  = MESSAGE_WAITING_DISCARD;
				dcs->alphabet   = SMS_ALPHABET_7_BIT_DEFAULT;
			}
			else if( (data[pos] & 0x30) == 0x10 )
			{
				dcs->mwi_group  = MESSAGE_WAITING_STORE;
				dcs->alphabet   = SMS_ALPHABET_7_BIT_DEFAULT;
			}
			else
			{
				dcs->mwi_group  = MESSAGE_WAITING_STORE;
				dcs->alphabet   = SMS_ALPHABET_UCS2;
			}
			/* bit 3 */
			dcs->mwi_active = ( data[pos] & 0x08 ) ? ACTIVE :
				NOT_ACTIVE;
			/* bit 2 is reserved */
			/* bits 1-0 */
			dcs->mwi_kind = data[pos] & 0x03;
		}
		break;
	default:
		/* reserved values	*/
		dcs->msg_class      	= MSG_CLASS_NONE;
		dcs->alphabet       	= SMS_ALPHABET_7_BIT_DEFAULT;
		dcs->is_compressed  	= NOT_COMPRESSED;
		dcs->mwi_group    	= MESSAGE_WAITING_NONE;
		dcs->mwi_active		= NOT_ACTIVE;
		dcs->mwi_kind		= MESSAGE_WAITING_OTHER;
		break;
	}

	if ( dcs->alphabet > SMS_ALPHABET_UCS2 )
		dcs->alphabet = SMS_ALPHABET_7_BIT_DEFAULT;
	/* keep raw dcs data*/
	dcs->raw_dcs_data = data[pos];
	return 0;
}
