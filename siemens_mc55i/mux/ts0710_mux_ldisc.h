/*
 * ts0710_mux_ldisc.h
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * Derived from files ..
 *
 * File: mux_macro.h
 * Copyright (C) 2002 2005 Motorola
 *
 * File: ts0710.h
 * Portions derived from rfcomm.c, original header as follows:
 * Copyright (C) 2000, 2001  Axis Communications AB
 * Author: Mats Friden <mats.friden@axis.com>
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

#include <linux/version.h>

#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,19))
#include <linux/config.h>
#endif

#include <linux/module.h>

#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/interrupt.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/fcntl.h>
#include <linux/string.h>
#include <linux/major.h>
#include <linux/mm.h>
#include <linux/init.h>

#include <asm/uaccess.h>
#include <asm/system.h>
#include <asm/bitops.h>

#include <asm/byteorder.h>
#include <asm/types.h>

#define TS0710_MAX_CHN 4 /* Siemens */


#ifdef __LITTLE_ENDIAN_BITFIELD

typedef struct {
	__u8 ea:1;
	__u8 cr:1;
	__u8 server_chn:6;
} __attribute__ ((packed)) address_field;

typedef struct {
	__u8 ea:1;
	__u8 len:7;
} __attribute__ ((packed)) short_length;

typedef struct {
	__u8 ea:1;
	__u8 l_len:7;
	__u8 h_len;
} __attribute__ ((packed)) long_length;

typedef struct {
	address_field addr;
	__u8 control;
	short_length length;
} __attribute__ ((packed)) short_frame_head;

typedef struct {
	short_frame_head h;
	__u8 data[0];
} __attribute__ ((packed)) short_frame;

typedef struct {
	address_field addr;
	__u8 control;
	long_length length;
	__u8 data[0];
} __attribute__ ((packed)) long_frame_head;

typedef struct {
	long_frame_head h;
	__u8 data[0];
} __attribute__ ((packed)) long_frame;

/* Type definitions for structures used for the multiplexer commands */
typedef struct {
	__u8 ea:1;
	__u8 cr:1;
	__u8 type:6;
} __attribute__ ((packed)) mcc_type;

typedef struct {
	mcc_type type;
	short_length length;
	__u8 value[0];
} __attribute__ ((packed)) mcc_short_frame_head;

typedef struct {
	mcc_short_frame_head h;
	__u8 value[0];
} __attribute__ ((packed)) mcc_short_frame;

typedef struct {
	mcc_type type;
	long_length length;
	__u8 value[0];
} __attribute__ ((packed)) mcc_long_frame_head;

typedef struct {
	mcc_long_frame_head h;
	__u8 value[0];
} __attribute__ ((packed)) mcc_long_frame;

/* MSC-command */
typedef struct {
	__u8 ea:1;
	__u8 fc:1;
	__u8 rtc:1;
	__u8 rtr:1;
	__u8 reserved:2;
	__u8 ic:1;
	__u8 dv:1;
} __attribute__ ((packed)) v24_sigs;

typedef struct {
	__u8 ea:1;
	__u8 b1:1;
	__u8 b2:1;
	__u8 b3:1;
	__u8 len:4;
} __attribute__ ((packed)) brk_sigs;

typedef struct {
	short_frame_head s_head;
	mcc_short_frame_head mcc_s_head;
	address_field dlci;
	__u8 v24_sigs;
	//brk_sigs break_signals;
	__u8 fcs;
} __attribute__ ((packed)) msc_msg;

/* PN-command */
typedef struct {
	short_frame_head s_head;
	mcc_short_frame_head mcc_s_head;
	__u8 dlci:6;
	__u8 res1:2;
	__u8 frame_type:4;
	__u8 credit_flow:4;
	__u8 prior:6;
	__u8 res2:2;
	__u8 ack_timer;
	__u8 frame_sizel;
	__u8 frame_sizeh;
	__u8 max_nbrof_retrans;
	__u8 credits;
	__u8 fcs;
} __attribute__ ((packed)) pn_msg;

/* NSC-command */
typedef struct {
	short_frame_head s_head;
	mcc_short_frame_head mcc_s_head;
	mcc_type command_type;
	__u8 fcs;
} __attribute__ ((packed)) nsc_msg;

/* CLD-command */
typedef struct {
	short_frame_head s_head;
	mcc_short_frame_head mcc_s_head;
	__u8 fcs;
} __attribute__ ((packed)) cld_msg;

#else
#error Only little-endianess supported now!
#endif

enum {
	REJECTED = 0,
	DISCONNECTED,
	CONNECTING,
	NEGOTIATING,
	CONNECTED,
	DISCONNECTING,
	FLOW_STOPPED
};

enum ts0710_events {
	CONNECT_IND,
	CONNECT_CFM,
	DISCONN_CFM
};

typedef struct {
	volatile __u8 state;
	volatile __u8 flow_control;
	volatile __u8 initiated;
	volatile __u8 initiator;
	volatile __u16 mtu;
	wait_queue_head_t open_wait;
	wait_queue_head_t close_wait;
} dlci_struct;

/* user space interfaces */
typedef struct {
	volatile __u8 initiator;
	volatile __u8 c_dlci;
	volatile __u16 mtu;
	volatile __u8 be_testing;
	volatile __u32 test_errs;
	wait_queue_head_t test_wait;

	dlci_struct dlci[TS0710_MAX_CHN];
} ts0710_con;

/* Special ioctl() upon a MUX device file for hanging up a call */
#define TS0710MUX_IO_MSC_HANGUP 0x54F0

/* Special ioctl() upon a MUX device file for MUX loopback test */
#define TS0710MUX_IO_TEST_CMD 0x54F1

/* Special Error code might be return from write() to a MUX device file  */
#define EDISCONNECTED 900	/* Logical data link is disconnected */

/* Special Error code might be return from open() to a MUX device file  */
#define EREJECTED 901		/* Logical data link connection request is rejected */
