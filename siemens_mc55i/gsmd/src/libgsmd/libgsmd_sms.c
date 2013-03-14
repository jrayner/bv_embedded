/* libgsmd_sms.c
 *
 * Copyright (C) 2007-2009 Jim Rayner <jimr@beyondvoice.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>

#include <libgsmd/libgsmd.h>
#include <libgsmd/misc.h>
#include <libgsmd/sms.h>

#include <gsmd/usock.h>
#include <gsmd/event.h>

#include "lgsm_internals.h"

int lgsm_sms_list(struct lgsm_handle *lh, enum gsmd_msg_sms_type stat)
{
	struct gsmd_msg_hdr *gmh;

	gmh = lgsm_gmh_fill(GSMD_MSG_SMS,
			GSMD_SMS_LIST, sizeof(int));
	if (!gmh)
		return -ENOMEM;

	*(int *) gmh->data = stat;

	return lgsm_send_then_free_gmh(lh, gmh);
}

int lgsm_sms_read(struct lgsm_handle *lh, int index)
{
	struct gsmd_msg_hdr *gmh;

	gmh = lgsm_gmh_fill(GSMD_MSG_SMS,
			GSMD_SMS_READ, sizeof(int));
	if (!gmh)
		return -ENOMEM;

	*(int *) gmh->data = index;

	return lgsm_send_then_free_gmh(lh, gmh);
}

int lgsm_sms_delete(struct lgsm_handle *lh,
		const struct lgsm_sms_delete *sms_del)
{
	struct gsmd_msg_hdr *gmh;
	struct gsmd_sms_delete *gsd;

	gmh = lgsm_gmh_fill(GSMD_MSG_SMS,
			GSMD_SMS_DELETE, sizeof(*gsd));
	if (!gmh)
		return -ENOMEM;

	gsd = (struct gsmd_sms_delete *) gmh->data;
	gsd->index = sms_del->index;
	gsd->delflg = sms_del->delflg;

	return lgsm_send_then_free_gmh(lh, gmh);
}

int lgsm_number2addr(struct gsmd_addr *dst, const char *src, int skipplus)
{
	char *ch;

	if (strlen(src) + 1 > sizeof(dst->number))
		return 1;
	if (src[0] == '+') {
		dst->type =
			GSMD_TOA_NPI_ISDN |
			GSMD_TOA_TON_INTERNATIONAL |
			GSMD_TOA_RESERVED;
		strcpy(dst->number, src + skipplus);
	} else {
		dst->type =
			GSMD_TOA_NPI_ISDN |
			GSMD_TOA_TON_UNKNOWN |
			GSMD_TOA_RESERVED;
		strcpy(dst->number, src);
	}

	for (ch = dst->number; *ch; ch ++)
		if (*ch < '0' || *ch > '9')
			return 1;
	return 0;
}

static int copy_sms(
		const struct lgsm_sms *sms, struct gsmd_sms_submit* submit, int pdu_user_header)
{
	if (lgsm_number2addr(&submit->addr, sms->addr, 1))
		return -EINVAL;

	submit->ask_ds = sms->ask_ds;
	submit->payload.has_header = pdu_user_header;
	submit->payload.physical_byte_length = sms->physical_byte_length;
	submit->payload.size_encoded_userdata = sms->size_encoded_userdata;
	submit->payload.dcs.alphabet = sms->dcs.alphabet;
	submit->payload.dcs.msg_class = sms->dcs.msg_class;
	memcpy(submit->payload.data, sms->data, LGSM_SMS_DATA_MAXLEN);

	return 0;
}

int lgsm_sms_send(struct lgsm_handle *lh,
        const struct lgsm_sms *sms, int pdu_user_header)
{
	/* FIXME: only support PDU mode */
	int retval = 0;
	struct gsmd_msg_hdr *gmh;
	struct gsmd_sms_submit *gss;
	
	gmh = lgsm_gmh_fill(GSMD_MSG_SMS,
		GSMD_SMS_SEND, sizeof(*gss));
	if (!gmh)
		return -ENOMEM;

	gss = (struct gsmd_sms_submit *) gmh->data;
	
	retval = copy_sms(sms, gss, pdu_user_header);
	if (retval)
		lgsm_gmh_free(gmh);
	else
		retval = lgsm_send_then_free_gmh(lh, gmh);
	
	return retval;
}

int lgsm_sms_write(struct lgsm_handle *lh,
		const struct lgsm_sms_write *sms_write, int pdu_user_header)
{
	int retval = 0;
	struct gsmd_msg_hdr *gmh;
	struct gsmd_sms_write *gsw;

	gmh = lgsm_gmh_fill(GSMD_MSG_SMS,
			GSMD_SMS_WRITE, sizeof(*gsw));
	if (!gmh)
		return -ENOMEM;

	gsw = (struct gsmd_sms_write *) gmh->data;
	gsw->stat = sms_write->stat;

	retval = copy_sms(&sms_write->sms, &gsw->sms, pdu_user_header);
	if (retval)
		lgsm_gmh_free(gmh);
	else
		retval = lgsm_send_then_free_gmh(lh, gmh);

	return retval;
}

int lgsm_ack_sms(struct lgsm_handle *lh, int ack)
{
#if 1
	// Gsmd now sends an ack irrespective of our processing. Due to the lack of
	// a proper Naking interface on the Siemens MC55i, sometimes the modem was
	// naking messages as the ack request taking to long to get to the modem.
	return 0;
#else
	struct gsmd_msg_hdr *gmh;
	
	gmh = lgsm_gmh_fill(GSMD_MSG_SMS,
		GSMD_SMS_ACK, sizeof(int));
	if (!gmh)
		return -ENOMEM;

	*(int *) gmh->data = ack;
	
	return lgsm_send_then_free_gmh(lh, gmh);
#endif
}

int lgsm_sms_get_storage(struct lgsm_handle *lh)
{
	return lgsm_send_simple(lh, GSMD_MSG_SMS, GSMD_SMS_GET_MSG_STORAGE);
}

int lgsm_sms_set_storage(struct lgsm_handle *lh, enum ts0705_mem_type mem1,
		enum ts0705_mem_type mem2, enum ts0705_mem_type mem3)
{
	struct gsmd_msg_hdr *gmh =
		lgsm_gmh_fill(GSMD_MSG_SMS, GSMD_SMS_SET_MSG_STORAGE,
				3 * sizeof(enum ts0705_mem_type));
	if (!gmh)
		return -ENOMEM;

	((enum ts0705_mem_type *) gmh->data)[0] = mem1;
	((enum ts0705_mem_type *) gmh->data)[1] = mem2;
	((enum ts0705_mem_type *) gmh->data)[2] = mem3;

	return lgsm_send_then_free_gmh(lh, gmh);
}

int lgsm_sms_get_smsc(struct lgsm_handle *lh)
{
	return lgsm_send_simple(lh, GSMD_MSG_SMS, GSMD_SMS_GET_SERVICE_CENTRE);
}

int lgsm_sms_set_smsc(struct lgsm_handle *lh, const char *number)
{
	struct gsmd_msg_hdr *gmh =
		lgsm_gmh_fill(GSMD_MSG_SMS, GSMD_SMS_SET_SERVICE_CENTRE,
				sizeof(struct gsmd_addr));
	if (!gmh)
		return -ENOMEM;

	if (lgsm_number2addr((struct gsmd_addr *) gmh->data, number, 0)) {
		lgsm_gmh_free(gmh);
		return -EINVAL;
	}

	return lgsm_send_then_free_gmh(lh, gmh);
}

int lgsm_sms_cancel(struct lgsm_handle *lh, int subtype)
{
	switch(subtype) {
	case GSMD_SMS_LIST:
	case GSMD_SMS_READ:
	case GSMD_SMS_SEND:
	case GSMD_SMS_WRITE:
	case GSMD_SMS_DELETE:
	case GSMD_SMS_GET_MSG_STORAGE:
	case GSMD_SMS_SET_MSG_STORAGE:
	case GSMD_SMS_GET_SERVICE_CENTRE:
	case GSMD_SMS_SET_SERVICE_CENTRE:
	case GSMD_SMS_ACK:
		return lgsm_cancel(lh, GSMD_MSG_SMS, subtype, 0);
	}
	return -EINVAL;
}

int lgsm_convert_ucs2_to_utf8(const unsigned short* ucs2_src, const int len,
	unsigned char* dest)
{
	int loop;
	int count = 0;
	for(loop=0; loop < len; loop++) {
		unsigned char* ptr = (unsigned char*) &ucs2_src[loop];
		unsigned char y = *ptr;
		unsigned char x = *(ptr+1);
		if (y < 0x08) {
			if ((!y)&&(x < 0x80)) {
				/* 0-7F = 0xxxxxxx */
				if (dest)
					dest[count] = x;
				count++;
			} else {
				/* C2-DF = 110yyyxx 10xxxxxx */
				if (dest) {
					dest[count] = 0xC0 + (y << 2) + ((x >> 6) & 0x3);
					dest[count + 1] = 0x80 + (x  & 0x3f);
				}
				count+= 2;
			}
		} else {
			/* E0-EF = 1110yyyy 10yyyyxx 10xxxxxx */
			if (dest) {
				dest[count] = 0xE0 + ((y >> 4) & 0xf);
				dest[count + 1] = 0x80 + ((y << 2)  & 0x3c) + ((x >> 6) & 0x3);
				dest[count + 2] = 0x80 + (x  & 0x3f);
			}
			count+= 3;
		}
	}
	return count;
}

int lgsm_convert_utf8_to_ucs2(const unsigned char* src, const int len,
	unsigned short* dest)
{
	int loop = 0;
	int count = 0;
	unsigned short val;
	while (loop < len) {
		/* 0-7F = 0xxxxxxx */
		if (src[loop] < 0x80) {
			val = src[loop++];
		}
		/* E0-EF = 1110yyyy 10yyyyxx 10xxxxxx */
		else if (src[loop] > 0xDF) {
			if (dest) {
				unsigned char a,b,c, y, x;
				a = src[loop++] & 0xf;
				b = src[loop++] & 0x3f;
				c = src[loop++] & 0x3f;
				y = (a << 4) + ((b >> 2) & 0xf);
				x = (b << 6) + c;
				val = (y << 8) + x;
			}
			else
				loop += 3;
		}
		/* C2-DF = 110yyyxx 10xxxxxx */
		else if (src[loop] > 0xC1) {
			if (dest) {
				unsigned char a,b, y, x;
				a = src[loop++] & 0x1f;
				b = src[loop++] & 0x3f;
				y = ((a >> 2) & 0xf);
				x = (a << 6) + b;
				val = (y << 8) + x;
			}
			else
				loop += 2;
		}
		else {
			return 0;
		}
		if (dest) {
			/* todo need endianess check */
			unsigned char* v1 = ((unsigned char*) &val);
			unsigned char* v2 = ((unsigned char*) &val) + 1;
			unsigned short tmp = *v1;
			tmp <<= 8;
			tmp += *v2;
			*(dest++) = tmp;
		}
		count++;
	}
	return count;
}

static unsigned char mapping_gsm338_to_utf8[128] = {
  0,   1,   3,   4,   6,   8,  10,  12,  14,  16,  18,  19,  21,  23,  24,  26,
 28,  30,  31,  33,  35,  37,  39,  41,  43,  45,  47,  49,  51,  53,  55,  57,
 59,  60,  61,  62,  63,  65,  66,  67,  68,  69,  70,  71,  72,  73,  74,  75,
 76,  77,  78,  79,  80,  81,  82,  83,  84,  85,  86,  87,  88,  89,  90,  91,
 92,  94,  95,  96,  97,  98,  99, 100, 101, 102, 103, 104, 105, 106, 107, 108,
109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119, 120, 122, 124, 126, 128,
130, 132, 133, 134, 135, 136, 137, 138, 139, 140, 141, 142, 143, 144, 145, 146,
147, 148, 149, 150, 151, 152, 153, 154, 155, 156, 157, 158, 160, 162, 164, 166
};

static unsigned char gsm338_to_utf8[168] = {
0x40, 0xC2, 0xA3, 0x24, 0xC2, 0xA5, 0xC3, 0xA8, 0xC3, 0xA9, 0xC3, 0xB9, 0xC3,
0xAC, 0xC3, 0xB2, 0xC3, 0xA7, 0x0A, 0xC3, 0x98, 0xC3, 0xB8, 0x0D, 0xC3, 0x85,
0xC3, 0xA5, 0xCE, 0x94, 0x5F, 0xCE, 0xA6, 0xCE, 0x93, 0xCE, 0x9B, 0xCE, 0xA9,
0xCE, 0xA0, 0xCE, 0xA8, 0xCE, 0xA3, 0xCE, 0x98, 0xCE, 0x9E, 0xC2, 0xA0, 0xC3,
0x86, 0xC3, 0xA6, 0xC3, 0x9F, 0xC3, 0x89, 0x20, 0x21, 0x22, 0x23, 0xC2, 0xA4,
0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F, 0x30, 0x31,
0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E,
0x3F, 0xC2, 0xA1, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A,
0x4B, 0x4C, 0x4D, 0x4E, 0x4F, 0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57,
0x58, 0x59, 0x5A, 0xC3, 0x84, 0xC3, 0x96, 0xC3, 0x91, 0xC3, 0x9C, 0xC2, 0xA7,
0xC2, 0xBF, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x6B,
0x6C, 0x6D, 0x6E, 0x6F, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78,
0x79, 0x7A, 0xC3, 0xA4, 0xC3, 0xB6, 0xC3, 0xB1, 0xC3, 0xBC, 0xC3, 0xA0
};

int lgsm_convert_gsm338_to_utf8(const unsigned char* src, const int len,
	unsigned char* dest)
{
	int loop;
	int count = 0;
	wchar_t ucs2 = 0;

	for(loop=0; loop < len; loop++) {

		unsigned char gsm338_val = src[loop];

		if (gsm338_val == 0x1B) { /* escape character */
			if (loop + 1 < len) {
				unsigned char utf8_val = 0;
				switch(src[++loop]) {
				case 0x0A: utf8_val = 0x0C; break; /* FORM FEED */
				case 0x14: utf8_val = 0x5E; break; /* CIRCUMFLEX ACCENT */
				case 0x28: utf8_val = 0x7B; break; /* LEFT CURLY BRACKET */
				case 0x29: utf8_val = 0x7D; break; /* RIGHT CURLY BRACKET */
				case 0x2F: utf8_val = 0x5C; break; /* REVERSE SOLIDUS */
				case 0x3C: utf8_val = 0x5B; break; /* LEFT SQUARE BRACKET */
				case 0x3D: utf8_val = 0x7E; break; /* TILDE */
				case 0x3E: utf8_val = 0x5D; break; /* RIGHT SQUARE BRACKET */
				case 0x40: utf8_val = 0x7C; break; /* VERTICAL LINE */
				case 0x65: /* EURO SIGN */
					if (dest) {
						dest[count] = 0xE2;
						dest[count+1] = 0x82;
						dest[count+2] = 0xAC;
					}
					count += 3;
					break;
				default:
					loop--; /* only a single char */
					if (dest) {
						dest[count] = 0xC2;
						dest[count+1] = 0xA0;
					}
					count += 2;
					break;
				}
				if (utf8_val) {
					if (dest)
						dest[count] = utf8_val;
					count++;
				}
			} else {
				/* Since at end, must be a single no-break space */
				if (dest) {
					dest[count] = 0xC2;
					dest[count+1] = 0xA0;
				}
				count += 2;
			}
		} else {
			if (gsm338_val < 0x80) {
				unsigned char pos = mapping_gsm338_to_utf8[gsm338_val];
				unsigned char* utf8_ptr = gsm338_to_utf8 + pos;
				if (dest)
					dest[count] = *utf8_ptr;
				count++;
				if (*utf8_ptr >= 0xC2) {
					if (dest)
						dest[count] = *(utf8_ptr + 1);
					count++;
				}
			} else {
				/* 8 bit value yet gsm338 is a 7bit encoding */
				return -1;
			}
		}
	}
	return count;
}

int lgsm_convert_utf8_to_gsm338(const unsigned char* src, const int len,
	unsigned char* dest)
{
	int loop;
	int retval = 0;
	int count = 0;
	unsigned char gsm338;

	for(loop=0; loop < len; loop++) {

		unsigned char utf8_val = src[loop];
		if (utf8_val < 0x7F) {
			gsm338 = utf8_val;
			if (utf8_val > 0x1F) {
				if (utf8_val == 0x24) /* $ */
					gsm338 = 0x02;
				else if (utf8_val == 0x40) /* @ */
					gsm338 = 0x00;
				else if ((utf8_val > 0x5A)&&(utf8_val < 0x5F)) {
					if (dest)
						dest[count] = 0x1B; /* escape sequence */
					count++;
					switch (utf8_val) {
					case 0x5B: gsm338 = 0x3C; break; /* LEFT SQUARE BRACKET */
					case 0x5C: gsm338 = 0x2F; break; /* REVERSE SOLIDUS */
					case 0x5D: gsm338 = 0x3E; break; /* RIGHT SQUARE BRACKET */
					case 0x5E: gsm338 = 0x14; break; /* CIRCUMFLEX ACCENT */
					}
				} else if (utf8_val == 0x5F) /* _ */
					gsm338 = 0x11;
				else if (utf8_val > 0x7A) {
					if (dest)
						dest[count] = 0x1B; /* escape sequence */
					count++;
					switch (utf8_val) {
					case 0x7B: gsm338 = 0x28; break; /* LEFT CURLY BRACKET */
					case 0x7C: gsm338 = 0x40; break; /* VERTICAL LINE */
					case 0x7D: gsm338 = 0x29; break; /* RIGHT CURLY BRACKET */
					case 0x7E: gsm338 = 0x3D; break; /* TILDE */
					default: retval = -1; break;
					}
				} else if (utf8_val == 0x60)
					retval = -1;
			} else if (utf8_val == 0x0C) {
				if (dest)
					dest[count] = 0x1B; /* escape sequence */
				count++;
				gsm338 = 0x0A; /* FORM FEED */
			} else if ((utf8_val != 0x0A)&&(utf8_val != 0x0D))
				retval = -1;
		} else {
			if (utf8_val == 0xC2) {
				utf8_val = src[++loop];
				switch (utf8_val) {
				case 0xA3: gsm338 = 0x01; break; /* POUND SIGN */
				case 0xA5: gsm338 = 0x03; break; /* YEN SIGN */
				case 0xA0: gsm338 = 0x1B; break; /* no-break space character */
				case 0xA4: gsm338 = 0x24; break; /* CURRENCY SIGN */
				case 0xA1: gsm338 = 0x40; break; /* INVERTED EXCLAMATION MARK */
				case 0xA7: gsm338 = 0x5F; break; /* SECTION SIGN */
				case 0xBF: gsm338 = 0x60; break; /* INVERTED QUESTION MARK */
				default: retval = -1; break;
				}
			} else if (utf8_val == 0xC3) {
				utf8_val = src[++loop];
				switch (utf8_val) { /* Latin chars */
				case 0xA8: gsm338 = 0x04; break; /* SMALL E WITH GRAVE */
				case 0xA9: gsm338 = 0x05; break; /* SMALL E WITH ACUTE */
				case 0xB9: gsm338 = 0x06; break; /* SMALL U WITH GRAVE */
				case 0xAC: gsm338 = 0x07; break; /* SMALL I WITH GRAVE */
				case 0xB2: gsm338 = 0x08; break; /* SMALL O WITH GRAVE */
				case 0xA7: gsm338 = 0x09; break; /* SMALL C WITH CEDILLA */
				case 0x98: gsm338 = 0x0B; break; /* CAPITAL O WITH STROKE */
				case 0xB8: gsm338 = 0x0C; break; /* SMALL O WITH STROKE */
				case 0x85: gsm338 = 0x0E; break; /* CAPITAL A WITH RING ABOVE */
				case 0xA5: gsm338 = 0x0F; break; /* SMALL A WITH RING ABOVE */
				case 0x86: gsm338 = 0x1C; break; /* CAPITAL AE */
				case 0xA6: gsm338 = 0x1D; break; /* SMALL AE */
				case 0x9F: gsm338 = 0x1E; break; /* SMALL SHARP S (German) */
				case 0x89: gsm338 = 0x1F; break; /* CAPITAL E WITH ACUTE */
				case 0x84: gsm338 = 0x5B; break; /* CAPITAL A WITH DIAERESIS */
				case 0x96: gsm338 = 0x5C; break; /* CAPITAL O WITH DIAERESIS */
				case 0x91: gsm338 = 0x5D; break; /* CAPITAL N WITH TILDE */
				case 0x9C: gsm338 = 0x5E; break; /* CAPITAL U WITH DIAERESIS */
				case 0xA4: gsm338 = 0x7B; break; /* SMALL A WITH DIAERESIS */
				case 0xB6: gsm338 = 0x7C; break; /* SMALL O WITH DIAERESIS */
				case 0xB1: gsm338 = 0x7D; break; /* SMALL N WITH TILDE */
				case 0xBC: gsm338 = 0x7E; break; /* SMALL U WITH DIAERESIS */
				case 0xA0: gsm338 = 0x7F; break; /* CAPITAL GRAVE */
				default: retval = -1; break;
				}
			} else if (utf8_val == 0xCE) {
				utf8_val = src[++loop];
				switch (utf8_val) {
				case 0x94: gsm338 = 0x10; break; /* DELTA */
				case 0xA6: gsm338 = 0x12; break; /* PHI */
				case 0x93: gsm338 = 0x13; break; /* GAMMA */
				case 0x9B: gsm338 = 0x14; break; /* LAMDA */
				case 0xA9: gsm338 = 0x15; break; /* OMEGA */
				case 0xA0: gsm338 = 0x16; break; /* PI */
				case 0xA8: gsm338 = 0x17; break; /* PSI */
				case 0xA3: gsm338 = 0x18; break; /* SIGMA */
				case 0x98: gsm338 = 0x19; break; /* THETA */
				case 0x9E: gsm338 = 0x1A; break; /* XI */
				default: retval = -1; break;
				}
			} else if (utf8_val == 0xE2) {
				if ((src[++loop] == 0x82)&&(src[++loop] == 0xAC)) {
					if (dest)
						dest[count] = 0x1B; /* escape sequence */
					count++;
					gsm338 = 0x65; /* EURO SIGN */
				} else
					retval = -1;
			} else {
				retval = -1;
			}
		}
		if (retval) {
			return -1;
		}

		if (dest)
			dest[count] = gsm338;
		count++;
	}
	return count;
}

int packing_7bit_character(const char *src, u_int8_t text_length,
	struct lgsm_sms *dest, u_int8_t header)
{
	int i,j = 0;
	unsigned char ch1, ch2;
	int shift = 0;
	int start_loop = 0;
	int filler_bit = 0;

	dest->dcs.alphabet = SMS_ALPHABET_7_BIT_DEFAULT;
	dest->size_encoded_userdata = text_length;

	if (header) {
		j = header;
		shift = header % 7;
		dest->size_encoded_userdata += (header << 3) / 7;
		if (shift) {
			/* filler septet */
			dest->size_encoded_userdata++;
			start_loop--;
			filler_bit = 1;
		}
	}

	for ( i=start_loop; i< text_length; i++ ) {

		if (filler_bit) {
			ch1 = 0x7F;
			filler_bit = 0;
		} else
			ch1 = src[i] & 0x7F;
		ch1 = ch1 >> shift;
		ch2 = src[(i+1)] & 0x7F;
		ch2 = ch2 << (7-shift);

		ch1 = ch1 | ch2;

		if (j > sizeof(dest->data))
			break;
		dest->data[j++] = ch1;

		shift++;

		if ( 7 == shift ) {
			shift = 0;
			i++;
		}
	}

	return j;
}

int unpacking_7bit_character(const struct gsmd_sms *src, unsigned char *out_ptr)
{
	unsigned char shift = 0;
	unsigned char second_part = 0;
	unsigned char loop = 0;
	unsigned int filler_bit = 0;
	unsigned int count = 0;
	unsigned int num_bytes = src->size_encoded_userdata;
	unsigned char* dest = out_ptr;

	if (src->has_header) {
		unsigned int udhl = (unsigned int) src->data[0];
		loop = udhl + 1;
	        if (udhl>GSMD_SMS_DATA_MAXLEN) return -1;

		memcpy(dest, src->data,loop);
		dest+=loop;

		int bits_used = (udhl + 1) << 3;
		shift = bits_used % 7;
		if (shift) {
			filler_bit = 1;
			num_bytes--;
		}
	}

	*dest = 0;
	for (; loop < src->physical_byte_length; loop++) {
		if (!filler_bit) {
			unsigned char first_mask = (0x7f >> shift);
			unsigned char first_part = src->data[loop] & first_mask;
			first_part <<= shift;
			*(dest++) |= first_part;
		}
		filler_bit = 0;

		second_part = src->data[loop];
		second_part >>= 7 - shift;
		*dest = second_part;

		shift++;
		if (7 == shift) {
			shift = 0;
			*(++dest) = '\0';
		}
	}

	return num_bytes;
}


int cbm_unpacking_7bit_character(const char *src, unsigned char *dest)
{
	int i;
	u_int8_t ch = 1;

	for (i = 0; i < 93 && ch; i ++)
		*(dest ++) = ch =
			((src[(i * 7 + 7) >> 3] << (7 - ((i * 7 + 7) & 7))) |
			 (src[(i * 7) >> 3] >> ((i * 7) & 7))) & 0x7f;
	*dest = '\0';

	return i;
}

/* Refer to 3GPP TS 11.11 Annex B */
int packing_UCS2_80(char *src, char *dest)
{
	return 0;
}

/* Refer to 3GPP TS 11.11 Annex B */
int unpacking_UCS2_80(char *src, char *dest)
{
	return 0;
}

/* Refer to 3GPP TS 11.11 Annex B */
int packing_UCS2_81(char *src, char *dest)
{
	return 0;
}

/* Refer to 3GPP TS 11.11 Annex B */
int unpacking_UCS2_81(char *src, char *dest)
{
	return 0;
}

/* Refer to 3GPP TS 11.11 Annex B */
int packing_UCS2_82(char *src, char *dest)
{
	return 0;
}

/* Refer to 3GPP TS 11.11 Annex B */
int unpacking_UCS2_82(char *src, char *dest)
{
	return 0;
}

int lgsm_cb_subscribe(struct lgsm_handle *lh)
{
	return lgsm_send_simple(lh, GSMD_MSG_CB, GSMD_CB_SUBSCRIBE);
}

int lgsm_cb_unsubscribe(struct lgsm_handle *lh)
{
	return lgsm_send_simple(lh, GSMD_MSG_CB, GSMD_CB_UNSUBSCRIBE);
}

int lgsm_cb_cancel(struct lgsm_handle *lh, int subtype)
{
	switch(subtype) {
	case GSMD_CB_SUBSCRIBE:
	case GSMD_CB_UNSUBSCRIBE:
		return lgsm_cancel(lh, GSMD_MSG_CB, subtype, 0);
	}
	return -EINVAL;
}
