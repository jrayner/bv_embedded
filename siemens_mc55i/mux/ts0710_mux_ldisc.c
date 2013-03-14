/*
 * File: mux_driver.c
 *
 * Portions derived from rfcomm.c, original header as follows:
 *
 * Copyright (C) 2000, 2001  Axis Communications AB
 *
 * Author: Mats Friden <mats.friden@axis.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 *
 * Exceptionally, Axis Communications AB grants discretionary and
 * conditional permissions for additional use of the text contained
 * in the company's release of the AXIS OpenBT Stack under the
 * provisions set forth hereunder.
 *
 * Provided that, if you use the AXIS OpenBT Stack with other files,
 * that do not implement functionality as specified in the Bluetooth
 * System specification, to produce an executable, this does not by
 * itself cause the resulting executable to be covered by the GNU
 * General Public License. Your use of that executable is in no way
 * restricted on account of using the AXIS OpenBT Stack code with it.
 *
 * This exception does not however invalidate any other reasons why
 * the executable file might be covered by the provisions of the GNU
 * General Public License.
 *
 */
/*
 * Copyright (C) 2002-2004  Motorola
 * Copyright (C) 2006 Harald Welte <laforge@openezx.org>
 * Copyright (C) 2007-2010 Jim Rayner <jimr@beyondvoice.com>
 *
 *  07/28/2002  Initial version
 *  11/18/2002  Second version
 *  04/21/2004  Add GPRS PROC
 */

/* Note this code currently reuses the N_MOUSE ldisc for now, TODO - need own
   line discipline ident */

/* #define TS0710DEBUG */
/* #define TS0710INFO */
#define TS0710ERROR

#include <linux/version.h>

#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,19))
#include <linux/config.h>
#endif

#include <linux/module.h>
#include <linux/types.h>

#include <linux/kernel.h>
#include <linux/proc_fs.h>

#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/interrupt.h>

#include <linux/tty.h>
#include <linux/tty_driver.h>
#include <linux/tty_flip.h>
#include <linux/tty_ldisc.h>

#include <linux/workqueue.h>
#include <linux/fcntl.h>
#include <linux/string.h>
#include <linux/major.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/init.h>

#include <asm/uaccess.h>
#include <asm/system.h>
#include <asm/bitops.h>
#include <linux/spinlock.h>
#include <asm/termios.h> /* defined N_... ldiscs */
#include <linux/poll.h>

#include "ts0710_mux_ldisc.h"


#define TS0710MUX_MAJOR 250

/* We map the tty index to the dlci. Therefore do not allow the tty index 0 */
#define TS0710MUX_MINOR_START 1

#define NR_MUXS TS0710_MAX_CHN

#define TS0710MUX_TIME_OUT 250	/* 2500ms */

#define SHORT_PAYLOAD_SIZE 127

#define TS0710_MAX_HDR_SIZE 5
#define SHORT_UIH_MTU_HDR   6   /* [f9 dlc ctrl len] data [crc f9] */
#define DEF_TS0710_MTU 98	/* Siemens defaults to 98 for mtu */

#define TS0710MUX_SEND_BUF_OFFSET 10
#define TS0710MUX_SEND_BUF_SIZE (DEF_TS0710_MTU + TS0710MUX_SEND_BUF_OFFSET + 34)
#define TS0710MUX_RECV_BUF_SIZE TS0710MUX_SEND_BUF_SIZE
#define TS0710MUX_MAX_BUF_SIZE TS0710MUX_SEND_BUF_SIZE

#define TS0710MUX_MAX_RECV_BUF_SIZE 2048

#define TS0710MUX_SERIAL_BUF_SIZE (DEF_TS0710_MTU + TS0710_MAX_HDR_SIZE)

#define TS0710MUX_MAX_TOTAL_FRAME_SIZE (DEF_TS0710_MTU + TS0710_MAX_HDR_SIZE + FLAG_SIZE)
#define TS0710MUX_MAX_CHARS_IN_BUF 65535
#define TS0710MUX_THROTTLE_THRESHOLD DEF_TS0710_MTU

#define SET_PF(ctr) ((ctr) | (1 << 4))
#define CLR_PF(ctr) ((ctr) & 0xef)
#define GET_PF(ctr) (((ctr) >> 4) & 0x1)

#define GET_PN_MSG_FRAME_SIZE(pn) ( ((pn)->frame_sizeh << 8) | ((pn)->frame_sizel))
#define SET_PN_MSG_FRAME_SIZE(pn, size) ({ (pn)->frame_sizel = (size) & 0xff; \
                                           (pn)->frame_sizeh = (size) >> 8; })

#define GET_LONG_LENGTH(a) ( ((a).h_len << 7) | ((a).l_len) )
#define SET_LONG_LENGTH(a, length) ({ (a).ea = 0; \
                                      (a).l_len = length & 0x7F; \
                                      (a).h_len = (length >> 7) & 0xFF; })

#define SHORT_CRC_CHECK 3
#define LONG_CRC_CHECK 4

#define EA 1
#define FCS_SIZE 1
#define FLAG_SIZE 2

#define TS0710_BASIC_FLAG 0xF9

/* the control field */
#define SABM 0x2f
#define SABM_SIZE 4
#define UA 0x63
#define UA_SIZE 4
#define DM 0x0f
#define DISC 0x43
#define UIH 0xef

/* the type field in a multiplexer command packet */
#define TEST 0x8
#define FCON 0x28
#define FCOFF 0x18
#define MSC 0x38
#define CLD 0x30
#define RPN 0x24
#define RLS 0x14
#define PN 0x20
#define NSC 0x4

/* V.24 modem control signals */
#define FC 0x2
#define RTC 0x4
#define RTR 0x8
#define IC 0x40
#define DV 0x80

#define CTRL_CHAN 0		/* The control channel is defined as DLCI 0 */
#define MCC_CMD 1		/* Multiplexer command cr */
#define MCC_RSP 0		/* Multiplexer response cr */

#define ADDRESS_FIELD_OFFSET 1
#define DLC_OFFSET 1

#define TEST_PATTERN_SIZE 250

#ifndef UNUSED_PARAM
#define UNUSED_PARAM(v) (void)(v)
#endif

/* Bit number in flags of mux_send_struct */
#define BUF_BUSY 0

#define RECV_RUNNING 0

#ifdef min
#undef min
#define min(a,b)    ( (a)<(b) ? (a):(b) )
#endif

typedef struct {
	volatile __u8 buf[TS0710MUX_SEND_BUF_SIZE];
	volatile __u8 *frame;
	unsigned long flags;
	volatile __u16 length;
	volatile __u8 filled;
	volatile __u8 dummy;	/* Alignment to 4*n bytes */
} mux_send_struct;

struct mux_recv_packet_tag {
	__u8 *data;
	__u32 length;
	struct mux_recv_packet_tag *next;
};
typedef struct mux_recv_packet_tag mux_recv_packet;

struct mux_recv_struct_tag {
	__u8 data[TS0710MUX_RECV_BUF_SIZE];
	__u32 length;
	__u32 total;
	mux_recv_packet *mux_packet;
	struct mux_recv_struct_tag *next;
	int no_tty;
	volatile __u8 post_unthrottle;
};
typedef struct mux_recv_struct_tag mux_recv_struct;

static unsigned long mux_recv_flags = 0;

static mux_send_struct *mux_send_info[NR_MUXS];
static volatile __u8 mux_send_info_flags[NR_MUXS];
static volatile __u8 mux_send_info_idx = NR_MUXS;

static mux_recv_struct *mux_recv_info[NR_MUXS];
static volatile __u8 mux_recv_info_flags[NR_MUXS];
static mux_recv_struct *mux_recv_queue = NULL;

static struct tty_driver *mux_driver;

static struct tty_struct *COMM_FOR_MUX_TTY;

/* Line discipline data */
typedef struct {
	struct list_head list;
	int size;
	unsigned char *body; 
} buf_list_t;

struct link_ldisc_data {

  	struct list_head 	in_buf_list;
	spinlock_t		in_buf_lock;
	int in_buf_size;
}; 
struct link_ldisc_data *ldisc_data_ptr;

/* TODO check all these required since duplicating functionality of tty_io */
static struct work_struct send_tqueue;
static struct work_struct receive_tqueue;
static struct work_struct post_recv_tqueue;

static struct tty_struct *mux_table[NR_MUXS];

#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,19))
static struct termios *mux_termios[NR_MUXS];
static struct termios *mux_termios_locked[NR_MUXS];
#else
static struct ktermios *mux_termios[NR_MUXS];
static struct ktermios *mux_termios_locked[NR_MUXS];
#endif

static volatile short int mux_tty[NR_MUXS];

static __u8 crctable[256];

static ts0710_con ts0710_connection;

/* forward decls */
static int get_from_inbuf_list(unsigned char *buf, int dst_count);
static int mux_ldisc_write(const unsigned char *buf, int count);
static int mux_ldisc_chars_in_buffer(struct tty_struct *tty);
static int send_ua(ts0710_con * ts0710, __u8 dlci);
static int send_dm(ts0710_con * ts0710, __u8 dlci);
static int send_sabm(ts0710_con * ts0710, __u8 dlci);
static int send_disc(ts0710_con * ts0710, __u8 dlci);
static void queue_uih(mux_send_struct * send_info, __u16 len,
		      ts0710_con * ts0710, __u8 dlci);
static int send_pn_msg(ts0710_con * ts0710, __u8 prior, __u32 frame_size,
		       __u8 credit_flow, __u8 credits, __u8 dlci, __u8 cr);
static int send_nsc_msg(ts0710_con * ts0710, mcc_type cmd, __u8 cr);
static void set_uih_hdr(short_frame * uih_pkt, __u8 dlci, __u32 len, __u8 cr);

static __u32 crc_check(__u8 * data, __u32 length, __u8 check_sum);
static __u8 crc_calc(__u8 * data, __u32 length);
static void create_crctable(__u8 table[]);
static void mux_sched_send(void);

static unsigned char g_hexbuf[600];
static void mux_debug_info(__u8 * buf, int len)
{
	int i, pos = 0;
	if (len < 200) {
		for (i = 0; i < len; i++) {
			sprintf(&g_hexbuf[pos], "%02x ", buf[i]);
			pos += 3;
		}
		g_hexbuf[pos] = 0;
		printk(KERN_INFO "data <%s>\n",g_hexbuf);
	} else {
		printk(KERN_INFO "Large data, len %d\n",len);
	}
}


/* debug fns */
#ifdef TS0710DEBUG
#define TS0710_DEBUG(fmt, arg...) \
	printk(KERN_INFO "MUX %s: " fmt " " , __func__, ## arg)

static void TS0710_DEBUGHEX(__u8 * buf, int len) {
	mux_debug_info(buf, len);
}

#else
#define TS0710_DEBUG(fmt...)
#define TS0710_DEBUGHEX(buf, len)
#endif

#ifdef TS0710INFO
#define TS0710_INFO(fmt, arg...) \
	printk(KERN_INFO "MUX INFO %s: " fmt " " , __func__, ## arg)

#else
#define TS0710_INFO(fmt...)
#endif

#ifdef TS0710ERROR
#define TS0710_ERROR(fmt, arg...) \
	printk(KERN_ERR "MUX ERROR %s: " fmt " " , __func__, ## arg)
static void TS0710_ERRORHEX(__u8 * buf, int len) {
	mux_debug_info(buf, len);
}
#else
#define TS0710_ERROR(fmt...)
#define TS0710_ERRORHEX(buf, len)
#endif

#ifdef TS0710LOG

static unsigned char g_tbuf[TS0710MUX_MAX_BUF_SIZE];
#define TS0710_LOG(fmt, arg...) printk(fmt, ## arg)
#define TS0710_PRINTK(fmt, arg...) printk(fmt, ## arg)

static void TS0710_LOGSTR_FRAME(__u8 send, __u8 * data, int len)
{
	short_frame *short_pkt;
	long_frame *long_pkt;
	__u8 *uih_data_start;
	__u32 uih_len;
	__u8 dlci;
	int pos;

	if (len <= 0) {
		return;
	}

	pos = 0;
	if (send) {
		pos += sprintf(&g_tbuf[pos], "<");
		short_pkt = (short_frame *) (data);
	} else {
		pos += sprintf(&g_tbuf[pos], ">%d ", *(data));
		short_pkt = (short_frame *) (data + ADDRESS_FIELD_OFFSET);
	}

	dlci = short_pkt->h.addr.server_chn;
	switch (CLR_PF(short_pkt->h.control)) {
	case SABM:
		pos += sprintf(&g_tbuf[pos], "C SABM %d ::", dlci);
		break;
	case UA:
		pos += sprintf(&g_tbuf[pos], "C UA %d ::", dlci);
		break;
	case DM:
		pos += sprintf(&g_tbuf[pos], "C DM %d ::", dlci);
		break;
	case DISC:
		pos += sprintf(&g_tbuf[pos], "C DISC %d ::", dlci);
		break;

	case UIH:
		if (!dlci) {
			pos += sprintf(&g_tbuf[pos], "C MCC %d ::", dlci);
		} else {

			if ((short_pkt->h.length.ea) == 0) {
				long_pkt = (long_frame *) short_pkt;
				uih_len = GET_LONG_LENGTH(long_pkt->h.length);
				uih_data_start = long_pkt->h.data;
			} else {
				uih_len = short_pkt->h.length.len;
				uih_data_start = short_pkt->data;
			}
			pos += sprintf(&g_tbuf[pos], "I %d D %d ::", dlci,
					uih_len);

		}
		break;
	default:
		pos += sprintf(&g_tbuf[pos], "N!!! %d ::", dlci);
		break;
	}

	if (len > (sizeof(g_tbuf) - pos - 1)) {
		len = (sizeof(g_tbuf) - pos - 1);
	}

	memcpy(&g_tbuf[pos], data, len);
	pos += len;
	g_tbuf[pos] = 0;

	/* 0x00 byte in the string pointed by g_tbuf may truncate the print result*/
	TS0710_LOG("%s\n", g_tbuf);
}
#else
#define TS0710_LOG(fmt...)
#define TS0710_PRINTK(fmt, arg...) printk(fmt, ## arg)
#define TS0710_LOGSTR_FRAME(send, data, len)
#endif

/* core fns */

static int valid_dlci(__u8 dlci)
{
	if ((dlci < TS0710_MAX_CHN) && (dlci > 0))
		return 1;
	else
		return 0;
}

static int basic_write(__u8 * buf, int len)
{
	int res;
    int total_len = len + 2;

	TS0710_DEBUG("total_len %d\n",total_len);

	buf[0] = TS0710_BASIC_FLAG;
	buf[len + 1] = TS0710_BASIC_FLAG;

	/* writing out the following bytes  */
	/* TS0710_DEBUGHEX(buf, total_len); */

	res = mux_ldisc_write(buf, total_len);
	if (res != total_len) {
		TS0710_ERRORHEX(buf, total_len);
		TS0710_ERROR("Write Error (wrote %d out of %d)\n",
			res, total_len);
		return res;
	}

	return total_len;
}

/* Functions for the crc-check and calculation */
#define CRC_VALID 0xcf

static __u32 crc_check(__u8 * data, __u32 length, __u8 check_sum)
{
	__u8 fcs = 0xff;

	while (length--) {
		fcs = crctable[fcs ^ *data++];
	}
	fcs = crctable[fcs ^ check_sum];

	if (fcs == (uint) 0xcf) {
		TS0710_DEBUG("CRC check OK\n");
		return 0;
	} else {
		TS0710_ERROR("CRC check failed\n");
		return 1;
	}
}

/* Calculates the checksum according to the ts0710 specification */
static __u8 crc_calc(__u8 * data, __u32 length)
{
	__u8 fcs = 0xff;

	while (length--) {
		fcs = crctable[fcs ^ *data++];
	}

	return 0xff - fcs;
}

/* Calulates a reversed CRC table for the FCS check */
/* TODO - use the static table from 27.010
 * (copy the one from net/bluetooth/rfcomm/core.c) */
static void create_crctable(__u8 table[])
{
	int i, j;

	__u8 data;
	__u8 code_word = (__u8) 0xe0;
	__u8 sr = (__u8) 0;

	for (j = 0; j < 256; j++) {
		data = (__u8) j;

		for (i = 0; i < 8; i++) {
			if ((data & 0x1) ^ (sr & 0x1)) {
				sr >>= 1;
				sr ^= code_word;
			} else {
				sr >>= 1;
			}

			data >>= 1;
			sr &= 0xff;
		}

		table[j] = sr;
		sr = 0;
	}
}

static void ts0710_reset_dlci(__u8 j)
{
	if (j >= TS0710_MAX_CHN)
		return;

	ts0710_connection.dlci[j].state = DISCONNECTED;
	ts0710_connection.dlci[j].flow_control = 0;
	ts0710_connection.dlci[j].mtu = DEF_TS0710_MTU;
	ts0710_connection.dlci[j].initiated = 0;
	ts0710_connection.dlci[j].initiator = 0;
	init_waitqueue_head(&ts0710_connection.dlci[j].open_wait);
	init_waitqueue_head(&ts0710_connection.dlci[j].close_wait);
}

static void ts0710_reset_con(void)
{
	__u8 j;

	ts0710_connection.initiator = 0;
	ts0710_connection.mtu = DEF_TS0710_MTU + TS0710_MAX_HDR_SIZE;
	ts0710_connection.be_testing = 0;
	ts0710_connection.test_errs = 0;
	init_waitqueue_head(&ts0710_connection.test_wait);

	for (j = 0; j < TS0710_MAX_CHN; j++) {
		ts0710_reset_dlci(j);
	}
}

static void ts0710_init(void)
{
	create_crctable(crctable);

	ts0710_reset_con();
}

static void ts0710_upon_disconnect(void)
{
	ts0710_con *ts0710 = &ts0710_connection;
	__u8 j;

	for (j = 0; j < TS0710_MAX_CHN; j++) {
		ts0710->dlci[j].state = DISCONNECTED;
		wake_up_interruptible(&ts0710->dlci[j].open_wait);
		wake_up_interruptible(&ts0710->dlci[j].close_wait);
	}
	ts0710->be_testing = 0;
	wake_up_interruptible(&ts0710->test_wait);
	ts0710_reset_con();
}

static int send_ua(ts0710_con * ts0710, __u8 dlci)
{
	__u8 buf[sizeof(short_frame) + FCS_SIZE + FLAG_SIZE];
	short_frame *ua;

	TS0710_DEBUG("dlci %d\n", dlci);

	ua = (short_frame *) (buf + 1);
	ua->h.addr.ea = 1;
	ua->h.addr.cr = ((~(ts0710->initiator)) & 0x1);
	ua->h.addr.server_chn = dlci;
	ua->h.control = SET_PF(UA);
	ua->h.length.ea = 1;
	ua->h.length.len = 0;
	ua->data[0] = crc_calc((__u8 *) ua, SHORT_CRC_CHECK);

	return basic_write(buf, sizeof(short_frame) + FCS_SIZE);
}

static int send_dm(ts0710_con * ts0710, __u8 dlci)
{
	__u8 buf[sizeof(short_frame) + FCS_SIZE + FLAG_SIZE];
	short_frame *dm;

	TS0710_DEBUG("dlci %d\n", dlci);

	dm = (short_frame *) (buf + 1);
	dm->h.addr.ea = 1;
	dm->h.addr.cr = ((~(ts0710->initiator)) & 0x1);
	dm->h.addr.server_chn = dlci;
	dm->h.control = SET_PF(DM);
	dm->h.length.ea = 1;
	dm->h.length.len = 0;
	dm->data[0] = crc_calc((__u8 *) dm, SHORT_CRC_CHECK);

	return basic_write(buf, sizeof(short_frame) + FCS_SIZE);
}

static int send_sabm(ts0710_con * ts0710, __u8 dlci)
{
	__u8 buf[sizeof(short_frame) + FCS_SIZE + FLAG_SIZE];
	short_frame *sabm;

	TS0710_DEBUG("dlci %d\n", dlci);

	sabm = (short_frame *) (buf + 1);
	sabm->h.addr.ea = 1;
	sabm->h.addr.cr = ((ts0710->initiator) & 0x1);
	sabm->h.addr.server_chn = dlci;
	sabm->h.control = SET_PF(SABM);
	sabm->h.length.ea = 1;
	sabm->h.length.len = 0;
	sabm->data[0] = crc_calc((__u8 *) sabm, SHORT_CRC_CHECK);

	return basic_write(buf, sizeof(short_frame) + FCS_SIZE);
}

static int send_disc(ts0710_con * ts0710, __u8 dlci)
{
	__u8 buf[sizeof(short_frame) + FCS_SIZE + FLAG_SIZE];
	short_frame *disc;

	TS0710_DEBUG("dlci %d\n", dlci);

	disc = (short_frame *) (buf + 1);
	disc->h.addr.ea = 1;
	disc->h.addr.cr = ((ts0710->initiator) & 0x1);
	disc->h.addr.server_chn = dlci;
	disc->h.control = SET_PF(DISC);
	disc->h.length.ea = 1;
	disc->h.length.len = 0;
	disc->data[0] = crc_calc((__u8 *) disc, SHORT_CRC_CHECK);

	return basic_write(buf, sizeof(short_frame) + FCS_SIZE);
}

static void queue_uih(mux_send_struct * send_info, __u16 len,
		      ts0710_con * ts0710, __u8 dlci)
{
	__u32 size;
	TS0710_INFO("Create for send UIH (%d bytes, dlc %d)\n",len, dlci);

	if (len > SHORT_PAYLOAD_SIZE) {
		long_frame *l_pkt;

		size = sizeof(long_frame) + len + FCS_SIZE;
		l_pkt = (long_frame *) (send_info->frame - sizeof(long_frame));
		set_uih_hdr((void *)l_pkt, dlci, len, ts0710->initiator);
		l_pkt->data[len] = crc_calc((__u8 *) l_pkt, LONG_CRC_CHECK);
		send_info->frame = ((__u8 *) l_pkt) - 1;
	} else {
		short_frame *s_pkt;

		size = sizeof(short_frame) + len + FCS_SIZE;
		s_pkt =
		    (short_frame *) (send_info->frame - sizeof(short_frame));
		set_uih_hdr((void *)s_pkt, dlci, len, ts0710->initiator);
		s_pkt->data[len] = crc_calc((__u8 *) s_pkt, SHORT_CRC_CHECK);
		send_info->frame = ((__u8 *) s_pkt) - 1;
	}
	send_info->length = size;
}

static int send_closedown_msg(void)
{
	__u8 buf[10];

	cld_msg *uih_pkt = (cld_msg *) (buf + 1);

	uih_pkt->s_head.addr.ea = 1;
	uih_pkt->s_head.addr.cr = 0x1;
	uih_pkt->s_head.addr.server_chn = 0;

	uih_pkt->s_head.control = CLR_PF(UIH);

	uih_pkt->s_head.length.ea = 1;
	uih_pkt->s_head.length.len = 2;

	/* multiplexer control channel command */
	uih_pkt->mcc_s_head.type.ea = 1;
	uih_pkt->mcc_s_head.type.cr = 1;
	uih_pkt->mcc_s_head.type.type = CLD;

	uih_pkt->mcc_s_head.length.ea = 1;
	uih_pkt->mcc_s_head.length.len = 1;

	uih_pkt->fcs = crc_calc((__u8 *) uih_pkt, SHORT_CRC_CHECK);

	return basic_write(buf, 6);
}

static int ts0710_fcon_msg(ts0710_con * ts0710, __u8 cr)
{
	__u8 buf[30];
	mcc_short_frame *mcc_pkt;
	short_frame *uih_pkt;
	__u32 size;

	size = sizeof(short_frame) + sizeof(mcc_short_frame) + FCS_SIZE;
	uih_pkt = (short_frame *) (buf + 1);
	set_uih_hdr(uih_pkt, CTRL_CHAN, sizeof(mcc_short_frame),
		    ts0710->initiator);
	uih_pkt->data[sizeof(mcc_short_frame)] =
	    crc_calc((__u8 *) uih_pkt, SHORT_CRC_CHECK);
	mcc_pkt = (mcc_short_frame *) (uih_pkt->data);

	mcc_pkt->h.type.ea = EA;
	mcc_pkt->h.type.cr = cr;
	mcc_pkt->h.type.type = FCON;
	mcc_pkt->h.length.ea = EA;
	mcc_pkt->h.length.len = 0;

	return basic_write(buf, size);
}

static int ts0710_fcoff_msg(ts0710_con * ts0710, __u8 cr)
{
	__u8 buf[30];
	mcc_short_frame *mcc_pkt;
	short_frame *uih_pkt;
	__u32 size;

	size = (sizeof(short_frame) + sizeof(mcc_short_frame) + FCS_SIZE);
	uih_pkt = (short_frame *) (buf + 1);
	set_uih_hdr(uih_pkt, CTRL_CHAN, sizeof(mcc_short_frame),
		    ts0710->initiator);
	uih_pkt->data[sizeof(mcc_short_frame)] =
	    crc_calc((__u8 *) uih_pkt, SHORT_CRC_CHECK);
	mcc_pkt = (mcc_short_frame *) (uih_pkt->data);

	mcc_pkt->h.type.ea = 1;
	mcc_pkt->h.type.cr = cr;
	mcc_pkt->h.type.type = FCOFF;
	mcc_pkt->h.length.ea = 1;
	mcc_pkt->h.length.len = 0;

	return basic_write(buf, size);
}

/* Sends an PN-messages and sets the not negotiable parameters to their
   default values */
static int send_pn_msg(ts0710_con * ts0710, __u8 prior, __u32 frame_size,
		       __u8 credit_flow, __u8 credits, __u8 dlci, __u8 cr)
{
	__u8 buf[30];
	pn_msg *pn_pkt;
	__u32 size;
	TS0710_DEBUG
	    ("dlc %d, prior:0x%02x frame_size:%d credit_flow:%x credits:%d cr:%x\n",
	     dlci, prior, frame_size, credit_flow, credits, cr);

	size = sizeof(pn_msg);
	pn_pkt = (pn_msg *) (buf + 1);

	set_uih_hdr((void *)pn_pkt, CTRL_CHAN,
		    size - (sizeof(short_frame) + FCS_SIZE), ts0710->initiator);
	pn_pkt->fcs = crc_calc((__u8 *) pn_pkt, SHORT_CRC_CHECK);

	pn_pkt->mcc_s_head.type.ea = 1;
	pn_pkt->mcc_s_head.type.cr = cr;
	pn_pkt->mcc_s_head.type.type = PN;
	pn_pkt->mcc_s_head.length.ea = 1;
	pn_pkt->mcc_s_head.length.len = 8;

	pn_pkt->res1 = 0;
	pn_pkt->res2 = 0;
	pn_pkt->dlci = dlci;
	pn_pkt->frame_type = 0;
	pn_pkt->credit_flow = credit_flow;
	pn_pkt->prior = prior;
	pn_pkt->ack_timer = 0;
	SET_PN_MSG_FRAME_SIZE(pn_pkt, frame_size);
	pn_pkt->credits = credits;
	pn_pkt->max_nbrof_retrans = 0;

	return basic_write(buf, size);
}

static int send_nsc_msg(ts0710_con * ts0710, mcc_type cmd, __u8 cr)
{
	__u8 buf[30];
	nsc_msg *nsc_pkt;
	__u32 size;

	size = sizeof(nsc_msg);
	nsc_pkt = (nsc_msg *) (buf + 1);

	set_uih_hdr((void *)nsc_pkt, CTRL_CHAN,
		    sizeof(nsc_msg) - sizeof(short_frame) - FCS_SIZE,
		    ts0710->initiator);

	nsc_pkt->fcs = crc_calc((__u8 *) nsc_pkt, SHORT_CRC_CHECK);

	nsc_pkt->mcc_s_head.type.ea = 1;
	nsc_pkt->mcc_s_head.type.cr = cr;
	nsc_pkt->mcc_s_head.type.type = NSC;
	nsc_pkt->mcc_s_head.length.ea = 1;
	nsc_pkt->mcc_s_head.length.len = 1;

	nsc_pkt->command_type.ea = 1;
	nsc_pkt->command_type.cr = cmd.cr;
	nsc_pkt->command_type.type = cmd.type;

	return basic_write(buf, size);
}

static int ts0710_msc_msg(ts0710_con * ts0710, __u8 value, __u8 cr, __u8 dlci)
{
	__u8 buf[30];
	msc_msg *msc_pkt;
	__u32 size;

	size = sizeof(msc_msg);
	msc_pkt = (msc_msg *) (buf + 1);

	set_uih_hdr((void *)msc_pkt, CTRL_CHAN,
		    sizeof(msc_msg) - sizeof(short_frame) - FCS_SIZE,
		    ts0710->initiator);

	msc_pkt->fcs = crc_calc((__u8 *) msc_pkt, SHORT_CRC_CHECK);

	msc_pkt->mcc_s_head.type.ea = 1;
	msc_pkt->mcc_s_head.type.cr = cr;
	msc_pkt->mcc_s_head.type.type = MSC;
	msc_pkt->mcc_s_head.length.ea = 1;
	msc_pkt->mcc_s_head.length.len = 2;

	msc_pkt->dlci.ea = 1;
	msc_pkt->dlci.cr = 1;
	msc_pkt->dlci.server_chn = dlci;

	msc_pkt->v24_sigs = value;

	TS0710_DEBUGHEX(buf, size);

	return basic_write(buf, size);
}

static int ts0710_test_msg(ts0710_con * ts0710, __u8 * test_pattern, __u32 len,
			   __u8 cr, __u8 * f_buf /*Frame buf */ )
{
	__u32 size;

	if (len > SHORT_PAYLOAD_SIZE) {
		long_frame *uih_pkt;
		mcc_long_frame *mcc_pkt;

		size =
		    (sizeof(long_frame) + sizeof(mcc_long_frame) + len +
		     FCS_SIZE);
		uih_pkt = (long_frame *) (f_buf + 1);

		set_uih_hdr((short_frame *) uih_pkt, CTRL_CHAN, len +
			    sizeof(mcc_long_frame), ts0710->initiator);
		uih_pkt->data[GET_LONG_LENGTH(uih_pkt->h.length)] =
		    crc_calc((__u8 *) uih_pkt, LONG_CRC_CHECK);
		mcc_pkt = (mcc_long_frame *) uih_pkt->data;

		mcc_pkt->h.type.ea = EA;
		/* cr tells whether it is a commmand (1) or a response (0) */
		mcc_pkt->h.type.cr = cr;
		mcc_pkt->h.type.type = TEST;
		SET_LONG_LENGTH(mcc_pkt->h.length, len);
		memcpy(mcc_pkt->value, test_pattern, len);
	} else if (len > (SHORT_PAYLOAD_SIZE - sizeof(mcc_short_frame))) {
		long_frame *uih_pkt;
		mcc_short_frame *mcc_pkt;

		/* Create long uih packet and short mcc packet */
		size =
		    (sizeof(long_frame) + sizeof(mcc_short_frame) + len +
		     FCS_SIZE);
		uih_pkt = (long_frame *) (f_buf + 1);

		set_uih_hdr((short_frame *) uih_pkt, CTRL_CHAN,
			    len + sizeof(mcc_short_frame), ts0710->initiator);
		uih_pkt->data[GET_LONG_LENGTH(uih_pkt->h.length)] =
		    crc_calc((__u8 *) uih_pkt, LONG_CRC_CHECK);
		mcc_pkt = (mcc_short_frame *) uih_pkt->data;

		mcc_pkt->h.type.ea = EA;
		mcc_pkt->h.type.cr = cr;
		mcc_pkt->h.type.type = TEST;
		mcc_pkt->h.length.ea = EA;
		mcc_pkt->h.length.len = len;
		memcpy(mcc_pkt->value, test_pattern, len);
	} else {
		short_frame *uih_pkt;
		mcc_short_frame *mcc_pkt;

		size =
		    (sizeof(short_frame) + sizeof(mcc_short_frame) + len +
		     FCS_SIZE);
		uih_pkt = (short_frame *) (f_buf + 1);

		set_uih_hdr((void *)uih_pkt, CTRL_CHAN, len
			    + sizeof(mcc_short_frame), ts0710->initiator);
		uih_pkt->data[uih_pkt->h.length.len] =
		    crc_calc((__u8 *) uih_pkt, SHORT_CRC_CHECK);
		mcc_pkt = (mcc_short_frame *) uih_pkt->data;

		mcc_pkt->h.type.ea = EA;
		mcc_pkt->h.type.cr = cr;
		mcc_pkt->h.type.type = TEST;
		mcc_pkt->h.length.ea = EA;
		mcc_pkt->h.length.len = len;
		memcpy(mcc_pkt->value, test_pattern, len);

	}
	return basic_write(f_buf, size);
}

static void set_uih_hdr(short_frame * uih_pkt, __u8 dlci, __u32 len, __u8 cr)
{
	uih_pkt->h.addr.ea = 1;
	uih_pkt->h.addr.cr = cr;
	uih_pkt->h.addr.server_chn = dlci;
	uih_pkt->h.control = CLR_PF(UIH);

	if (len > SHORT_PAYLOAD_SIZE) {
		TS0710_DEBUG("Large payload\n");
		SET_LONG_LENGTH(((long_frame *) uih_pkt)->h.length, len);
	} else {
		uih_pkt->h.length.ea = 1;
		uih_pkt->h.length.len = len;
	}
}

/* Parses a control channel packet */
void process_mcc(__u8 * data, __u32 len, ts0710_con * ts0710, int longpkt)
{
	mcc_short_frame *mcc_short_pkt;
	int j;

	if (longpkt) {
		mcc_short_pkt =
		    (mcc_short_frame *) (((long_frame *) data)->data);
	} else {
		mcc_short_pkt =
		    (mcc_short_frame *) (((short_frame *) data)->data);
	}

	switch (mcc_short_pkt->h.type.type) {
	case TEST:
		if (mcc_short_pkt->h.type.cr == MCC_RSP) {
			TS0710_INFO("Received test command response\n");

			if (ts0710->be_testing) {
				if ((mcc_short_pkt->h.length.ea) == 0) {
					mcc_long_frame *mcc_long_pkt;
					mcc_long_pkt =
					    (mcc_long_frame *) mcc_short_pkt;
					if (GET_LONG_LENGTH
					    (mcc_long_pkt->h.length) !=
					    TEST_PATTERN_SIZE) {
						ts0710->test_errs =
						    TEST_PATTERN_SIZE;
						TS0710_ERROR
						    ("Err: recd test patt is %d bytes, not expected %d\n",
						     GET_LONG_LENGTH
						     (mcc_long_pkt->h.length),
						     TEST_PATTERN_SIZE);
					} else {
						ts0710->test_errs = 0;
						for (j = 0;
						     j < TEST_PATTERN_SIZE;
						     j++) {
							if (mcc_long_pkt->
							    value[j] !=
							    (j & 0xFF)) {
								(ts0710->
								 test_errs)++;
							}
						}
					}

				} else {

#if TEST_PATTERN_SIZE < 128
					if (mcc_short_pkt->h.length.len !=
					    TEST_PATTERN_SIZE) {
#endif

						ts0710->test_errs =
						    TEST_PATTERN_SIZE;
						TS0710_ERROR
						    ("Err: recd test patt is %d bytes, not expected %d\n",
						     mcc_short_pkt->h.length.
						     len, TEST_PATTERN_SIZE);

#if TEST_PATTERN_SIZE < 128
					} else {
						ts0710->test_errs = 0;
						for (j = 0;
						     j < TEST_PATTERN_SIZE;
						     j++) {
							if (mcc_short_pkt->
							    value[j] !=
							    (j & 0xFF)) {
								(ts0710->
								 test_errs)++;
							}
						}
					}
#endif

				}

				ts0710->be_testing = 0;	/* Clear the flag */
				wake_up_interruptible(&ts0710->test_wait);
			} else {
				TS0710_ERROR
				    ("Err: shouldn't or late to get test cmd response\n");
			}
		} else {
			__u8 *test_buf = (__u8 *) kmalloc(len + 32, GFP_ATOMIC);
			if (!test_buf) {
				break;
			}

			if ((mcc_short_pkt->h.length.ea) == 0) {
				mcc_long_frame *mcc_long_pkt;
				mcc_long_pkt = (mcc_long_frame *) mcc_short_pkt;
				ts0710_test_msg(ts0710, mcc_long_pkt->value,
						GET_LONG_LENGTH(mcc_long_pkt->h.
								length),
						MCC_RSP, test_buf);
			} else {
				ts0710_test_msg(ts0710, mcc_short_pkt->value,
						mcc_short_pkt->h.length.len,
						MCC_RSP, test_buf);
			}

			kfree(test_buf);
		}
		break;

	case FCON:
		TS0710_INFO("Received Flow control on (all channels)\n");
		if (mcc_short_pkt->h.type.cr == MCC_CMD) {
			ts0710->dlci[0].state = CONNECTED;
			ts0710_fcon_msg(ts0710, MCC_RSP);
			mux_sched_send();
		}
		break;

	case FCOFF:
		TS0710_INFO("Received Flow control off (all channels)\n");
		if (mcc_short_pkt->h.type.cr == MCC_CMD) {
			for (j = 0; j < TS0710_MAX_CHN; j++) {
				ts0710->dlci[j].state = FLOW_STOPPED;
			}
			ts0710_fcoff_msg(ts0710, MCC_RSP);
		}
		break;

	case MSC:
		{
			__u8 dlci;
			__u8 v24_sigs;

			dlci = (mcc_short_pkt->value[0]) >> 2;
			v24_sigs = mcc_short_pkt->value[1];

			if ((ts0710->dlci[dlci].state != CONNECTED)
			    && (ts0710->dlci[dlci].state != FLOW_STOPPED)) {
				TS0710_ERROR("Error MSC dlc %d not connected, sending DM\n",
					dlci);
				send_dm(ts0710, dlci);
				break;
			}
			if (mcc_short_pkt->h.type.cr == MCC_CMD) {
				TS0710_DEBUG("Recd Modem status cmd dlc 0x%02x v24 0x%02x\n",
					dlci,v24_sigs);
				if (v24_sigs & 2) {
					if (ts0710->dlci[dlci].state ==
					    CONNECTED) {
						TS0710_INFO("Recd Flow off on dlc %d\n", dlci);
						ts0710->dlci[dlci].state =
						    FLOW_STOPPED;
					}
				} else {
					if (ts0710->dlci[dlci].state ==
					    FLOW_STOPPED) {
						ts0710->dlci[dlci].state =
						    CONNECTED;
						TS0710_INFO("Recd Flow on on dlc %d\n", dlci);
						mux_sched_send();
					}
				}

				ts0710_msc_msg(ts0710, v24_sigs, MCC_RSP, dlci);
			} else {
				TS0710_INFO("Recd Modem status response\n");

				if (v24_sigs & 2) {
					TS0710_DEBUG("Flow stop accepted\n");
				}
			}
			break;
		}

	case PN:
		{
			__u8 dlci;
			__u16 frame_size;
			pn_msg *pn_pkt;

			pn_pkt = (pn_msg *) data;
			dlci = pn_pkt->dlci;
			frame_size = GET_PN_MSG_FRAME_SIZE(pn_pkt);
			TS0710_INFO("Recd DLC parameter negotiation, PN\n");
			if (pn_pkt->mcc_s_head.type.cr == MCC_CMD) {
				TS0710_DEBUG("recd PN command with:\n");
				TS0710_DEBUG("frame size:%d\n", frame_size);

				frame_size =
				    min(frame_size, ts0710->dlci[dlci].mtu);
				send_pn_msg(ts0710, pn_pkt->prior, frame_size,
					    0, 0, dlci, MCC_RSP);
				ts0710->dlci[dlci].mtu = frame_size;
				TS0710_INFO("mtu dlc 0 set to %d\n",
					     ts0710->dlci[dlci].mtu);
			} else {
				TS0710_DEBUG("frame size:%d\n", frame_size);

				frame_size =
				    min(frame_size, ts0710->dlci[dlci].mtu);
				ts0710->dlci[dlci].mtu = frame_size;

				TS0710_INFO("mtu set on dlc %d to %d\n",
				     dlci, ts0710->dlci[dlci].mtu);

				if (ts0710->dlci[dlci].state == NEGOTIATING) {
					ts0710->dlci[dlci].state = CONNECTING;
					wake_up_interruptible(&ts0710->
							      dlci[dlci].
							      open_wait);
				}
			}
			break;
		}

	case NSC:
		TS0710_ERROR("recd non supported cmd response\n");
		break;

	default:
		TS0710_ERROR("recd a non supported cmd\n");
		send_nsc_msg(ts0710, mcc_short_pkt->h.type, MCC_RSP);
		break;
	}
}

static mux_recv_packet *get_mux_recv_packet(__u32 size)
{
	mux_recv_packet *recv_packet;

	recv_packet =
	    (mux_recv_packet *) kmalloc(sizeof(mux_recv_packet), GFP_ATOMIC);
	TS0710_DEBUG("recv_packet %p\n",recv_packet);
	if (!recv_packet) {
		return 0;
	}

	recv_packet->data = (__u8 *) kmalloc(size, GFP_ATOMIC);
	if (!(recv_packet->data)) {
		kfree(recv_packet);
		return 0;
	}
	recv_packet->length = 0;
	recv_packet->next = 0;
	return recv_packet;
}

static void free_mux_recv_packet(mux_recv_packet * recv_packet)
{
	TS0710_DEBUG("recv_packet %p\n",recv_packet);
	if (!recv_packet) {
		return;
	}

	if (recv_packet->data) {
		kfree(recv_packet->data);
	}
	kfree(recv_packet);
}

static void free_mux_recv_struct(mux_recv_struct * recv_info)
{
	mux_recv_packet *recv_packet1, *recv_packet2;

	if (!recv_info) {
		return;
	}

	recv_packet1 = recv_info->mux_packet;
	while (recv_packet1) {
		recv_packet2 = recv_packet1->next;
		free_mux_recv_packet(recv_packet1);
		recv_packet1 = recv_packet2;
	}

	kfree(recv_info);
}

static inline void add_post_recv_queue(mux_recv_struct ** head,
				       mux_recv_struct * new_item)
{
	new_item->next = *head;
	*head = new_item;
}

static void ts0710_flow_on(__u8 dlci, ts0710_con * ts0710)
{
	int i;

	if ((ts0710->dlci[0].state != CONNECTED)
	    && (ts0710->dlci[0].state != FLOW_STOPPED)) {
		return;
	} else if ((ts0710->dlci[dlci].state != CONNECTED)
		   && (ts0710->dlci[dlci].state != FLOW_STOPPED)) {
		return;
	}

	if (!(ts0710->dlci[dlci].flow_control)) {
		return;
	}

	for (i = 0; i < 3; i++) {
		if (ts0710_msc_msg(ts0710, EA | RTC | RTR | DV, MCC_CMD, dlci) <
		    0) {
			continue;
		} else {
			TS0710_LOG("send Flow on on dlc %d\n", dlci);
			ts0710->dlci[dlci].flow_control = 0;
			break;
		}
	}
}

static void ts0710_flow_off(struct tty_struct *tty, __u8 dlci,
			    ts0710_con * ts0710)
{
	int i;

	if (test_and_set_bit(TTY_THROTTLED, &tty->flags)) {
		return;
	}

	if ((ts0710->dlci[0].state != CONNECTED)
	    && (ts0710->dlci[0].state != FLOW_STOPPED)) {
		return;
	} else if ((ts0710->dlci[dlci].state != CONNECTED)
		   && (ts0710->dlci[dlci].state != FLOW_STOPPED)) {
		return;
	}

	if (ts0710->dlci[dlci].flow_control) {
		return;
	}

	for (i = 0; i < 3; i++) {
		if (ts0710_msc_msg
		    (ts0710, EA | FC | RTC | RTR | DV, MCC_CMD, dlci) < 0) {
			continue;
		} else {
			TS0710_LOG("send Flow off on dlc %d\n", dlci);
			ts0710->dlci[dlci].flow_control = 1;
			break;
		}
	}
}

static int ts0710_recv_data(ts0710_con * ts0710, char *data, int len)
{
	long_frame *long_pkt;
	__u8 *uih_data_start;
	__u32 uih_len;
	__u8 be_connecting;
#ifdef TS0710DEBUG
	unsigned long t;
#endif

	short_frame *short_pkt = (short_frame *) data;
	__u8 dlci = short_pkt->h.addr.server_chn;

	switch (CLR_PF(short_pkt->h.control)) {
	case SABM:
		TS0710_INFO("SABM dlc %d\n",dlci);
		if (!dlci) {
			ts0710->dlci[0].state = CONNECTED;

			/* sending UA back */
			send_ua(ts0710, dlci);
			wake_up_interruptible(&ts0710->dlci[0].open_wait);

		} else if (valid_dlci(dlci)) {

			TS0710_DEBUG("Incoming connect on channel %d\n", dlci);
			/* sending UA back */
			send_ua(ts0710, dlci);

			ts0710->dlci[dlci].state = CONNECTED;
			wake_up_interruptible(&ts0710->dlci[dlci].open_wait);

		} else {
			TS0710_ERROR("invalid dlc %d, sending DM\n", dlci);
			send_dm(ts0710, dlci);
		}
		break;

	case UA:
		TS0710_INFO("UA dlc %d\n",dlci);
		if (!dlci) {
			if (ts0710->dlci[0].state == CONNECTING) {
				TS0710_DEBUG("dlc 0 is Connected\n");
				ts0710->dlci[0].state = CONNECTED;
				wake_up_interruptible(&ts0710->dlci[0].
						      open_wait);
			} else if (ts0710->dlci[0].state == DISCONNECTING) {
				ts0710_upon_disconnect();
			} else {
				TS0710_INFO("dlc 0 state %d - unexpected UA\n",
					ts0710->dlci[dlci].state);
			}
		} else if (valid_dlci(dlci)) {
			TS0710_DEBUG("Incoming UA on channel %d\n", dlci);

			if (ts0710->dlci[dlci].state == CONNECTING) {
				TS0710_DEBUG("dlc %d is Connected\n",dlci);
				ts0710->dlci[dlci].state = CONNECTED;
				wake_up_interruptible(&ts0710->dlci[dlci].
						      open_wait);
			} else if (ts0710->dlci[dlci].state == DISCONNECTING) {
				ts0710->dlci[dlci].state = DISCONNECTED;
				wake_up_interruptible(&ts0710->dlci[dlci].
						      open_wait);
				wake_up_interruptible(&ts0710->dlci[dlci].
						      close_wait);
				ts0710_reset_dlci(dlci);
			} else {
				TS0710_INFO("dlc %d state %d - unexpected UA\n",
					dlci, ts0710->dlci[dlci].state);
			}
		} else {
			TS0710_ERROR("invalid dlc %d\n", dlci);
		}
		break;

	case DM:
		TS0710_INFO("DM dlc %d\n",dlci);
		if (!dlci) {
			if (ts0710->dlci[0].state == CONNECTING) {
				be_connecting = 1;
			} else {
				be_connecting = 0;
			}
			ts0710_upon_disconnect();
			if (be_connecting) {
				ts0710->dlci[0].state = REJECTED;
			}
		} else if (valid_dlci(dlci)) {
			TS0710_INFO("Incoming DM on channel %d\n", dlci);

			if (ts0710->dlci[dlci].state == CONNECTING) {
				ts0710->dlci[dlci].state = REJECTED;
			} else {
				ts0710->dlci[dlci].state = DISCONNECTED;
			}
			wake_up_interruptible(&ts0710->dlci[dlci].open_wait);
			wake_up_interruptible(&ts0710->dlci[dlci].close_wait);
			ts0710_reset_dlci(dlci);
		} else {
			TS0710_ERROR("invalid dlc %d\n", dlci);
		}
		break;

	case DISC:
		TS0710_INFO("DISC dlc %d\n",dlci);
		if (!dlci) {
			/* sending UA back */
			send_ua(ts0710, dlci);
			ts0710_upon_disconnect();
		} else if (valid_dlci(dlci)) {
			TS0710_INFO("DISC, sending back UA\n");

			send_ua(ts0710, dlci);

			ts0710->dlci[dlci].state = DISCONNECTED;
			wake_up_interruptible(&ts0710->dlci[dlci].open_wait);
			wake_up_interruptible(&ts0710->dlci[dlci].close_wait);
			ts0710_reset_dlci(dlci);
		} else {
			TS0710_ERROR("invalid dlci %d\n", dlci);
		}
		break;

	case UIH:
		if ((dlci >= TS0710_MAX_CHN)) {
			TS0710_ERROR("UIH invalid dlc %d\n", dlci);
			send_dm(ts0710, dlci);
			break;
		}

		if (GET_PF(short_pkt->h.control)) {
			TS0710_ERROR("Error UIH dlc %d packet with P/F set, discard it!\n",
				dlci);
			break;
		}

		if ((ts0710->dlci[dlci].state != CONNECTED)
		    && (ts0710->dlci[dlci].state != FLOW_STOPPED)) {
			TS0710_ERROR("Error UIH dlc %d not connected, sending DM\n", dlci);
			send_dm(ts0710, dlci);
			break;
		}

		if ((short_pkt->h.length.ea) == 0) {
			long_pkt = (long_frame *) data;
			uih_len = GET_LONG_LENGTH(long_pkt->h.length);
			uih_data_start = long_pkt->h.data;
			TS0710_INFO("Long UIH dlc %d, uih_len %d\n", dlci, uih_len);
		} else {
			uih_len = short_pkt->h.length.len;
			uih_data_start = short_pkt->data;
			TS0710_INFO("Short UIH dlc %d, uih_len %d\n", dlci, uih_len);
		}

		if (dlci == 0) {
			TS0710_DEBUG("UIH on serv_channel 0\n");
			process_mcc(data, len, ts0710,
				    !(short_pkt->h.length.ea));
		} else if (valid_dlci(dlci)) {
			/* do tty dispatch */
			__u8 tty_idx;
			struct tty_struct *tty;
			__u8 queue_data;
			__u8 post_recv;
			__u8 flow_control;
			mux_recv_struct *recv_info;
			int recv_room;
			mux_recv_packet *recv_packet, *recv_packet2;

			TS0710_DEBUG("UIH on channel %d\n", dlci);

			if (uih_len > ts0710->dlci[dlci].mtu) {
				TS0710_ERROR
				    ("Error: dlc %d, uih_len:%d > mtu:%d, discarded\n",
				     dlci, uih_len, ts0710->dlci[dlci].mtu);
				break;
			}
			if (!uih_len) {
				break;
			}

			tty_idx = dlci;

			tty = mux_table[tty_idx];
			if ((!mux_tty[tty_idx]) || (!tty)) {
				TS0710_ERROR("Port /dev/mux%d not open, discard it! \n",tty_idx);
				TS0710_ERROR("mux_tty[%d] = %d, tty is at %p\n",
					tty_idx, mux_tty[tty_idx], tty);
			} else {	/* Begin processing received data */
				if ((!mux_recv_info_flags[tty_idx])
				    || (!mux_recv_info[tty_idx])) {
					TS0710_ERROR("No mux_recv_info, discarded! /dev/mux%d\n",
						tty_idx);
					break;
				}

				recv_info = mux_recv_info[tty_idx];
				if (recv_info->total > 8192) {
					TS0710_ERROR
					    ("Discard data for tty_idx:%d, recv_info->total > 8192\n",
					    	tty_idx);
					break;
				}

				queue_data = 0;
				post_recv = 0;
				flow_control = 0;
				recv_room = 65535;
				if (tty->receive_room)
					recv_room = tty->receive_room;

				if (test_bit(TTY_THROTTLED, &tty->flags)) {
					queue_data = 1;
				} else {
					if ((recv_room - (uih_len + recv_info->total)) <
					    ts0710->dlci[dlci].mtu) {
						flow_control = 1;
					}
				}

				if (!queue_data) {
					TS0710_DEBUG("Put recd data into read buffer of /dev/mux%d\n",
						tty_idx);

#ifdef TS0710DEBUG
					t = jiffies;
#endif
#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,27))
					(tty->ldisc.receive_buf) (tty, uih_data_start, NULL, uih_len);
#elsif (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,31))
					(tty->ldisc.ops->receive_buf) (tty, uih_data_start, NULL, uih_len);
#else
					(tty->ldisc->ops->receive_buf) (tty, uih_data_start, NULL, uih_len);
#endif

#ifdef TS0710DEBUG
					TS0710_DEBUG
					    ("tty->ldisc.receive_buf (took ticks: %lu)\n",
					     (jiffies - t));
#endif

				} else {
					/* Queue data */
					TS0710_DEBUG("Put recd data into recv queue of /dev/mux%d\n",
					     tty_idx);
					if (recv_info->total) {
						/* recv_info is already linked into mux_recv_queue */

						recv_packet = get_mux_recv_packet(uih_len);
						if (!recv_packet) {
							TS0710_ERROR("no memory\n");
							break;
						}

						memcpy(recv_packet->data, uih_data_start, uih_len);
						
						recv_packet->length = uih_len;
						recv_info->total += uih_len;
						recv_packet->next = NULL;

						if (!(recv_info->mux_packet)) {
							recv_info->mux_packet = recv_packet;
						} else {
							recv_packet2 = recv_info->mux_packet;
							/* TODO check this code */
							while (recv_packet2->next) {
								recv_packet2 = recv_packet2->next;
							}
							recv_packet2->next =recv_packet;
						}
					} else {
						if (uih_len > TS0710MUX_RECV_BUF_SIZE) {
							TS0710_ERROR
							    ("Error: tty_idx:%d, uih_len %d too big\n",
							     tty_idx, uih_len);
							uih_len = TS0710MUX_RECV_BUF_SIZE;
						}
						memcpy(recv_info->data, uih_data_start, uih_len);
						
						recv_info->length = uih_len;
						recv_info->total = uih_len;

						add_post_recv_queue(&mux_recv_queue, recv_info);
					}
				} /* End Queue data */

				if (flow_control) {
					/* Do something for flow control */
					ts0710_flow_off(tty, dlci, ts0710);
				}

				if (post_recv) 
					schedule_work(&post_recv_tqueue);
			} /* End processing received data */
		} else {
			TS0710_ERROR("invalid dlc %d\n", dlci);
		}

		break;

	default:
		TS0710_ERROR("illegal packet\n");
		break;
	}
	return 0;
}

static void ts0710_close_channel(__u8 dlci)
{
	ts0710_con *ts0710 = &ts0710_connection;
	int try;
	unsigned long t;

	TS0710_INFO("ts0710_disc_command on channel %d\n", dlci);

	if ((ts0710->dlci[dlci].state == DISCONNECTED)
	    || (ts0710->dlci[dlci].state == REJECTED)) {
		return;
	} else if (ts0710->dlci[dlci].state == DISCONNECTING) {
		/* Reentry */
		return;
	} else {
		ts0710->dlci[dlci].state = DISCONNECTING;
		try = 3;
		while (try--) {
			t = jiffies;
			send_disc(ts0710, dlci);
			interruptible_sleep_on_timeout(&ts0710->dlci[dlci].close_wait,
						       TS0710MUX_TIME_OUT);
			if (ts0710->dlci[dlci].state == DISCONNECTED) {
				break;
			} else if (signal_pending(current)) {
				TS0710_ERROR("dlc %d Send DISC got signal!\n", dlci);
				break;
			} else if ((jiffies - t) >= TS0710MUX_TIME_OUT) {
				TS0710_ERROR("dlc %d Send DISC timeout!\n", dlci);
				continue;
			}
		}

		if (ts0710->dlci[dlci].state != DISCONNECTED) {
			if (dlci == 0) {	/* Control Channel */
				ts0710_upon_disconnect();
			} else {	/* Other Channel */
				ts0710->dlci[dlci].state = DISCONNECTED;
				wake_up_interruptible(&ts0710->dlci[dlci].close_wait);
				ts0710_reset_dlci(dlci);
			}
		}
	}
}

static int ts0710_open_channel(__u8 dlci)
{
	ts0710_con *ts0710 = &ts0710_connection;
	int try;
	int retval;
	unsigned long t;

	retval = -ENODEV;
	if (dlci == 0) {
		/* control channel */
		if ((ts0710->dlci[0].state == CONNECTED)
		    || (ts0710->dlci[0].state == FLOW_STOPPED)) {
			return 0;
		} else if (ts0710->dlci[0].state == CONNECTING) {
			/* Reentry */
			TS0710_ERROR("dlc 0, re-entry to open DLCI 0, pid: %d, %s\n",
			     current->pid, current->comm);
			try = 11;
			while (try--) {
				t = jiffies;
				interruptible_sleep_on_timeout(&ts0710->dlci[0].open_wait,
							       TS0710MUX_TIME_OUT);
				if ((ts0710->dlci[0].state == CONNECTED)
				    || (ts0710->dlci[0].state == FLOW_STOPPED)) {
					retval = 0;
					break;
				} else if (ts0710->dlci[0].state == REJECTED) {
					retval = -EREJECTED;
					break;
				} else if (ts0710->dlci[0].state == DISCONNECTED) {
					break;
				} else if (signal_pending(current)) {
					TS0710_ERROR("dlc %d Wait for connecting got signal!\n",
					     dlci);
					retval = -EAGAIN;
					break;
				} else if ((jiffies - t) >= TS0710MUX_TIME_OUT) {
					TS0710_ERROR("dlc %d Wait for connecting timeout!\n",
					     dlci);
					continue;
				} else if (ts0710->dlci[0].state == CONNECTING) {
					continue;
				}
			}

			if (ts0710->dlci[0].state == CONNECTING) {
				ts0710->dlci[0].state = DISCONNECTED;
			}
		} else if ((ts0710->dlci[0].state != DISCONNECTED)
			   && (ts0710->dlci[0].state != REJECTED)) {
			TS0710_ERROR("dlc %d state is invalid!\n", dlci);
			return retval;
		} else {
			ts0710->initiator = 1;
			ts0710->dlci[0].state = CONNECTING;
			ts0710->dlci[0].initiator = 1;
			try = 10;
			while (try--) {
				t = jiffies;
				send_sabm(ts0710, 0);
				interruptible_sleep_on_timeout(&ts0710->dlci[0].
							       open_wait,
							       TS0710MUX_TIME_OUT);
				if ((ts0710->dlci[0].state == CONNECTED)
				    || (ts0710->dlci[0].state == FLOW_STOPPED)) {
					retval = 0;
					break;
				} else if (ts0710->dlci[0].state == REJECTED) {
					TS0710_ERROR("dlc %d Send SABM got rejected!\n",
					     dlci);
					retval = -EREJECTED;
					break;
				} else if (signal_pending(current)) {
					TS0710_ERROR("dlc %d Send SABM got signal!\n",
					     dlci);
					retval = -EAGAIN;
					break;
				} else if ((jiffies - t) >= TS0710MUX_TIME_OUT) {
					TS0710_ERROR("dlc %d Send SABM timeout!\n",
					     dlci);
					continue;
				}
			}

			if (ts0710->dlci[0].state == CONNECTING) {
				ts0710->dlci[0].state = DISCONNECTED;
			}
			wake_up_interruptible(&ts0710->dlci[0].open_wait);
		}
	} else {		/* other channel */
		if ((ts0710->dlci[0].state != CONNECTED)
		    && (ts0710->dlci[0].state != FLOW_STOPPED)) {
			return retval;
		} else if ((ts0710->dlci[dlci].state == CONNECTED)
			   || (ts0710->dlci[dlci].state == FLOW_STOPPED)) {
			return 0;
		} else if ((ts0710->dlci[dlci].state == NEGOTIATING)
			   || (ts0710->dlci[dlci].state == CONNECTING)) {
			/* Reentry */
			try = 8;
			while (try--) {
				t = jiffies;
				interruptible_sleep_on_timeout(&ts0710->dlci[dlci].open_wait,
							       TS0710MUX_TIME_OUT);
				if ((ts0710->dlci[dlci].state == CONNECTED)
				    || (ts0710->dlci[dlci].state == FLOW_STOPPED)) {
					retval = 0;
					break;
				} else if (ts0710->dlci[dlci].state == REJECTED) {
					retval = -EREJECTED;
					break;
				} else if (ts0710->dlci[dlci].state == DISCONNECTED) {
					break;
				} else if (signal_pending(current)) {
					TS0710_ERROR("dlc %d Wait for connecting got signal!\n",
					     dlci);
					retval = -EAGAIN;
					break;
				} else if ((jiffies - t) >= TS0710MUX_TIME_OUT) {
					TS0710_ERROR("dlc %d Wait for connecting timeout!\n", dlci);
					continue;
				} else
				    if ((ts0710->dlci[dlci].state == NEGOTIATING)
					|| (ts0710->dlci[dlci].state == CONNECTING)) {
					continue;
				}
			}

			if ((ts0710->dlci[dlci].state == NEGOTIATING)
			    || (ts0710->dlci[dlci].state == CONNECTING)) {
				ts0710->dlci[dlci].state = DISCONNECTED;
			}
		} else if ((ts0710->dlci[dlci].state != DISCONNECTED)
			   && (ts0710->dlci[dlci].state != REJECTED)) {
			TS0710_ERROR("dlc %d state is invalid!\n", dlci);
			return retval;
		} else {
			ts0710->dlci[dlci].state = NEGOTIATING;
			ts0710->dlci[dlci].initiator = 1;
			try = 3;
			while (try--) {
				t = jiffies;
				send_pn_msg(ts0710, 7, ts0710->dlci[dlci].mtu,
					    0, 0, dlci, 1);
				interruptible_sleep_on_timeout(&ts0710->
							       dlci[dlci].
							       open_wait,
							       TS0710MUX_TIME_OUT);
				if (ts0710->dlci[dlci].state == CONNECTING) {
					break;
				} else if (signal_pending(current)) {
					TS0710_ERROR("dlc %d Send pn_msg got signal!\n", dlci);
					retval = -EAGAIN;
					break;
				} else if ((jiffies - t) >= TS0710MUX_TIME_OUT) {
					TS0710_ERROR("dlc %d Send pn_msg timeout!\n", dlci);
					continue;
				}
			}

			if (ts0710->dlci[dlci].state == CONNECTING) {
				try = 3;
				while (try--) {
					t = jiffies;
					send_sabm(ts0710, dlci);
					interruptible_sleep_on_timeout(&ts0710->
								       dlci
								       [dlci].
								       open_wait,
								       TS0710MUX_TIME_OUT);
					if ((ts0710->dlci[dlci].state == CONNECTED)
					    || (ts0710->dlci[dlci].state == FLOW_STOPPED)) {
						retval = 0;
						break;
					} else if (ts0710->dlci[dlci].state == REJECTED) {
						TS0710_ERROR("dlc %d Send SABM got rejected!\n", dlci);
						retval = -EREJECTED;
						break;
					} else if (signal_pending(current)) {
						TS0710_ERROR("dlc %d Send SABM got signal!\n", dlci);
						retval = -EAGAIN;
						break;
					} else if ((jiffies - t) >=
						   TS0710MUX_TIME_OUT) {
						TS0710_ERROR("dlc %d Send SABM timeout!\n", dlci);
						continue;
					}
				}
			}

			if ((ts0710->dlci[dlci].state == NEGOTIATING)
			    || (ts0710->dlci[dlci].state == CONNECTING)) {
				ts0710->dlci[dlci].state = DISCONNECTED;
			}
			wake_up_interruptible(&ts0710->dlci[dlci].open_wait);
		}
	}
	return retval;
}

static int ts0710_exec_test_cmd(void)
{
	ts0710_con *ts0710 = &ts0710_connection;
	__u8 *f_buf;		/* Frame buffer */
	__u8 *d_buf;		/* Data buffer */
	int retval = -EFAULT;
	int j;
	unsigned long t;

	if (ts0710->be_testing) {
		/* Reentry */
		t = jiffies;
		interruptible_sleep_on_timeout(&ts0710->test_wait,
					       3 * TS0710MUX_TIME_OUT);
		if (ts0710->be_testing == 0) {
			if (ts0710->test_errs == 0) {
				retval = 0;
			} else {
				retval = -EFAULT;
			}
		} else if (signal_pending(current)) {
			TS0710_ERROR("Wait for Test_cmd response got signal!\n");
			retval = -EAGAIN;
		} else if ((jiffies - t) >= 3 * TS0710MUX_TIME_OUT) {
			TS0710_ERROR("Wait for Test_cmd response timeout!\n");
			retval = -EFAULT;
		}
	} else {
		ts0710->be_testing = 1;	/* Set the flag */

		f_buf = (__u8 *) kmalloc(TEST_PATTERN_SIZE + 32, GFP_KERNEL);
		d_buf = (__u8 *) kmalloc(TEST_PATTERN_SIZE + 32, GFP_KERNEL);
		if ((!f_buf) || (!d_buf)) {
			if (f_buf) {
				kfree(f_buf);
			}
			if (d_buf) {
				kfree(d_buf);
			}

			ts0710->be_testing = 0;	/* Clear the flag */
			ts0710->test_errs = TEST_PATTERN_SIZE;
			wake_up_interruptible(&ts0710->test_wait);
			return -ENOMEM;
		}

		for (j = 0; j < TEST_PATTERN_SIZE; j++) {
			d_buf[j] = j & 0xFF;
		}

		t = jiffies;
		ts0710_test_msg(ts0710, d_buf, TEST_PATTERN_SIZE, MCC_CMD, f_buf);
		interruptible_sleep_on_timeout(&ts0710->test_wait,
					       2 * TS0710MUX_TIME_OUT);
		if (ts0710->be_testing == 0) {
			if (ts0710->test_errs == 0) {
				retval = 0;
			} else {
				retval = -EFAULT;
			}
		} else if (signal_pending(current)) {
			TS0710_ERROR("Send Test_cmd got signal!\n");
			retval = -EAGAIN;
		} else if ((jiffies - t) >= 2 * TS0710MUX_TIME_OUT) {
			TS0710_ERROR("Send Test_cmd timeout!\n");
			ts0710->test_errs = TEST_PATTERN_SIZE;
			retval = -EFAULT;
		}

		ts0710->be_testing = 0;	/* Clear the flag */
		wake_up_interruptible(&ts0710->test_wait);

		/* Release buffer */
		if (f_buf) {
			kfree(f_buf);
		}
		if (d_buf) {
			kfree(d_buf);
		}
	}

	return retval;
}

static void mux_sched_send(void)
{
	if (!schedule_work(&send_tqueue)) {
		TS0710_INFO("Failed to schedule work\n");
	}
}

/****************************
 * TTY driver routines
*****************************/

static void mux_close(struct tty_struct *tty, struct file *filp)
{
	ts0710_con *ts0710 = &ts0710_connection;
	int line;
	__u8 dlci;

	UNUSED_PARAM(filp);

	if (!tty) {
		return;
	}
	line = tty->index;
	if ((line < TS0710MUX_MINOR_START) || (line >= NR_MUXS)) {
		TS0710_ERROR("mux%d is not a valid port\n",line);
		return;
	}

	if (mux_tty[line] > 0)
		mux_tty[line]--;

	dlci = line;
	if (mux_tty[line] == 0)
		ts0710_close_channel(line);

	if (line == 1) {
		/* TODO - temporary, this assumes that dlc1 always exists and always
		   last to close */
		TS0710_INFO("dlc 1 closing - therefore close cmd channel as well\n");

		/* TODO - for Siemens TC65 you need to send a CLD command - no
		   further comms afterwards is possible */
		send_closedown_msg();
		ts0710_upon_disconnect();

		/* if not Siemens use ts0710_close_channel(0); */
	}

	TS0710_INFO("closing /dev/mux%d\n", line);

	if (mux_tty[line] == 0) {
		if ((mux_send_info_flags[line]) && (mux_send_info[line])) {
			mux_send_info_flags[line] = 0;
			kfree(mux_send_info[line]);
			mux_send_info[line] = 0;
		}

		if ((mux_recv_info_flags[line]) && (mux_recv_info[line]) &&
			(mux_recv_info[line]->total == 0)) {
			mux_recv_info_flags[line] = 0;
			free_mux_recv_struct(mux_recv_info[line]);
			mux_recv_info[line] = 0;
		}

		ts0710_flow_on(dlci, ts0710);
		schedule_work(&post_recv_tqueue);

		wake_up_interruptible(&tty->read_wait);
		wake_up_interruptible(&tty->write_wait);
		tty->packet = 0;
	}
}

static void mux_throttle(struct tty_struct *tty)
{
	ts0710_con *ts0710 = &ts0710_connection;
	int line;
	int i;
	__u8 dlci;

	if (!tty) {
		return;
	}

	line = tty->index;
	if ((line < TS0710MUX_MINOR_START) || (line >= NR_MUXS)) {
		TS0710_ERROR("mux%d is not a valid port\n",line);
		return;
	}

	TS0710_DEBUG("minor number is: %d\n", line);

	dlci = line;
	if ((ts0710->dlci[0].state != CONNECTED)
	    && (ts0710->dlci[0].state != FLOW_STOPPED)) {
		return;
	} else if ((ts0710->dlci[dlci].state != CONNECTED)
		   && (ts0710->dlci[dlci].state != FLOW_STOPPED)) {
		return;
	}

	if (ts0710->dlci[dlci].flow_control) {
		return;
	}

	for (i = 0; i < 3; i++) {
		if (ts0710_msc_msg(
				ts0710, EA | FC | RTC | RTR | DV, MCC_CMD, dlci) < 0) {
			continue;
		} else {
			TS0710_LOG("Send Flow off on dlc %d\n", dlci);
			ts0710->dlci[dlci].flow_control = 1;
			break;
		}
	}
}

static void mux_unthrottle(struct tty_struct *tty)
{
	ts0710_con *ts0710 = &ts0710_connection;
	int line;
	__u8 dlci;
	mux_recv_struct *recv_info;

	if (!tty) {
		return;
	}
	line = tty->index;
	if ((line < TS0710MUX_MINOR_START) || (line >= NR_MUXS)) {
		TS0710_ERROR("mux%d is not a valid port\n",line);
		return;
	}

	if ((!mux_recv_info_flags[line]) || (!mux_recv_info[line])) {
		return;
	}

	TS0710_DEBUG("minor number is: %d\n", line);

	recv_info = mux_recv_info[line];
	dlci = line;

	if (recv_info->total) {
		recv_info->post_unthrottle = 1;
		schedule_work(&post_recv_tqueue);
	} else {
		ts0710_flow_on(dlci, ts0710);
	}
}

static int mux_chars_in_buffer(struct tty_struct *tty)
{
	ts0710_con *ts0710 = &ts0710_connection;
	int retval;
	int line;
	__u8 dlci;
	mux_send_struct *send_info;

	retval = TS0710MUX_MAX_CHARS_IN_BUF;
	if (!tty) {
		goto out;
	}
	line = tty->index;
	if ((line < TS0710MUX_MINOR_START) || (line >= NR_MUXS)) {
		TS0710_ERROR("mux%d is not a valid port\n",line);
		goto out;
	}

	dlci = line;
	if (ts0710->dlci[0].state == FLOW_STOPPED) {
		TS0710_ERROR
		    ("Flow stopped on all chnls, returning MAX chars in buffer\n");
		goto out;
	} else if (ts0710->dlci[dlci].state == FLOW_STOPPED) {
		TS0710_DEBUG("Flow stopped, returning MAX chars in buffer\n");
		goto out;
	} else if (ts0710->dlci[dlci].state != CONNECTED) {
		TS0710_DEBUG("dlc %d not connected\n", dlci);
		goto out;
	}

	if (!(mux_send_info_flags[line])) {
		goto out;
	}
	send_info = mux_send_info[line];
	if (!send_info) {
		goto out;
	}
	if (send_info->filled) {
		goto out;
	}

	retval = 0;

out:
	return retval;
}

static int mux_write(struct tty_struct *tty,
		     const unsigned char *buf, int count)
{
	ts0710_con *ts0710 = &ts0710_connection;
	int line;
	__u8 dlci;
	mux_send_struct *send_info;
	__u8 *d_buf;
	__u16 c;

	if (count <= 0) {
		return 0;
	}

	if (!tty) {
		return 0;
	}

	line = tty->index;
	if ((line < TS0710MUX_MINOR_START) || (line >= NR_MUXS)) {
		TS0710_ERROR("mux%d is not a valid port\n",line);
		return -ENODEV;
	}

	dlci = line;
	if (ts0710->dlci[0].state == FLOW_STOPPED) {
		TS0710_ERROR("Flow stopped on all chnls, returning zero /dev/mux%d\n",
		     line);
		return 0;
	} else if (ts0710->dlci[dlci].state == FLOW_STOPPED) {
		TS0710_INFO("Flow stopped, returning zero /dev/mux%d\n", line);
		return 0;
	} else if (ts0710->dlci[dlci].state == CONNECTED) {

		if (!(mux_send_info_flags[line])) {
			TS0710_ERROR("mux_send_info_flags[%d] == 0\n", line);
			return -ENODEV;
		}
		send_info = mux_send_info[line];
		if (!send_info) {
			TS0710_ERROR("mux_send_info[%d] == 0\n", line);
			return -ENODEV;
		}

		c = min(count, (ts0710->dlci[dlci].mtu - SHORT_UIH_MTU_HDR));
		if (c <= 0) {
			return 0;
		}

		if (test_and_set_bit(BUF_BUSY, &send_info->flags))
			return 0;

		if (send_info->filled) {
			clear_bit(BUF_BUSY, &send_info->flags);
			return 0;
		}

		d_buf = ((__u8 *) send_info->buf) + TS0710MUX_SEND_BUF_OFFSET;

		memcpy(d_buf, buf, c);

		TS0710_DEBUGHEX(d_buf, c);

		send_info->frame = d_buf;
		queue_uih(send_info, c, ts0710, dlci);
		send_info->filled = 1;
		clear_bit(BUF_BUSY, &send_info->flags);

		if (mux_ldisc_chars_in_buffer(COMM_FOR_MUX_TTY) == 0) {
			/* Sending bottom half should be
			   run after return from this function */
			mux_sched_send();
		}
		return c;
	} else {
		TS0710_ERROR("DLCI %d not connected\n", dlci);
		return -EDISCONNECTED;
	}
}

static int mux_write_room(struct tty_struct *tty)
{
	ts0710_con *ts0710 = &ts0710_connection;
	int retval = 0;
	int line;
	__u8 dlci;
	mux_send_struct *send_info;

	if (!tty) {
		goto out;
	}
	line = tty->index;
	if ((line < TS0710MUX_MINOR_START) || (line >= NR_MUXS)) {
		TS0710_ERROR("mux%d is not a valid port\n",line);
		goto out;
	}

	dlci = line;
	if (ts0710->dlci[0].state == FLOW_STOPPED) {
		TS0710_ERROR("Flow stopped on all chnls, returning ZERO\n");
		goto out;
	} else if (ts0710->dlci[dlci].state == FLOW_STOPPED) {
		TS0710_INFO("Flow stopped, returning ZERO\n");
		goto out;
	} else if (ts0710->dlci[dlci].state != CONNECTED) {
		TS0710_ERROR("dlc %d not connected\n", dlci);
		goto out;
	}

	if (!(mux_send_info_flags[line])) {
		goto out;
	}
	send_info = mux_send_info[line];
	if (!send_info) {
		goto out;
	}
	if (send_info->filled) {
		goto out;
	}

	retval = ts0710->dlci[dlci].mtu - 1; /* TODO why -1 ? */

out:
	return retval;
}

#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,39))
static int mux_ioctl(struct tty_struct *tty, struct file *file,
	unsigned int cmd, unsigned long arg)
#else
static int mux_ioctl(struct tty_struct *tty, unsigned int cmd,
	unsigned long arg)
#endif
{
	ts0710_con *ts0710 = &ts0710_connection;
	int line;
	__u8 dlci;

#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,39))
	UNUSED_PARAM(file);
#endif
	UNUSED_PARAM(arg);

	if (!tty) {
		return -EIO;
	}
	line = tty->index;
	if ((line < TS0710MUX_MINOR_START) || (line >= NR_MUXS)) {
		TS0710_ERROR("mux%d is not a valid port\n",line);
		return -ENODEV;
	}

	dlci = line;
	switch (cmd) {
	case TS0710MUX_IO_MSC_HANGUP:
		if (ts0710_msc_msg(ts0710, EA | RTR | DV, MCC_CMD, dlci) < 0) {
			return -EAGAIN;
		}
		return 0;
	case TS0710MUX_IO_TEST_CMD:
		return ts0710_exec_test_cmd();
	default:
		break;
	}
	return -ENOIOCTLCMD;
}

static void mux_flush_buffer(struct tty_struct *tty)
{
	int line;

	if (!tty) {
		return;
	}

	line = tty->index;
	if ((line < TS0710MUX_MINOR_START) || (line >= NR_MUXS)) {
		TS0710_ERROR("mux%d is not a valid port\n",line);
		return;
	}

	if ((mux_send_info_flags[line]) && (mux_send_info[line])
	    && (mux_send_info[line]->filled)) {

		mux_send_info[line]->filled = 0;
	}

	wake_up_interruptible(&tty->write_wait);
#ifdef SERIAL_HAVE_POLL_WAIT
	wake_up_interruptible(&tty->poll_wait);
#endif
#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,27))
	if ((tty->flags & (1 << TTY_DO_WRITE_WAKEUP)) &&
	    tty->ldisc.write_wakeup) {
		(tty->ldisc.write_wakeup) (tty);
	}
#elsif (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,31))
	if ((tty->flags & (1 << TTY_DO_WRITE_WAKEUP)) &&
	    tty->ldisc.ops->write_wakeup) {
		(tty->ldisc.ops->write_wakeup) (tty);
	}
#else
	if ((tty->flags & (1 << TTY_DO_WRITE_WAKEUP)) &&
	    tty->ldisc->ops->write_wakeup) {
		(tty->ldisc->ops->write_wakeup) (tty);
	}
#endif
}

static int mux_open(struct tty_struct *tty, struct file *filp)
{
	int retval = -ENODEV;
	int line = 0;
	__u8 dlci;
	mux_send_struct *send_info;
	mux_recv_struct *recv_info;

	UNUSED_PARAM(filp);

	if (!tty) {
		goto out;
	}
	line = tty->index;
	if ((line < TS0710MUX_MINOR_START) || (line >= NR_MUXS)) {
		TS0710_ERROR("mux%d is not a valid port\n",line);
		goto out;
	}

#ifdef TS0710SERVER
	/* do nothing as a server */
	mux_tty[line]++;
	retval = 0;
#else
	mux_tty[line]++;

	dlci = line;
	mux_table[line] = tty;

	/* Open server channel 0 first */
	if ((retval = ts0710_open_channel(0)) != 0) {
		TS0710_ERROR("Can't connect server channel 0!\n");
		ts0710_init();

		mux_tty[line]--;
		goto out;
	}

	/* Allocate memory first. As soon as connection has been established,
	 * MUX may receive */
	if (mux_send_info_flags[line] == 0) {
		send_info = (mux_send_struct *) kmalloc(sizeof(mux_send_struct),
						GFP_KERNEL);
		if (!send_info) {
			retval = -ENOMEM;

			mux_tty[line]--;
			goto out;
		}
		send_info->length = 0;
		send_info->flags = 0;
		send_info->filled = 0;
		mux_send_info[line] = send_info;
		mux_send_info_flags[line] = 1;
	}

	if (mux_recv_info_flags[line] == 0) {
		recv_info = (mux_recv_struct *) kmalloc(sizeof(mux_recv_struct),
						GFP_KERNEL);
		if (!recv_info) {
			mux_send_info_flags[line] = 0;
			kfree(mux_send_info[line]);
			mux_send_info[line] = 0;
			retval = -ENOMEM;

			mux_tty[line]--;
			goto out;
		}
		recv_info->length = 0;
		recv_info->total = 0;
		recv_info->mux_packet = 0;
		recv_info->next = 0;
		recv_info->no_tty = line;
		recv_info->post_unthrottle = 0;
		mux_recv_info[line] = recv_info;
		mux_recv_info_flags[line] = 1;
	}

	/* Now establish DLCI connection */
	if ((retval = ts0710_open_channel(dlci)) != 0) {
		TS0710_ERROR("Can't connect channel %d!\n",dlci);
		ts0710_reset_dlci(dlci);

		mux_send_info_flags[line] = 0;
		kfree(mux_send_info[line]);
		mux_send_info[line] = 0;

		mux_recv_info_flags[line] = 0;
		free_mux_recv_struct(mux_recv_info[line]);
		mux_recv_info[line] = 0;

		mux_tty[line]--;
		goto out;
	}

	retval = 0;
#endif
    TS0710_INFO("opening /dev/mux%d\n", line);

out:
	TS0710_DEBUG("returning %d (mux_tty[%d] = %d)\n", retval, line,
		mux_tty[line]);
	return retval;
}

static void mux_dispatcher(struct tty_struct *tty)
{
	UNUSED_PARAM(tty);
	schedule_work(&receive_tqueue);
}

static unsigned char* processed_received_data(unsigned char *start,
	unsigned char* end)
{
	unsigned char *start_flag = 0;
	int framelen = -1;
	unsigned char *search = start;

	while (1) {
		if (start_flag == 0) {
			/* Frame Start Flag not found */
			unsigned char* debug_start = search;
			framelen = -1;
			while (search < end) {
				if (*search == TS0710_BASIC_FLAG) {
					/* Just in case more than one flag */
					unsigned char* chk = search + 1;
					while ((chk < end) && (*chk == TS0710_BASIC_FLAG)) {
						search = chk;
						TS0710_INFO("Additional flag\n");
						chk++;
					}
					if (search != debug_start) {
						TS0710_INFO("Skipped %d\n",(int)(search - debug_start));
					}
					start_flag = search;
					break;
				}
#ifdef TS0710LOG
				else {
					TS0710_LOG(">S %02x %c\n", *search, *search);
				}
#endif
				search++;
			}

			if (start_flag == 0) {
				if (debug_start < end) {
					TS0710_ERRORHEX(start, (int)(end - start));
					TS0710_ERROR("no start of frame found %02x (%d from end)\n",
						*debug_start, (int) (end - debug_start));
				}
				TS0710_INFO("No frame found for now\n");
				/* reset ptr to start */
				return start;
			}
		} else {
			short_frame *short_pkt;
			long_frame *long_pkt;

			/* Frame Start Flag found */
			/* 1 start flag + 1 address + 1 control + 1 or 2 length +
			   lengths data + 1 FCS + 1 end flag */
			if ((framelen == -1) &&
				((end - start_flag) > TS0710_MAX_HDR_SIZE)) {

				short_pkt = (short_frame *) (start_flag + ADDRESS_FIELD_OFFSET);
				if (short_pkt->h.length.ea == 1) {
					/* short frame */
					framelen = TS0710_MAX_HDR_SIZE + short_pkt->h.length.len + 1;
				} else {
					/* long frame */
					long_pkt = (long_frame *) (start_flag + ADDRESS_FIELD_OFFSET);
					framelen = TS0710_MAX_HDR_SIZE +
						GET_LONG_LENGTH(long_pkt->h.length) + 2;
						
					/* mtu is 98 on Siemens so don't expect long frames */
					TS0710_ERROR("Siemens unexpected long frame %d\n",framelen); 
				}

				if (framelen > TS0710MUX_MAX_TOTAL_FRAME_SIZE) {
					TS0710_LOGSTR_FRAME(0, start_flag, (end - start_flag));
					TS0710_ERROR
					    ("Frame len %d > Max frame:%d\n",
					     framelen, TS0710MUX_MAX_TOTAL_FRAME_SIZE);
					search = start_flag + 1;
					start_flag = 0;
					continue;
				}
			}

			if ((framelen != -1) && ((end - start_flag) >= framelen)) {
				
				/* The frame should exist in the buffer */
				if (*(start_flag + framelen - 1) == TS0710_BASIC_FLAG) {
					__u8 *uih_data_start;
					__u32 uih_len;
					__u32 crc_error;

					/* End flag is where we expected it */
					TS0710_LOGSTR_FRAME(0, start_flag, framelen);
					TS0710_DEBUGHEX(start_flag, framelen);

					short_pkt =
						(short_frame *) (start_flag + ADDRESS_FIELD_OFFSET);
						
					if ((short_pkt->h.length.ea) == 0) {
						long_pkt = (long_frame *) (start_flag +
								    ADDRESS_FIELD_OFFSET);
						uih_len = GET_LONG_LENGTH(long_pkt->h.length);
						uih_data_start = long_pkt->h.data;

						crc_error = crc_check((__u8*) (start_flag + DLC_OFFSET),
							      LONG_CRC_CHECK,*(uih_data_start + uih_len));
					} else {
						uih_len = short_pkt->h.length.len;
						uih_data_start = short_pkt->data;

						crc_error = crc_check((__u8*) (start_flag + DLC_OFFSET),
							      SHORT_CRC_CHECK,*(uih_data_start + uih_len));
					}

					if (!crc_error) {
						ts0710_recv_data(&ts0710_connection, start_flag +
							ADDRESS_FIELD_OFFSET, framelen - 2);

					} else {
						TS0710_ERRORHEX(start_flag, framelen);
						TS0710_ERROR("crc error [framelen %d]\n",
							framelen);
						/* disregard the frame */
						search = start_flag + 1;
						start_flag = 0;
						continue;
					}

					search = start_flag + framelen;

					if ((end - start_flag) > framelen) {
						if (TS0710_BASIC_FLAG != *search) {
							/* Some modems use the end flag as the start flag
							 * of the next frame */
							TS0710_INFO("No following start flag\n");
							TS0710_DEBUGHEX(search, (end - search));
							search--;
						} 
					}
				} else {
					TS0710_LOGSTR_FRAME(0, start_flag, framelen);
					TS0710_ERRORHEX(start_flag, framelen);
					TS0710_ERROR("Expected end flag missing [framelen %d]\n",
						framelen);
					search = start_flag + 1;
				}

				if (search == end) {
					TS0710_DEBUG("Nothing more to process\n");
					return start;
				}
				start_flag = 0;
				continue;
			} else {
				TS0710_DEBUG("Not received all the frame yet\n");
			}

			if (start_flag != start) {
				/* move what is left to the start of the recv buffer */
				int remaining = end - start_flag;
				TS0710_INFO("Moved %d remaining in recv buf\n",remaining);
				memcpy(start, start_flag, remaining);

				return start + remaining;
			}
			break;
		}		/* End Frame Start Flag found */
	}			/* End while(1) */
    return end;
}

static unsigned char tbuf[TS0710MUX_MAX_RECV_BUF_SIZE];
static unsigned char *tbuf_ptr = tbuf;

#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,20))
static void receive_worker(void *private_)
#else
static void receive_worker(struct work_struct *private_)
#endif
{
	int recv_count, tbuf_free, tbuf_read;

	UNUSED_PARAM(private_);

	if (!COMM_FOR_MUX_TTY)
		return;

	if (test_and_set_bit(RECV_RUNNING, &mux_recv_flags)) {
		schedule_work(&receive_tqueue);
		return;
	}

	TS0710_DEBUG(">>\n");

	while (1) {
		tbuf_free = TS0710MUX_MAX_RECV_BUF_SIZE - (tbuf_ptr - tbuf);
		TS0710_DEBUG("%i bytes free\n", tbuf_free);
		tbuf_read = get_from_inbuf_list(tbuf_ptr, tbuf_free);
		if (tbuf_read == 0) {
			TS0710_DEBUG("finished read\n");
			break;
		} else {
			TS0710_DEBUG("read %i bytes.\n", tbuf_read);
		};
		tbuf_ptr += tbuf_read;
	};
 
	recv_count = (tbuf_ptr - tbuf);

	/* received the following bytes */
	TS0710_DEBUGHEX(tbuf, recv_count);
 
	tbuf_ptr = processed_received_data(tbuf,tbuf_ptr);

 	TS0710_INFO("recv buf %d, after processed %d\n",
 		recv_count,(int) (tbuf_ptr - tbuf));

	clear_bit(RECV_RUNNING, &mux_recv_flags);
	TS0710_DEBUG("<<\n");
}

#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,20))
static void post_recv_worker(void *private_)
#else
static void post_recv_worker(struct work_struct *private_)
#endif
{
	ts0710_con *ts0710 = &ts0710_connection;
	int tty_idx;
	struct tty_struct *tty;
	__u8 post_recv;
	__u8 flow_control;
	__u8 dlci;
	mux_recv_struct *recv_info, *recv_info2, *post_recv_q;
	int recv_room;
	mux_recv_packet *recv_packet, *recv_packet2;

	UNUSED_PARAM(private_);

	if (test_and_set_bit(RECV_RUNNING, &mux_recv_flags)) {
		schedule_work(&post_recv_tqueue);
		return;
	}

	TS0710_DEBUG(">> mux_recv_queue=%p\n",mux_recv_queue);

	post_recv = 0;
	if (!mux_recv_queue) {
		goto out;
	}

	post_recv_q = NULL;
	recv_info2 = mux_recv_queue;
	while ((recv_info = recv_info2)) {
		recv_info2 = recv_info->next;

		if (!(recv_info->total)) {
			TS0710_ERROR
			    ("Error: Should not get here, recv_info->total == 0 \n");
			continue;
		}

		tty_idx = recv_info->no_tty;

		dlci = tty_idx;

		tty = mux_table[tty_idx];
		if ((!mux_tty[tty_idx]) || (!tty)) {
			TS0710_INFO
			    ("No application waiting for, free recv_info! tty_idx:%d\n",
			     tty_idx);
			mux_recv_info_flags[tty_idx] = 0;
			free_mux_recv_struct(mux_recv_info[tty_idx]);
			mux_recv_info[tty_idx] = 0;
			ts0710_flow_on(dlci, ts0710);
			continue;
		}

		TS0710_DEBUG("/dev/mux%d recv_info->total is: %d\n", tty_idx,
			     recv_info->total);

		if (test_bit(TTY_THROTTLED, &tty->flags)) {
			add_post_recv_queue(&post_recv_q, recv_info);
			continue;
		}

		flow_control = 0;
		recv_packet2 = recv_info->mux_packet;
		while (recv_info->total) {
			recv_room = 65535;
			if (tty->receive_room)
				recv_room = tty->receive_room;

			if (recv_info->length) {
				if (recv_room < recv_info->length) {
					flow_control = 1;
					break;
				}

				/* Put queued data into read buffer of tty */
				TS0710_DEBUG
				    ("Put queued recv data into read buffer of /dev/mux%d\n",
				     tty_idx);
				TS0710_DEBUGHEX(recv_info->data, recv_info->length);
#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,27))
				(tty->ldisc.receive_buf) (tty, recv_info->data, NULL,
							  recv_info->length);
#elsif (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,31))
				(tty->ldisc.ops->receive_buf) (tty, recv_info->data, NULL,
							  recv_info->length);
#else
				(tty->ldisc->ops->receive_buf) (tty, recv_info->data, NULL,
							  recv_info->length);
#endif
				recv_info->total -= recv_info->length;
				recv_info->length = 0;
			} else {	/* recv_info->length == 0 */
				if ((recv_packet = recv_packet2)) {
					recv_packet2 = recv_packet->next;

					if (recv_room < recv_packet->length) {
						flow_control = 1;
						recv_info->mux_packet = recv_packet;
						break;
					}

					/* Put queued data into read buffer of tty */
					TS0710_DEBUG
					    ("Put queued recv data into read buffer of /dev/mux%d\n",
					     tty_idx);
					TS0710_DEBUGHEX(recv_packet->data, recv_packet->length);
#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,27))
					(tty->ldisc.receive_buf) (tty, recv_packet->data, NULL,
								  recv_packet->length);
#elsif (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,31))
					(tty->ldisc.ops->receive_buf) (tty,recv_packet->data, NULL,
								  recv_packet->length);
#else
					(tty->ldisc->ops->receive_buf) (tty,recv_packet->data, NULL,
								  recv_packet->length);
#endif
					recv_info->total -= recv_packet->length;
					free_mux_recv_packet(recv_packet);
				} else {
					TS0710_ERROR
					    ("Error: Should not get here, recv_info->total is:%u \n",
					    	recv_info->total);
				}
			} /* End recv_info->length == 0 */
		} /* End while( recv_info->total ) */

		if (!(recv_info->total)) {
			/* Important clear */
			recv_info->mux_packet = 0;

			if (recv_info->post_unthrottle) {
				/* Do something for post_unthrottle */
				ts0710_flow_on(dlci, ts0710);
				recv_info->post_unthrottle = 0;
			}
		} else {
			add_post_recv_queue(&post_recv_q, recv_info);

			if (flow_control) {
				/* Do something for flow control */
				if (recv_info->post_unthrottle) {
					set_bit(TTY_THROTTLED, &tty->flags);
					recv_info->post_unthrottle = 0;
				} else {
					ts0710_flow_off(tty, dlci, ts0710);
				}
			} /* End if( flow_control ) */
		}
	} /* End while( (recv_info = recv_info2) ) */

	mux_recv_queue = post_recv_q;

      out:
	clear_bit(RECV_RUNNING, &mux_recv_flags);
	TS0710_DEBUG("<<\n");
}

static void mux_sender(void)
{
	mux_send_struct *send_info;
	int chars;
	__u8 idx;

	chars = mux_ldisc_chars_in_buffer(COMM_FOR_MUX_TTY);
	if (!chars) {
		TS0710_DEBUG("no chars in driver buffer\n");
		/* check if mux ttys are waiting to write */
		mux_sched_send();
		return;
	}

	idx = mux_send_info_idx;
	if ((idx < NR_MUXS) && (mux_send_info_flags[idx])) {
		send_info = mux_send_info[idx];
		if ((send_info) && (send_info->filled)
		    && (send_info->length <= (TS0710MUX_SERIAL_BUF_SIZE - chars))) {

			mux_sched_send();
		}
	}
}

#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,20))
static void send_worker(void *private_)
#else
static void send_worker(struct work_struct *private_)
#endif
{
	ts0710_con *ts0710 = &ts0710_connection;
	__u8 j;
	mux_send_struct *send_info;
	int chars;
	struct tty_struct *tty;
	__u8 dlci;

	UNUSED_PARAM(private_);

	TS0710_DEBUG(">>\n");

	mux_send_info_idx = NR_MUXS;

	if (ts0710->dlci[0].state == FLOW_STOPPED) {
		TS0710_ERROR("Flow stopped on all channels\n");
		return;
	}

	for (j = 0; j < NR_MUXS; j++) {

		if (!(mux_send_info_flags[j])) {
			continue;
		}

		send_info = mux_send_info[j];
		if (!send_info) {
			continue;
		}

		if (!(send_info->filled)) {
			continue;
		}

		dlci = j;
		if (ts0710->dlci[dlci].state == FLOW_STOPPED) {
			TS0710_INFO("Flow stopped on channel dlc %d\n",
				     dlci);
			continue;
		} else if (ts0710->dlci[dlci].state != CONNECTED) {
			TS0710_INFO("dlc %d not connected\n", dlci);
			send_info->filled = 0;
			continue;
		}

		chars = mux_ldisc_chars_in_buffer(COMM_FOR_MUX_TTY);
		if (chars > TS0710MUX_SERIAL_BUF_SIZE) {
			TS0710_ERROR("Unexpected driver num chars in buff %d\n", chars);
			mux_send_info_idx = j;
			break;
		} else if (send_info->length <= (TS0710MUX_SERIAL_BUF_SIZE - chars)) {
			int bytes_sent;
			TS0710_DEBUG("Send queued UIH from /dev/mux%d\n", j);
			bytes_sent = basic_write((__u8 *) send_info->frame,send_info->length);
			if (bytes_sent < send_info->length) {
				TS0710_INFO("Failed to send complete UIH (%d < %d)\n",
					bytes_sent,send_info->length);
				/* Hopefully the receiver will disregard the partial frame */
				mux_send_info_idx = j;
				break;
			}
			send_info->length = 0;
			send_info->filled = 0;
		} else {
			TS0710_INFO("Wait for %d to become available on mux%d\n",
				send_info->length,j);
			mux_send_info_idx = j;
			break;
		}
	} /* End for() loop */

	/* Queue UIH data to be transmitted */
	for (j = 0; j < NR_MUXS; j++) {

		if (!(mux_send_info_flags[j])) {
			continue;
		}

		send_info = mux_send_info[j];
		if (!send_info) {
			continue;
		}

		if (send_info->filled) {
			continue;
		}

		/* Now queue UIH data to send_info->buf */

		if (!mux_tty[j]) {
			continue;
		}

		tty = mux_table[j];
		if (!tty) {
			continue;
		}

		dlci = j;
		if (ts0710->dlci[dlci].state == FLOW_STOPPED) {
			TS0710_INFO("Flow stopped on channel dlc %d\n", dlci);
			continue;
		} else if (ts0710->dlci[dlci].state != CONNECTED) {
			TS0710_INFO("dlc %d not connected\n", dlci);
			continue;
		}

#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,27))
		if ((tty->flags & (1 << TTY_DO_WRITE_WAKEUP))
		    && tty->ldisc.write_wakeup) {
			(tty->ldisc.write_wakeup) (tty);
		}
#elsif (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,31))
		if ((tty->flags & (1 << TTY_DO_WRITE_WAKEUP))
		    && tty->ldisc.ops->write_wakeup) {
			(tty->ldisc.ops->write_wakeup) (tty);
		}
#else
		if ((tty->flags & (1 << TTY_DO_WRITE_WAKEUP))
		    && tty->ldisc->ops->write_wakeup) {
			(tty->ldisc->ops->write_wakeup) (tty);
		}
#endif
		wake_up_interruptible(&tty->write_wait);

#ifdef SERIAL_HAVE_POLL_WAIT
		wake_up_interruptible(&tty->poll_wait);
#endif

		if (send_info->filled) {
			if (j < mux_send_info_idx) {
				mux_send_info_idx = j;
			}
		}
	} /* End for() loop */
	TS0710_DEBUG("<<\n");
}

/******************************************************************************/
/* line discipline */

static void append_to_inbuf_list(const unsigned char *buf, int count)
{
	buf_list_t *inbuf;
	
	inbuf = kmalloc(sizeof(buf_list_t), GFP_KERNEL);
	if (!inbuf) {
		TS0710_ERROR("(%d) out of memory!\n",sizeof(buf_list_t));
		return;
	}
	
	inbuf->size = count;

	inbuf->body = kmalloc(sizeof(char)*count, GFP_KERNEL);
	if (!inbuf->body) {
		kfree(inbuf);
		TS0710_ERROR("(%d) out of memory!\n",sizeof(unsigned char)*count);
		return;
	}
	memcpy(inbuf->body, buf, count);
	spin_lock(&ldisc_data_ptr->in_buf_lock);
    ldisc_data_ptr->in_buf_size += count;
	list_add_tail(&inbuf->list, &ldisc_data_ptr->in_buf_list);
	spin_unlock(&ldisc_data_ptr->in_buf_lock);
}

static int get_from_inbuf_list(unsigned char *buf, int dst_count)
{
	int ret = 0;
	spin_lock(&ldisc_data_ptr->in_buf_lock);
	if (!(list_empty(&ldisc_data_ptr->in_buf_list))) { 
		int src_count;
		buf_list_t *inbuf;
		struct list_head *ptr;

		ptr = ldisc_data_ptr->in_buf_list.next;
		inbuf = list_entry(ptr, buf_list_t, list);
		src_count = inbuf->size;
		if (dst_count >= src_count) {
			memcpy(buf, inbuf->body, src_count);
			ret = src_count;
            ldisc_data_ptr->in_buf_size -= src_count;
			list_del(ptr);
			kfree(inbuf->body);
			kfree(inbuf);
		} else {
	        TS0710_ERROR("** Fail to transfer %d to dest buf %d **\n",
				src_count,dst_count);
		}
	}
	spin_unlock(&ldisc_data_ptr->in_buf_lock);

	return ret;
}

static int mux_ldisc_write(const unsigned char *buf, int count)
{	
#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,25))
	int ret = COMM_FOR_MUX_TTY->driver->write(COMM_FOR_MUX_TTY, buf, count);
#else
	int ret = COMM_FOR_MUX_TTY->driver->ops->write(COMM_FOR_MUX_TTY, buf, count);
#endif
	mux_sender();

	return ret;
}

static int mux_ldisc_chars_in_buffer(struct tty_struct *tty)
{		
	return 0;
/* TODO this blocks - come back to
#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,25))
	return tty->driver->chars_in_buffer(tty);
#else
	return tty->driver->ops->chars_in_buffer(tty);
#endif
*/
}

static int
link_ldisc_open(struct tty_struct *tty)
{
	if (!tty)
		return -EINVAL;

	TS0710_INFO("link_ldisc_open\n");
	/* TODO - force connection of the mux - assumption that this is called after
	 * cmux issued */

	COMM_FOR_MUX_TTY = tty;
	tty->disc_data = NULL;
	tty->receive_room = 65536;

	return 0;
}

static void
link_ldisc_close(struct tty_struct *tty)
{
	TS0710_INFO("link_ldisc_close\n\n");

	/* TODO - force disconnection of the mux */
}

static int link_ldisc_hangup(struct tty_struct *tty)
{
	TS0710_INFO("link_ldisc_hangup\n");
	link_ldisc_close(tty);
	return 0;
}

static ssize_t
link_ldisc_read(struct tty_struct *tty, struct file *file,
		  unsigned char __user *buf, size_t count)
{
	/* TODO - we could give back diagnostic info in this read since /dev/muxX
	 * used for data reads */
	TS0710_INFO("link_ldisc_read\n");
	return -EAGAIN;
}

static ssize_t
link_ldisc_write(struct tty_struct *tty, struct file *file,
		   const unsigned char *buf, size_t count)
{
	/* TODO - write currently not used - /dev/muxX used for data writes */
	TS0710_INFO("link_ldisc_write - do nothing with <%s>\n",buf);
	return 0;
}

static int
link_ldisc_ioctl(struct tty_struct *tty, struct file *file,
		   unsigned int cmd, unsigned long arg)
{
	TS0710_INFO("link_ldisc_ioctl\n");
	/* TODO - any ioctls need processing/ */
	return 0;
}

static unsigned int
link_ldisc_poll(struct tty_struct *tty, struct file *file, poll_table *wait)
{
	TS0710_INFO("link_ldisc_poll\n");
	/* TODO - see link_ldisc_read comment above - only poll for diagnostic info */
	return 0;
}

static void
link_ldisc_receive(struct tty_struct *tty, const unsigned char *buf,
		  char *cflags, int count)
{
	TS0710_DEBUG(" count=%d\n",count);

	/* Add to inbuf so can later be retrieved by get_from_inbuf_list */
	append_to_inbuf_list(buf,count);

	/* schedule the receive work queue */
	mux_dispatcher(tty);
}

static void
link_ldisc_wakeup(struct tty_struct *tty)
{
	TS0710_INFO("link_ldisc_wakeup\n");
	/* TODO - any use for wakeup? */
}


#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,27))
static struct tty_ldisc tty_ldisc_info = {
	.owner  	= THIS_MODULE,
	.magic		= TTY_LDISC_MAGIC,
	.name		= "link_ldisc",
	.open		= link_ldisc_open,
	.close		= link_ldisc_close,
	.hangup		= link_ldisc_hangup,
	.read	    = link_ldisc_read,
	.write		= link_ldisc_write,
	.ioctl	    = link_ldisc_ioctl,
	.poll	    = link_ldisc_poll,
	.receive_buf 	= link_ldisc_receive,
	.write_wakeup 	= link_ldisc_wakeup,
};
#else
static struct tty_ldisc_ops tty_ldisc_ops_info = {
	.owner  	= THIS_MODULE,
	.magic		= TTY_LDISC_MAGIC,
	.name		= "link_ldisc",
	.open		= link_ldisc_open,
	.close		= link_ldisc_close,
	.hangup		= link_ldisc_hangup,
	.read	    = link_ldisc_read,
	.write		= link_ldisc_write,
	.ioctl	    = link_ldisc_ioctl,
	.poll	    = link_ldisc_poll,
	.receive_buf 	= link_ldisc_receive,
	.write_wakeup 	= link_ldisc_wakeup,
};
#endif

static int link_ldisc_init(void)
{
	int err;
	TS0710_INFO("link_ldisc_init\n");

	if (!(ldisc_data_ptr = kzalloc(sizeof(struct link_ldisc_data), GFP_KERNEL))) {
		TS0710_ERROR("link_ldisc_init: Out of memory.\n");		
		return -ENOMEM;
	}
	INIT_LIST_HEAD(&ldisc_data_ptr->in_buf_list);
#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,39))
	ldisc_data_ptr->in_buf_lock = SPIN_LOCK_UNLOCKED;
#else
	ldisc_data_ptr->in_buf_lock =
		__SPIN_LOCK_UNLOCKED(ldisc_data_ptr->in_buf_lock);
#endif

	/* Reuse the N_MOUSE ldisc for now, TODO - need own line discipline ident */
#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,27))
	err = tty_register_ldisc(N_MOUSE, &tty_ldisc_info);
#else
	err = tty_register_ldisc(N_MOUSE, &tty_ldisc_ops_info);
#endif
	if (err != 0) {
		printk(KERN_ERR "link_ldisc: error %d registering line disc.\n",err);
	}

	return err;
}

static void link_ldisc_cleanup(void)
{
	TS0710_INFO("link_ldisc_cleanup\n");

	if (tty_unregister_ldisc(N_MOUSE) != 0) {
		printk(KERN_ERR "failed to unregister line discipline\n");
	}
	kfree(ldisc_data_ptr);
}

/******************************************************************************/
/* Module init & exit */

static struct tty_operations serial_ops = {
	.open = mux_open,
	.close = mux_close,
	.write = mux_write,
	.write_room = mux_write_room,
	.flush_buffer = mux_flush_buffer,
	.chars_in_buffer = mux_chars_in_buffer,
	.throttle = mux_throttle,
	.unthrottle = mux_unthrottle,
	.ioctl = mux_ioctl,
};

static int __init mux_init(void)
{
	__u8 j;

	TS0710_INFO("mux_init\n");
	TS0710_INFO("The process is \"%s\" (pid %i)\n", current->comm, current->pid);

	/* set up the line discipline first */
	if (link_ldisc_init()) {
		panic("Failed to initialise the line discipline\n");
	}

	ts0710_init();

	for (j = 0; j < NR_MUXS; j++) {
		mux_send_info_flags[j] = 0;
		mux_send_info[j] = 0;
		mux_recv_info_flags[j] = 0;
		mux_recv_info[j] = 0;
	}
	mux_send_info_idx = NR_MUXS;
	mux_recv_queue = NULL;
	mux_recv_flags = 0;

#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,20))
	INIT_WORK(&send_tqueue, send_worker, NULL);
	INIT_WORK(&receive_tqueue, receive_worker, NULL);
	INIT_WORK(&post_recv_tqueue, post_recv_worker, NULL);
#else
	INIT_WORK(&send_tqueue, send_worker);
	INIT_WORK(&receive_tqueue, receive_worker);
	INIT_WORK(&post_recv_tqueue, post_recv_worker);
#endif

	mux_driver = alloc_tty_driver(NR_MUXS);
	if (!mux_driver)
		return -ENOMEM;

	mux_driver->owner 	= THIS_MODULE;
	mux_driver->driver_name = "mux";
	mux_driver->name 	= "mux";
	mux_driver->major 	= TS0710MUX_MAJOR;
	mux_driver->minor_start = TS0710MUX_MINOR_START;
	mux_driver->type 	= TTY_DRIVER_TYPE_SERIAL;
	mux_driver->subtype = SERIAL_TYPE_NORMAL;
	mux_driver->flags 	= TTY_DRIVER_RESET_TERMIOS | TTY_DRIVER_REAL_RAW |
		TTY_DRIVER_DYNAMIC_DEV;

	mux_driver->init_termios = tty_std_termios;
	mux_driver->init_termios.c_iflag = 0;
	mux_driver->init_termios.c_oflag = 0;
	mux_driver->init_termios.c_cflag = B38400 | CS8 | CREAD;
	mux_driver->init_termios.c_lflag = 0;

	mux_driver->termios = mux_termios;
	mux_driver->termios_locked = mux_termios_locked;
	mux_driver->other = NULL;

	tty_set_operations(mux_driver, &serial_ops);

	if (tty_register_driver(mux_driver))
		panic("Couldn't register mux driver");

	for (j=TS0710MUX_MINOR_START; j<NR_MUXS; j++)
		tty_register_device(mux_driver, j, NULL);

	return 0;
}

static void __exit mux_exit(void)
{
	__u8 j;

	/* TODO clean up work queues
	cancel_delayed_work(&send_tqueue);
	cancel_delayed_work(&receive_tqueue);
	cancel_delayed_work(&post_recv_tqueue);
	flush_workqueue(&send_tqueue);
	flush_workqueue(&receive_tqueue);
	flush_workqueue(&post_recv_tqueue); */

	mux_send_info_idx = NR_MUXS;
	mux_recv_queue = NULL;
	for (j = 0; j < NR_MUXS; j++) {
		if ((mux_send_info_flags[j]) && (mux_send_info[j])) {
			kfree(mux_send_info[j]);
		}
		mux_send_info_flags[j] = 0;
		mux_send_info[j] = 0;

		if ((mux_recv_info_flags[j]) && (mux_recv_info[j])) {
			free_mux_recv_struct(mux_recv_info[j]);
		}
		mux_recv_info_flags[j] = 0;
		mux_recv_info[j] = 0;
	}

	for (j=TS0710MUX_MINOR_START; j<NR_MUXS; j++)
		tty_unregister_device(mux_driver, j);

	if (tty_unregister_driver(mux_driver))
		panic("Couldn't unregister mux driver");

	/* remove the line discipline last */
	link_ldisc_cleanup();
}

module_init(mux_init);
module_exit(mux_exit);

/******************************************************************************/

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jim Rayner <jimr@beyondvoice.com>");
MODULE_DESCRIPTION("27.010 multiplexer");
