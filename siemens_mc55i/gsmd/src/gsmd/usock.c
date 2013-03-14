/* gsmd unix domain socket handling
 *
 * (C) 2006-2007 by OpenMoko, Inc.
 * Written by Harald Welte <laforge@openmoko.org>
 * All Rights Reserved
 *
 * Copyright (C) 2007-2009 Jim Rayner <jimr@beyondvoice.com>
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
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 */

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>

#include <sys/socket.h>
#include <sys/un.h>

#include "gsmd.h"

#include <gsmd/gsmd.h>
#include <gsmd/usock.h>
#include <gsmd/select.h>
#include <gsmd/atcmd.h>
#include <gsmd/usock.h>
#include <gsmd/talloc.h>
#include <gsmd/ts0707.h>
#include <gsmd/sms.h>
#include <gsmd/unsolicited.h>

#define MAX_SIM_BUSY_RETRIES 10
#define SIM_BUSY_RETRY_DELAY 4

static void *__ucmd_ctx, *__gu_ctx, *__pb_r_ctx, *__pb_f_ctx;

struct gsmd_ucmd *ucmd_alloc(int extra_size)
{
	struct gsmd_ucmd *ucmd =
		talloc_size(__ucmd_ctx,
			   sizeof(struct gsmd_ucmd) + extra_size);
	if (ucmd) {
		ucmd->hdr.version = GSMD_PROTO_VERSION;
		ucmd->hdr.len = extra_size;
	}
	return ucmd;
}

static struct gsmd_ucmd *ucmd_header(struct gsmd *g)
{
	struct gsmd_ucmd *ucmd_hdr = NULL;
	if (llist_empty(&g->free_ucmd_hdr)) {
		gsmd_log(GSMD_NOTICE, "no free ucmd hdr\n");
		g->num_free_ucmd_hdrs = 0;
		ucmd_hdr = ucmd_alloc(0);
	} else {
		ucmd_hdr = llist_entry(g->free_ucmd_hdr.next,
				  struct gsmd_ucmd, list);
		llist_del(&ucmd_hdr->list);
		g->num_free_ucmd_hdrs--;
	}
	DEBUGP("num_free_ucmd_hdrs %d\n",g->num_free_ucmd_hdrs);

	return ucmd_hdr;
}

int valid_ucmd(struct gsmd_ucmd *ucmd)
{
	return (ucmd && ucmd->hdr.len);
}

static void check_free_list(struct gsmd *g)
{
	int loop;
	for (loop = g->num_free_ucmd_hdrs; loop < g->max_free_ucmd_hdrs; loop++) {
		struct gsmd_ucmd* ucmd_hdr = ucmd_alloc(0);
		if (!ucmd_hdr)
			break;
		llist_add(&ucmd_hdr->list, &g->free_ucmd_hdr);
		g->num_free_ucmd_hdrs++;
	}
}

int usock_cmd_enqueue(struct gsmd_ucmd *ucmd, struct gsmd_user *gu)
{
	if (!ucmd)
		return -ENOMEM;

	DEBUGP("enqueueing usock cmd %p for user %p (data len %d)\n", ucmd, gu, ucmd->hdr.len);

	/* add to per-user list of finished cmds */
	llist_add_tail(&ucmd->list, &gu->finished_ucmds);

	/* mark socket of user as we-want-to-write */
	gu->gfd.when |= GSMD_FD_WRITE;

	return 0;
}

/* callback for completed passthrough gsmd_atcmd's */
static int usock_passthrough_cb(struct gsmd_atcmd *cmd, void *ctx, char *resp)
{
	struct gsmd_user *gu = ctx;
	int rlen = strlen(resp)+1;
	struct gsmd_ucmd *ucmd =
		gsmd_ucmd_fill(gu, rlen, GSMD_MSG_PASSTHROUGH, GSMD_PASSTHROUGH_RESP, cmd);

	DEBUGP("entering(cmd=%p, gu=%p)\n", cmd, gu);

	if (valid_ucmd(ucmd)) {
		memcpy(ucmd->buf, resp, rlen);
	}

	return usock_cmd_enqueue(ucmd, gu);
}

typedef int usock_msg_handler(struct gsmd_user *gu, struct gsmd_msg_hdr *gph, int len);

static int usock_rcv_passthrough(struct gsmd_user *gu, struct gsmd_msg_hdr *gph, int len)
{
	struct gsmd_atcmd *cmd;
	cmd = atcmd_fill((char *)gph+sizeof(*gph), gph->len, &usock_passthrough_cb, gu, gph);
	if (!cmd)
		return -ENOMEM;

	DEBUGP("submitting cmd=%p, gu=%p\n", cmd, gu);

	return atcmd_submit(gu->gsmd, cmd, GSMD_CMD_CHANNEL0);
}

static int usock_rcv_event(struct gsmd_user *gu, struct gsmd_msg_hdr *gph, int len)
{
	u_int32_t *evtmask = (u_int32_t *) ((char *)gph + sizeof(*gph));

	if (len < sizeof(*gph) + sizeof(u_int32_t))
		return -EINVAL;

	if (gph->msg_subtype != GSMD_EVT_SUBSCRIPTIONS)
		return -EINVAL;

	gu->subscriptions = *evtmask;
}

static int simple_cmd_cb(struct gsmd_atcmd *cmd, void *ctx, char *resp)
{
	struct gsmd_user *gu = ctx;
	struct gsmd_ucmd *ucmd;

	DEBUGP("resp: %s\n", resp);

	int type = 0;
	int subtype = 0;
	if (cmd->gph) {
		type = cmd->gph->msg_type;
		subtype = cmd->gph->msg_subtype;
	}
	ucmd = gsmd_ucmd_fill(gu, 0, type, subtype, cmd);

	return usock_cmd_enqueue(ucmd, gu);
}


static int usock_rcv_voicecall(struct gsmd_user *gu, struct gsmd_msg_hdr *gph,
				int len)
{
	struct gsmd_atcmd *cmd = NULL;
	struct gsmd_addr *ga;
	struct gsmd_dtmf *gd;
	int atcmd_len;

	switch (gph->msg_subtype) {
	case GSMD_VOICECALL_DIAL:
		if (len < sizeof(*gph) + sizeof(*ga))
			return -EINVAL;
		ga = (struct gsmd_addr *) ((void *)gph + sizeof(*gph));
		ga->number[GSMD_ADDR_MAXLEN] = '\0';
		cmd = atcmd_fill("ATD", 7 + strlen(ga->number),
				 &simple_cmd_cb, gu, gph);
		if (!cmd)
			return -ENOMEM;
		sprintf(cmd->buf, "ATD\"%s\";", ga->number);
		cmd->timeout_value = 60;
		break;
	case GSMD_VOICECALL_HANGUP:
		/* ATH0 is not supported by QC, we hope ATH is supported by everyone */
		cmd = atcmd_fill("ATH", 4, &simple_cmd_cb, gu, gph);
		if (gu->gsmd->number_channels > GSMD_ATH_CMD_CHANNEL) {
			/* Hangup should not be delayed, so use special channel if multiplexed */
			return atcmd_submit(gu->gsmd, cmd, GSMD_ATH_CMD_CHANNEL);
		} else {
			/* This command is special because it needs to be sent to
			* the MS even if a command is currently executing.  */
			if (cmd) {
				return cancel_current_atcmd(gu->gsmd, cmd, GSMD_CMD_CHANNEL0);
			}
		}
		break;
	case GSMD_VOICECALL_ANSWER:
		cmd = atcmd_fill("ATA", 4, &simple_cmd_cb, gu, gph);
		break;
	case GSMD_VOICECALL_DTMF:
		if (len < sizeof(*gph) + sizeof(*gd))
			return -EINVAL;

		gd = (struct gsmd_dtmf *) ((void *)gph + sizeof(*gph));
		if (len < sizeof(*gph) + sizeof(*gd) + gd->len)
			return -EINVAL;

		/* FIXME: we don't yet support DTMF of multiple digits */
		if (gd->len != 1)
			return -EINVAL;

		atcmd_len = 1 + strlen("AT+VTS=") + (gd->len * 2);
		cmd = atcmd_fill("AT+VTS=", atcmd_len, &simple_cmd_cb, gu, gph);
		if (!cmd)
			return -ENOMEM;

		sprintf(cmd->buf, "AT+VTS=%c;", gd->dtmf[0]);
		break;
	default:
		return -EINVAL;
	}

	if (cmd)
		return atcmd_submit(gu->gsmd, cmd, GSMD_CMD_CHANNEL0);
	else
		return -ENOMEM;
}

static int usock_rcv_ussd(struct gsmd_user *gu, struct gsmd_msg_hdr *gph,
				int len)
{
	struct gsmd_atcmd *cmd = NULL;

	switch (gph->msg_subtype) {
	case GSMD_USSD_SEND:
		{
		char* ussd_data = NULL;
		if (len < sizeof(*gph))
			return -EINVAL;
		ussd_data = (char *) ((void *)gph + sizeof(*gph));
		cmd = atcmd_fill("AT+CUSD=1,", 11 + strlen(ussd_data),
				 &simple_cmd_cb, gu, gph);
		if (!cmd)
			return -ENOMEM;
		sprintf(cmd->buf, "AT+CUSD=1,%s", ussd_data);
		/* USSD requests can take a while when roaming */
		cmd->timeout_value = 60;
		}
		break;
	case GSMD_USSD_RSP:
		{
		char* ussd_rsp = NULL;
		int ussd_rsp_len = 2;
		if (len < sizeof(*gph))
			return -EINVAL;
		if (len > sizeof(*gph)) {
			ussd_rsp = (char *) ((void *)gph + sizeof(*gph));
			ussd_rsp_len += strlen(ussd_rsp);
		}
		cmd = atcmd_fill("", ussd_rsp_len, &simple_cmd_cb, gu, gph);
		if (!cmd)
			return -ENOMEM;
		if (ussd_rsp)
			sprintf(cmd->buf, "%s%c", ussd_rsp,26); /* ^Z ends the response */
		else
			sprintf(cmd->buf, "%c", 27); /* ESC sends cancel to the network */
		/* USSD responses can take a while when roaming */
		cmd->timeout_value = 60;
		}
		break;
	default:
		return -EINVAL;
	}

	if (cmd)
		return atcmd_submit(gu->gsmd, cmd, GSMD_CMD_CHANNEL0);
	else
		return -ENOMEM;
}

static int null_cmd_cb(struct gsmd_atcmd *cmd, void *ctx, char *resp)
{
	gsmd_log(GSMD_DEBUG, "null cmd cb\n");
	return 0;
}

/* PIN command callback. Gets called for response to AT+CPIN cmcd */
static int pin_cmd_cb(struct gsmd_atcmd *cmd, void *ctx, char *resp)
{
	struct gsmd_user *gu = ctx;
	struct gsmd_ucmd *ucmd;
	int type = -1;
	int retval = 0;

	gsmd_log(GSMD_DEBUG, "pin cmd cb (%d)\n",cmd->ret);

	if (cmd->ret) {
		unsigned int val = -cmd->ret;
		type = map_cme_error_to_pin_type(val);
		if (type) {
			gsmd_log(GSMD_DEBUG, "Incorrect pin/puk\n");
			/* reset ret since carry the detail of the problem in the ucmd */
			cmd->ret = 0;
		} else {
			gsmd_log(GSMD_DEBUG, "Pin cmd failure %d\n",cmd->ret);
		}
	} else {
		gsmd_log(GSMD_DEBUG, "Success\n");
		type = GSMD_PIN_NONE;
	}

	ucmd = gsmd_ucmd_fill(gu, sizeof(int), GSMD_MSG_PIN, GSMD_PIN_INPUT, cmd);

	if (valid_ucmd(ucmd)) {
		*(int *) ucmd->buf = type;
	}
	usock_cmd_enqueue(ucmd, gu);

	/* send out success event after pin cnf */
	if (GSMD_PIN_NONE == type && !cmd->ret)
	{
		/* inform users that pin/puk entry has been successful */
		retval = pin_status_changed(gu->gsmd, 0);
	}

	return retval;
}

static char* pincmds[2] = { "AT+CPIN=\"", "AT+CPIN2=\"" };

static int usock_rcv_pin(struct gsmd_user *gu, struct gsmd_msg_hdr *gph,
			 int len)
{
	struct gsmd_pin *gp = (struct gsmd_pin *) ((void *)gph + sizeof(*gph));
	struct gsmd_atcmd *cmd;
	int retval = 0;

	if (gph->len < sizeof(*gp) || len < sizeof(*gp)+sizeof(*gph))
		return -EINVAL;

	switch (gph->msg_subtype) {
	case GSMD_CHG_LOCK:
		{
		gsmd_log(GSMD_DEBUG, "chg lock %d pin='%s'\n",
			gp->lock, gp->pin);

		cmd = atcmd_fill("", 13+GSMD_PIN_MAXLEN+2,
				&simple_cmd_cb, gu, gph);
		if (!cmd)
			return -ENOMEM;
	
		sprintf(cmd->buf, "AT+CLCK=\"SC\",%d,\"%s\"",gp->lock,gp->pin);
		cmd->buflen = strlen(cmd->buf) + 1;
	
		retval = atcmd_submit(gu->gsmd, cmd, GSMD_CMD_CHANNEL0);
		}
		break;
	case GSMD_CHG_PIN:
		{
		gsmd_log(GSMD_DEBUG, "chg pin pin='%s' newpin='%s'\n",
			gp->pin, gp->newpin);

		cmd = atcmd_fill("", 14+GSMD_PIN_MAXLEN+3+GSMD_PIN_MAXLEN+2,
				&simple_cmd_cb, gu, gph);
		if (!cmd)
			return -ENOMEM;
	
		sprintf(cmd->buf, "AT+CPWD=\"SC\",\"%s\",\"%s\"",gp->pin,gp->newpin);
		cmd->buflen = strlen(cmd->buf) + 1;
	
		retval = atcmd_submit(gu->gsmd, cmd, GSMD_CMD_CHANNEL0);
		}
		break;
	case GSMD_PIN_INPUT:
	case GSMD_PIN2_INPUT:
		{
		char* txt_ptr = NULL;
		gsmd_log(GSMD_DEBUG, "pin type=%u pin='%s' newpin='%s'\n",
			gp->type, gp->pin, gp->newpin);

		switch (gph->msg_subtype) {
		case GSMD_PIN_INPUT:
			txt_ptr = pincmds[0];
			break;
		case GSMD_PIN2_INPUT:
			txt_ptr = pincmds[1];
			break;
		default:
			gsmd_log(GSMD_ERROR, "unknown pin type %u\n",
				gph->msg_subtype);
			return -EINVAL;
		}
	
		/* create cmd buffer (max size) */
		cmd = atcmd_fill(txt_ptr, 9+GSMD_PIN_MAXLEN+3+GSMD_PIN_MAXLEN+2,
				&pin_cmd_cb, gu, gph);
		if (!cmd)
			return -ENOMEM;
	
		strcat(cmd->buf, gp->pin);
	
		switch (gp->type) {
		case GSMD_PIN_SIM_PUK:
		case GSMD_PIN_SIM_PUK2:
			strcat(cmd->buf, "\",\"");
			strcat(cmd->buf, gp->newpin);
			break;
		default:
			break;
		}
	
		strcat(cmd->buf, "\"");
	
		/* set buflen according to final cmd size */
		cmd->buflen = strlen(cmd->buf) + 1;
	
		retval = atcmd_submit_highpriority(gu->gsmd, cmd, GSMD_CMD_CHANNEL0);
		}
		break;
	}

	return retval;
}

static int phone_shutdown_cb(struct gsmd_atcmd *cmd, void *ctx, char *resp)
{
	struct gsmd* gsmd = ctx;
	if (cmd->ret) {
		gsmd_log(GSMD_DEBUG, "Power off failed\n");
		// Assumption power will be removed anyway
	}
	gsmd_log(GSMD_DEBUG, "Modem powered-off. Shutdown gsmd\n");
	gsmd->running = 0;
	return 0;
}

int shutdown_modem(struct gsmd* gsmd, struct gsmd_msg_hdr *gph)
{
	int retval = 0;
	struct gsmd_timer *timer;
	struct gsmd_atcmd *cmd = NULL;
	if (STATUS_OK == gsmd->modem_status) {
		gsmd_log(GSMD_DEBUG, "Shutting down the modem\n");
		cancel_all_atcmds(gsmd);
		if (gsmd->number_channels > 1) {
			retval = atcmd_revert_to_single_port_mode(gsmd);
		}
		if (!retval) {
			cmd = atcmd_fill("AT^SMSO", 7+1, &phone_shutdown_cb, gsmd, gph);
			if (cmd) {
				retval = atcmd_submit_highpriority(gsmd, cmd, GSMD_CMD_CHANNEL0);
			}
		}
	}
	if (!cmd || retval) {
		gsmd_log(GSMD_DEBUG, "Doing immediate shutdown\n");
		gsmd->running = 0;
	}
	return retval;
}

static int phone_power_cb(struct gsmd_atcmd *cmd, void *ctx, char *resp)
{
	struct gsmd_user *gu = ctx;
	struct gsmd_ucmd *ucmd;
	int subtype = 0;

	if (cmd->gph) {
		subtype = cmd->gph->msg_subtype;
	}

	if (cmd->ret) {
		gsmd_log(GSMD_DEBUG, "Radio power change failed\n");
	} else {
		switch(subtype) {
		case GSMD_PHONE_RESUME:
			gsmd_eat_garbage();
			/* deliberate fall through */
		case GSMD_PHONE_POWERUP:
			gsmd_log(GSMD_DEBUG, "Modem powered-on\n");
			gu->gsmd->dev_state.on = 1;
			break;
		case GSMD_PHONE_SUSPEND:
		case GSMD_PHONE_POWERDOWN:
			gsmd_log(GSMD_DEBUG, "Modem powered-off\n");
			gu->gsmd->dev_state.on = 0;
			break;
		case GSMD_PHONE_POWERSAVE:
			gsmd_log(GSMD_DEBUG, "Modem power save\n");
			gu->gsmd->dev_state.on = 1;
		   	// TODO Need to switch on serial hardware flow control
			break;
		case GSMD_PHONE_FLIGHTMODE:
			gsmd_log(GSMD_DEBUG, "Flight mode. Radio powered-down\n");
			gu->gsmd->dev_state.on = 0;
			break;
		case GSMD_PHONE_AV_CURRENT:
			gsmd_log(GSMD_DEBUG, "Av Current mA\n");
			break;
		case GSMD_PHONE_VOLTAGE:
			gsmd_log(GSMD_DEBUG, "Voltage mV\n");
			break;
		}
	}

	ucmd = gsmd_ucmd_fill(gu, 0, GSMD_MSG_PHONE, subtype, cmd);

	return usock_cmd_enqueue(ucmd, gu);
}

static int phone_va_cb(struct gsmd_atcmd *cmd, void *ctx, char *resp)
{
	struct gsmd_user *gu = ctx;
	struct gsmd_ucmd *ucmd;
	int subtype = 0;
	int val = 0;
	int bcs = 0;
	int bcl = 0;

	if (cmd->gph) {
		subtype = cmd->gph->msg_subtype;
	}

	if (cmd->ret) {
		gsmd_log(GSMD_DEBUG, "VA lookup failed\n");
	} else {
		switch(subtype) {
		case GSMD_PHONE_AV_CURRENT: {
				if (sscanf(resp, "^SBC: %i,%i,%i", &bcs, &bcl, &val) < 3) {
					gsmd_log(GSMD_NOTICE, "Unknown format <%s>\n",resp);
					cmd->ret = -EINVAL;
				}
				gsmd_log(GSMD_DEBUG, "Battery conn status %d capacity %d Av Current %d mA\n",bcs,bcl,val);
			}
			break;
		case GSMD_PHONE_VOLTAGE:
			if (sscanf(resp, "^SBV: %i", &val) < 1) {
				gsmd_log(GSMD_NOTICE, "Unknown format <%s>\n",resp);
				cmd->ret = -EINVAL;
			}
			gsmd_log(GSMD_DEBUG, "Voltage %d mV\n",val);
			break;
		}
	}

	ucmd = gsmd_ucmd_fill(gu, sizeof(int) * 3, GSMD_MSG_PHONE, subtype, cmd);
	if (valid_ucmd(ucmd)) {
		int* ptr = (int*) ucmd->buf;
		*ptr = val;
		ptr++;
		*ptr = bcs;
		ptr++;
		*ptr = bcl;
	}

	return usock_cmd_enqueue(ucmd, gu);
}

static int get_inf_cb(struct gsmd_atcmd *cmd, void *ctx, char *resp)
{
	struct gsmd_user *gu = ctx;
	struct gsmd_ucmd *ucmd;

	DEBUGP("resp: %s\n", resp);

	int subtype = 0;
	if (cmd->gph)
		subtype = cmd->gph->msg_subtype;

	ucmd = gsmd_ucmd_fill(gu, strlen(resp)+1, GSMD_MSG_PHONE, subtype, cmd);

	if (valid_ucmd(ucmd)) {
		strcpy(ucmd->buf, resp);
	}

	return usock_cmd_enqueue(ucmd, gu);
}

static int quick_response(struct gsmd_user *gu, int type, int subtype, int ret)
{
	int retval = -ENOMEM;
	struct gsmd_ucmd *ucmd = ucmd_header(gu->gsmd);
	if (ucmd) {
		ucmd->hdr.msg_type = type;
		ucmd->hdr.msg_subtype = subtype;
		ucmd->hdr.id = 0;
		ucmd->hdr.ret = ret;
		ucmd->hdr.flags = UCMD_FINAL_CB_FLAG;
		retval = usock_cmd_enqueue(ucmd, gu);
	}
	return retval;
}

static int usock_rcv_phone(struct gsmd_user *gu, struct gsmd_msg_hdr *gph,
			   int len)
{
	struct gsmd_atcmd *cmd;
	struct gsmd* gsmd = gu->gsmd;

	switch (gph->msg_subtype) {
	case GSMD_PHONE_SUSPEND:
		DEBUGP("-- Suspending --\n");
		cancel_all_atcmds(gsmd);
		remove_all_timers();
		if (gsmd->reset_timer) {
			gsmd_timer_free(gsmd->reset_timer);
			gsmd->reset_timer = NULL;
		}
		if (gsmd->nak_timer) {
			gsmd_timer_free(gsmd->nak_timer);
			gsmd->nak_timer = NULL;
		}
		if (gsmd->sim_inserted_retry_timer) {
			gsmd_timer_free(gsmd->sim_inserted_retry_timer);
			gsmd->sim_inserted_retry_timer = NULL;
		}
		if (gsmd->sim_busy_retry_timer) {
			gsmd_timer_free(gsmd->sim_busy_retry_timer);
			gsmd->sim_busy_retry_timer = NULL;
		}
		gu->gsmd->suspended = 1;
		return quick_response(gu, GSMD_MSG_PHONE, GSMD_PHONE_SUSPEND, 0);
	case GSMD_PHONE_RESUME:
		DEBUGP("-- Resuming --\n");
		gu->gsmd->sim_inserted_retry_count = 0;
		gu->gsmd->sim_busy_retry_count = 0;
		gu->gsmd->suspended = 0;
		// No clue what state the serial and modem are in, so need to reset
		gsmd_eat_garbage();
		gsmd_initsettings2(gu->gsmd);
		return quick_response(gu, GSMD_MSG_PHONE, GSMD_PHONE_RESUME, 0);
	case GSMD_PHONE_POWERUP:
		cmd = atcmd_fill("AT+CFUN=1", 9+1, &phone_power_cb, gu, gph);
		break;
	case GSMD_PHONE_SHUTDOWN:
		return shutdown_modem(gu->gsmd,gph);
	case GSMD_PHONE_POWERDOWN:
		cmd = atcmd_fill("AT+CFUN=0", 9+1, &phone_power_cb, gu, gph);
		break;
	case GSMD_PHONE_FLIGHTMODE:
		cmd = atcmd_fill("AT+CFUN=4", 9+1, &phone_power_cb, gu, gph); // Not available on Siemens
		break;
	case GSMD_PHONE_POWERSAVE: // Siemens specific - don't use if muxed
		if (gu->gsmd->number_channels > 1) {
			// TODO need to use mux PSC command in this instance
			gsmd_log(GSMD_DEBUG, "Comms power save not available\n");
			return -EINVAL;
		}
		cmd = atcmd_fill("AT+CFUN=7", 9+1, &phone_power_cb, gu, gph);
		break;
	case GSMD_PHONE_AV_CURRENT:
		cmd = atcmd_fill("AT^SBC?", 7 + 1, &phone_va_cb, gu, gph);
		break;
	case GSMD_PHONE_VOLTAGE:
		cmd = atcmd_fill("AT^SBV", 6 + 1, &phone_va_cb, gu, gph);
		break;
	case GSMD_PHONE_GET_MANUF:
		cmd = atcmd_fill("AT+CGMI", 7 + 1, &get_inf_cb, gu, gph);
		break;
	case GSMD_PHONE_GET_MODEL:
		cmd = atcmd_fill("AT+CGMM", 7 + 1, &get_inf_cb, gu, gph);
		break;
	case GSMD_PHONE_GET_REVISION:
		cmd = atcmd_fill("AT+CGMR", 7 + 1, &get_inf_cb, gu, gph);
		break;
	case GSMD_PHONE_GET_IMEI:
		cmd = atcmd_fill("AT+CGSN", 7 + 1, &get_inf_cb, gu, gph);
		break;
	default:
		return -EINVAL;
	}
	if (!cmd)
		return -ENOMEM;

	return atcmd_submit(gu->gsmd, cmd, GSMD_CMD_CHANNEL0);
}

static int network_vmail_cb(struct gsmd_atcmd *cmd, void *ctx, char *resp)
{
	struct gsmd_user *gu = ctx;
	struct gsmd_voicemail *vmail;
	struct gsmd_ucmd *ucmd;
	char *comma;

	DEBUGP("entering(cmd=%p, gu=%p)\n", cmd, gu);

	ucmd = gsmd_ucmd_fill(gu, sizeof(struct gsmd_voicemail), GSMD_MSG_NETWORK,
		GSMD_NETWORK_VMAIL_SET, cmd);

	if (valid_ucmd(ucmd)) {
		if (cmd->buf[7] == '=') {
			/* response to set command */
			ucmd->hdr.msg_subtype = GSMD_NETWORK_VMAIL_SET;
			/* FIXME: */
		} else {
			/* response to get command */
			char *tok = strtok(resp, ",");
			if (!tok)
				goto out_free_einval;
			ucmd->hdr.msg_subtype = GSMD_NETWORK_VMAIL_GET;
			vmail->enable = atoi(tok);

			tok = strtok(NULL, ",");
			if (!tok)
				goto out_free_einval;
			strncpy(vmail->addr.number, tok, GSMD_ADDR_MAXLEN);
			vmail->addr.number[GSMD_ADDR_MAXLEN] = '\0';

			tok = strtok(NULL, ",");
			if (!tok)
				goto out_free_einval;
			vmail->addr.type = atoi(tok);
		}
	}

	return usock_cmd_enqueue(ucmd, gu);

out_free_einval:
	gsmd_log(GSMD_ERROR, "can't understand voicemail resp <%s>\n",resp);
	ucmd->hdr.ret = -EINVAL;

	return usock_cmd_enqueue(ucmd, gu);
}

struct gsmd_ucmd *gsmd_ucmd_fill(struct gsmd_user *gu, int len, u_int8_t msg_type,
		u_int8_t msg_subtype, struct gsmd_atcmd *cmd)
{
	struct gsmd_ucmd *ucmd;

	if (cmd->ret) {
		gsmd_log(GSMD_NOTICE, "ucmd error <%d>\n",cmd->ret);
		ucmd = ucmd_header(gu->gsmd);
	} else {
		ucmd = ucmd_alloc(len);
		if (!ucmd) {
			gsmd_log(GSMD_NOTICE, "Alloc failed\n");
			ucmd = ucmd_header(gu->gsmd);
			if (ucmd)
				cmd->ret = -ENOMEM;
			else
				gsmd_log(GSMD_NOTICE, "Failed to create ucmd\n");
		}
	}

	if (ucmd) {
		ucmd->hdr.msg_type = msg_type;
		ucmd->hdr.msg_subtype = msg_subtype;
		ucmd->hdr.id = cmd->id;
		ucmd->hdr.ret = cmd->ret;
		if (cmd->flags & ATCMD_FINAL_CB_FLAG)
			ucmd->hdr.flags = UCMD_FINAL_CB_FLAG;
	}

	return ucmd;
}

static int network_sigq_cb(struct gsmd_atcmd *cmd, void *ctx, char *resp)
{
	struct gsmd_user *gu = ctx;
	struct gsmd_ucmd *ucmd;

	ucmd = gsmd_ucmd_fill(gu, sizeof(struct gsmd_signal_quality), GSMD_MSG_NETWORK,
				GSMD_NETWORK_SIGQ_GET, cmd);

	if (valid_ucmd(ucmd)) {
		struct gsmd_signal_quality *gsq = (struct gsmd_signal_quality *) ucmd->buf;
		char *comma;
		gsq->rssi = atoi(resp + 6);
		comma = strchr(resp, ',');
		if (!comma) {
			gsmd_log(GSMD_NOTICE, "Unknown format <%s>\n",resp);
			ucmd->hdr.ret = -EIO;
		} else {
		   gsq->ber = atoi(comma+1);
		}
	}

	return usock_cmd_enqueue(ucmd, gu);
}

static int network_oper_cb(struct gsmd_atcmd *cmd, void *ctx, char *resp)
{
	struct gsmd_user *gu = ctx;
	struct gsmd_ucmd *ucmd;
	const char *end, *opname;
	int format, s;

	if (!cmd->ret) {
		/* Format: <mode>[,<format>,<oper>] */
		/* In case we're not registered, return an empty string.  */
		if (sscanf(resp, "+COPS: %*i,%i,\"%n", &format, &s) <= 0)
			end = opname = resp;
		else {
			/* If the phone returned the opname in a short or numeric
			 * format, then it probably doesn't know the operator's full
			 * name or doesn't support it.  Return any information we
			 * have in this case.  */
			if (format != 0)
				gsmd_log(GSMD_NOTICE, "+COPS response in a format "
						" different than long alphanumeric - "
						" returning as is!\n");
			opname = resp + s;
			end = strchr(opname, '"');
			if (!end) {
				gsmd_log(GSMD_NOTICE, "Unknown format <%s>\n",resp);
				cmd->ret = -EINVAL;
			}
		}
	}

	ucmd = gsmd_ucmd_fill(gu, end - opname + 1, GSMD_MSG_NETWORK,
				GSMD_NETWORK_OPER_GET, cmd);

	if (valid_ucmd(ucmd)) {
		memcpy(ucmd->buf, opname, end - opname);
		ucmd->buf[end - opname] = '\0';
	}

	return usock_cmd_enqueue(ucmd, gu);
}

static int network_opers_parse(const char *str, struct gsmd_msg_oper out[])
{
	int len = 0;
	int stat, n;
	char opname_longalpha[16 + 1];
	char opname_shortalpha[8 + 1];
	char opname_num[6 + 1];

	if (strncmp(str, "+COPS: ", 7))
		goto final;
	str += 7;

	while (*str == '(') {
		n = 0;
		if (out) {
			out->is_last = 0;
			if (sscanf(str,
						"(%i,\"%16[^\"]\","
						"\"%8[^\"]\",\"%6[0-9]\")%n",
						&stat,
						opname_longalpha,
						opname_shortalpha,
						opname_num,
						&n) < 4) {
				opname_shortalpha[0] = 0;
				if (sscanf(str,"(%i,\"%16[^\"]\",,\"%6[0-9]\")%n",
							&stat,
							opname_longalpha,
							opname_num,
							&n) < 3) {
					goto final;
				}
			}
			out->stat = stat;
			memcpy(out->opname_longalpha, opname_longalpha,
					sizeof(out->opname_longalpha));
			memcpy(out->opname_shortalpha, opname_shortalpha,
					sizeof(out->opname_shortalpha));
			memcpy(out->opname_num, opname_num,
					sizeof(out->opname_num));
		} else
			if (sscanf(str,
						"(%*i,\"%*[^\"]\","
						"\"%*[^\"]\",\"%*[0-9]\")%n",
						&n) < 0) {
				goto final;
			}
			if (!n && sscanf(str,"(%*i,\"%*[^\"]\",,\"%*[0-9]\")%n",&n) < 0) {
				goto final;
			}
		if (n < 10 || str[n - 1] != ')')
			goto final;
		if (str[n] == ',')
			n ++;
		str += n;
		len ++;
		if (out)
			out ++;
	}
final:
	if (out)
		out->is_last = 1;
	return len;
}

static int network_opers_cb(struct gsmd_atcmd *cmd, void *ctx, char *resp)
{
	struct gsmd_user *gu = ctx;
	struct gsmd_ucmd *ucmd;
	int len = 0;

	if (!cmd->ret) {
		len = network_opers_parse(resp, 0);
		DEBUGP("len %d\n", len);
	}

	ucmd = gsmd_ucmd_fill(gu, sizeof(struct gsmd_msg_oper) * (len + 1), GSMD_MSG_NETWORK,
				GSMD_NETWORK_OPER_LIST, cmd);

	if (valid_ucmd(ucmd) && len) {
		network_opers_parse(resp, (struct gsmd_msg_oper *) ucmd->buf);
	}

	return usock_cmd_enqueue(ucmd, gu);
}

static int network_pref_opers_cb(struct gsmd_atcmd *cmd, void *ctx, char *resp)
{
	struct gsmd_user *gu = (struct gsmd_user *) ctx;
	struct gsmd_ucmd *ucmd;
	int index;
	int format;
	char opname[17];

	if (!cmd->ret) {
		if (sscanf(resp, "+CPOL: %i,%i,\"%16[^\"]\"", &index, &format, opname) < 2) {
			gsmd_log(GSMD_NOTICE, "Unknown format <%s>\n",resp);
			cmd->ret = -EINVAL;
		}
	}

	ucmd = gsmd_ucmd_fill(gu, sizeof(struct gsmd_msg_prefoper), GSMD_MSG_NETWORK,
				GSMD_NETWORK_PREF_LIST, cmd);

	if (valid_ucmd(ucmd)) {
		struct gsmd_msg_prefoper *entry = (struct gsmd_msg_prefoper *) ucmd->buf;
		entry->index = index;
		if (ATCMD_FINAL_CB_FLAG & cmd->flags)
			entry->is_last = 1;
		memcpy(entry->opname_longalpha, opname,
				sizeof(entry->opname_longalpha));
	}

	return usock_cmd_enqueue(ucmd, gu);
}

static int network_pref_num_cb(struct gsmd_atcmd *cmd, void *ctx, char *resp)
{
	struct gsmd_user *gu = (struct gsmd_user *) ctx;
	struct gsmd_ucmd *ucmd;
	int min_index, max_index;

	if (!cmd->ret) {
		/* This is not a full general case, theoretically the range string
		 * can include commas and more dashes, but we have no full parser for
		 * ranges yet.  */
		if (sscanf(resp, "+CPOL: (%i-%i)", &min_index, &max_index) < 2) {
			gsmd_log(GSMD_NOTICE, "Unknown format <%s>\n",resp);
			cmd->ret = -EINVAL;
		}
	}

	ucmd = gsmd_ucmd_fill(gu, sizeof(int), GSMD_MSG_NETWORK,
				GSMD_NETWORK_PREF_SPACE, cmd);

	if (valid_ucmd(ucmd)) {
		*((int*) ucmd->buf) = max_index - min_index + 1;
	}

	return usock_cmd_enqueue(ucmd, gu);
}

static int network_ownnumbers_cb(struct gsmd_atcmd *cmd, void *ctx, char *resp)
{
	struct gsmd_user *gu = (struct gsmd_user *) ctx;
	struct gsmd_ucmd *ucmd;
	int len = 0;

	if (!cmd->ret) {
		char dummy;
		if (sscanf(resp, "+CNUM: \"%*[^\"]\"%c%n", &dummy, &len) > 0)
			len -= strlen("+CNUM: \"\",");
		else
			len = 0;
	}

	ucmd = gsmd_ucmd_fill(gu, sizeof(struct gsmd_own_number) + len + 1, GSMD_MSG_NETWORK,
				GSMD_NETWORK_GET_NUMBER, cmd);

	if (valid_ucmd(ucmd)) {
		int ret;
		int type = 0;
		struct gsmd_own_number *num = (struct gsmd_own_number *) ucmd->buf;
		num->is_last = 0;

		if (len) {
			ret = sscanf(resp, "+CNUM: \"%[^\"]\",\"%32[^\"]\",%i,%*i,%i,",
					num->name, num->addr.number,
					&type, &num->service) - 1; // TODO service number not used in Siemens
			num->name[len] = 0;
		} else {
			ret = sscanf(resp, "+CNUM: ,\"%32[^\"]\",%i,%*i,%i,",
					num->addr.number,
					&type, &num->service);// TODO service number not used in Siemens
		}

		if (type < 255)
			num->addr.type = type;
		if (ret < 2) {
			// TODO stream comms SIM has a different cnum format
			gsmd_log(GSMD_NOTICE, "Unknown format <%s> <ret %d>\n",resp,ret);
			ucmd->hdr.ret = 0; //-EINVAL;
		}
		if (ret < 3)
			num->service = GSMD_SERVICE_UNKNOWN;
		if (ATCMD_FINAL_CB_FLAG & cmd->flags)
			num->is_last = 1;
	}

	return usock_cmd_enqueue(ucmd, gu);
}

static int get_network_status(struct gsmd_user *gu, struct gsmd_msg_hdr *gph)
{
	int retval = 0;
	struct gsmd_ucmd *ucmd;
	struct gsmd_atcmd tmp;

	tmp.ret = 0;
	tmp.flags = UCMD_FINAL_CB_FLAG;
	tmp.id = gph->id;

	ucmd = gsmd_ucmd_fill(gu, sizeof(u_int8_t), GSMD_MSG_NETWORK, 
		GSMD_NETWORK_GET_REG_ST, &tmp);

	if (valid_ucmd(ucmd)) {
		u_int8_t* st = (u_int8_t*) ucmd->buf;
		*st = gu->gsmd->network_status;
		gsmd_log(GSMD_DEBUG, "get_network_status %d\n",*st);
	}

	if (usock_cmd_enqueue(ucmd, gu))
		retval = -ENOMEM;
	return retval;
}

static int usock_rcv_network(struct gsmd_user *gu, struct gsmd_msg_hdr *gph,
				 int len)
{
	struct gsmd_atcmd *cmd;
	struct gsmd_voicemail *vmail = (struct gsmd_voicemail *) gph->data;
	char *oper = (char *) gph->data;
	char buffer[15 + sizeof(gsmd_oper_numeric)];
	int cmdlen;

	switch (gph->msg_subtype) {
	case GSMD_NETWORK_REGISTER:
		if (oper && *oper)
			cmdlen = sprintf(buffer, "AT+COPS=1,2,\"%s\"",oper);
		else
			cmdlen = sprintf(buffer, "AT+COPS=0");
		cmd = atcmd_fill(buffer, cmdlen + 1, &simple_cmd_cb, gu, gph);
		break;
	case GSMD_NETWORK_DEREGISTER:
		cmd = atcmd_fill("AT+COPS=2", 9+1, &simple_cmd_cb, gu, gph);
		break;
	case GSMD_NETWORK_VMAIL_GET:
		cmd = atcmd_fill("AT+CSVM?", 8+1, &network_vmail_cb, gu, gph);
		break;
	case GSMD_NETWORK_VMAIL_SET:
		cmd = atcmd_fill("AT+CSVM=", 8+1, &network_vmail_cb, gu, gph);
		break;
	case GSMD_NETWORK_SIGQ_GET:
		cmd = atcmd_fill("AT+CSQ", 6+1, &network_sigq_cb, gu, gph);
		break;
	case GSMD_NETWORK_OPER_GET:
		/* Set long alphanumeric format */
		//atcmd_submit(gu->gsmd, atcmd_fill("AT+COPS=3,0", 11+1,
		//			&null_cmd_cb, gu, NULL),GSMD_CMD_CHANNEL0); // TODO this should do the 2nd cmd only if the 1st one succeeds
		cmd = atcmd_fill("AT+COPS?", 8+1, &network_oper_cb, gu, gph);
		break;
	case GSMD_NETWORK_OPER_LIST:
		cmd = atcmd_fill("AT+COPS=?", 9+1, &network_opers_cb, gu, gph);
		// Longer timeout, COPS can take a while
		if (cmd) cmd->timeout_value = 60;
		break;
	case GSMD_NETWORK_PREF_LIST:
		/* Set long alphanumeric format */
		//atcmd_submit(gu->gsmd, atcmd_fill("AT+CPOL=,0", 10 + 1,
		//			&null_cmd_cb, gu, NULL), GSMD_CMD_CHANNEL0); // TODO this should do the 2nd cmd only if the 1st one succeeds
		cmd = atcmd_fill("AT+CPOL?", 8 + 1, &network_pref_opers_cb, gu, gph);
		// Longer timeout, CPOL can take a while
		if (cmd) cmd->timeout_value = 60;
		break;
	case GSMD_NETWORK_PREF_DEL:
		cmdlen = sprintf(buffer, "AT+CPOL=%i", *(int *) gph->data);
		cmd = atcmd_fill(buffer, cmdlen + 1, &simple_cmd_cb, gu, gph);
		break;
	case GSMD_NETWORK_PREF_ADD:
		cmdlen = sprintf(buffer, "AT+CPOL=,2,\"%s\"", oper);
		cmd = atcmd_fill(buffer, cmdlen + 1, &simple_cmd_cb, gu, gph);
		break;
	case GSMD_NETWORK_PREF_SPACE:
		cmd = atcmd_fill("AT+CPOL=?", 9 + 1, &network_pref_num_cb, gu, gph);
		break;
	case GSMD_NETWORK_GET_NUMBER:
		cmd = atcmd_fill("AT+CNUM", 7 + 1, &network_ownnumbers_cb, gu, gph);
		break;
	case GSMD_NETWORK_GET_REG_ST:
		return get_network_status(gu, gph);
	default:
		return -EINVAL;
	}
	if (!cmd)
		return -ENOMEM;

	return atcmd_submit(gu->gsmd, cmd, GSMD_CMD_CHANNEL0);
}

/* forward decl */
static int usock_rcv_phonebook(struct gsmd_user *gu,
		struct gsmd_msg_hdr *gph,int len);

void cleanup_sim_busy(struct gsmd *gsmd, int do_cancel_cb)
{
	DEBUGP("cleanup_sim_busy %d\n",do_cancel_cb);

	if (gsmd->sim_busy_retry_timer) {
		gsmd_timer_free(gsmd->sim_busy_retry_timer);
		gsmd->sim_busy_retry_timer=NULL;
	}

	if (gsmd->sim_busy_dreq) {
		struct gsmd_atcmd *dreq = gsmd->sim_busy_dreq;
		if (do_cancel_cb) {
			DEBUGP("informing client of error %d\n",do_cancel_cb);
			dreq->ret = do_cancel_cb;
			dreq->cb(dreq, dreq->ctx, "ERROR");
		}

		if (dreq->gph)
			talloc_free(dreq->gph);

		talloc_free(gsmd->sim_busy_dreq);
		gsmd->sim_busy_dreq = NULL;
	}
}

static void sim_busy_retry_cb(struct gsmd_timer *tmr, void *data)
{
	struct gsmd *gsmd = data;
	if (gsmd) {
		int do_cancel_cb = 0;
		int retval;
		struct gsmd_atcmd *dreq = gsmd->sim_busy_dreq;

		DEBUGP("retry sim busy req\n");
		gsmd->sim_busy_retry_count++;
	
		/* redo the original phonebook request */
		retval = usock_rcv_phonebook(dreq->ctx,dreq->gph,dreq->gph->len);
		if (retval < 0) {
			DEBUGP("failed <%d> to exec phonebook re-request\n",retval);
			do_cancel_cb = retval;
		}

		/* clean up */
		cleanup_sim_busy(gsmd, do_cancel_cb);
	}
}

static int do_sim_busy_retry(struct gsmd_user *gu, struct gsmd_atcmd *cmd)
{
	struct gsmd *gsmd = gu->gsmd;
	if (gsmd && cmd && !gsmd->sim_busy_retry_timer && gsmd->sim_busy_retry_count < MAX_SIM_BUSY_RETRIES) {
		struct timeval tv;

		DEBUGP("sim busy (busy retry count %d)\n",gsmd->sim_busy_retry_count);
		tv.tv_sec = SIM_BUSY_RETRY_DELAY;
		tv.tv_usec = 0;

		if (gsmd->sim_busy_dreq) {
			gsmd_log(GSMD_ERROR, "unexpected busy req data already exists\n");
			cleanup_sim_busy(gsmd,0);
		}

		gsmd->sim_busy_retry_timer = gsmd_timer_create(&tv,&sim_busy_retry_cb,gsmd);
		if (!gsmd->sim_busy_retry_timer) {
			gsmd_log(GSMD_ERROR, "failed to create busy retry timeout\n");
		} else {
			gsmd->sim_busy_dreq = atcmd_fill(cmd->buf, cmd->buflen, cmd->cb, gu, cmd->gph);
			if (!gsmd->sim_busy_dreq) {
				gsmd_log(GSMD_ERROR, "failed to copy orig pbk request\n");
				cleanup_sim_busy(gsmd,0);
				return 0;
			}

			/* timeout request successfully set up */
			DEBUGP("sim is busy - requested a retry in %d seconds\n",SIM_BUSY_RETRY_DELAY);
			return 1;
		}
	}
	return 0;
}

static int phonebook_find_cb(struct gsmd_atcmd *cmd, void *ctx, char *resp)
{
	struct gsmd_user *gu = ctx;
	struct gsmd_ucmd *ucmd;
	int retval = 0;
	struct gsmd_phonebooks *gps;
	char *fcomma, *lcomma, *ptr1, *ptr2 = NULL;
	int *num;

	DEBUGP("resp: %s\n", resp);

	if (cmd && (-GSM0707_CME_SIM_BUSY == cmd->ret) && do_sim_busy_retry(gu,cmd)) {
		retval = 0;
	} else {
		if ((!cmd->ret)&&!strncmp(resp,"+CPBF: ",7)) {
	
			gu->gsmd->sim_busy_retry_count = 0;

			/*
			* [+CPBF: <index1>,<number>,<type>,<text>[[...]
			* <CR><LF>+CPBF: <index2>,<unmber>,<type>,<text>]]
			*/
			ptr1 = strtok(resp, "\n");

			while (ptr1) {
				gps = talloc(__pb_f_ctx, struct gsmd_phonebooks);
				if (!gps) {
					gsmd_log(GSMD_NOTICE, "Failed find alloc\n");
					cmd->ret = -ENOMEM;
					break;
				}

				ptr2 = strchr(ptr1, ' ');
				gps->pb.index = atoi(ptr2+1);

				fcomma = strchr(ptr1, '"');
				lcomma = strchr(fcomma+1, '"');
				strncpy(gps->pb.numb, fcomma + 1, (lcomma-fcomma-1));
				gps->pb.numb[(lcomma - fcomma) - 1] = '\0';

				gps->pb.type = atoi(lcomma + 2);

				/* TODO handle gsm338 text */
				ptr2 = strrchr(ptr1, ',');
				fcomma = ptr2 + 1;
				lcomma = strchr(fcomma + 1, '"');
				strncpy(gps->pb.text, fcomma + 1, (lcomma - fcomma - 1));
				gps->pb.text[(lcomma - fcomma) - 1] = '\0';

				llist_add_tail(&gps->list, &gu->pb_find_list);

				gu->pb_find_num++;

				ptr1 = strtok(NULL, "\n");
			}
		}

		if (ATCMD_FINAL_CB_FLAG & cmd->flags) {
			if (gu->pb_find_status)
				cmd->ret = gu->pb_find_status;

			ucmd = gsmd_ucmd_fill(gu, sizeof(int), GSMD_MSG_PHONEBOOK,
						GSMD_PHONEBOOK_FIND, cmd);

			if (valid_ucmd(ucmd)) {
				*((int*) ucmd->buf) = gu->pb_find_num;
			}

			retval = usock_cmd_enqueue(ucmd, gu);
		} else {
			gu->pb_find_status = cmd->ret;
		}
	}
	return retval;
}

static int phonebook_read_cb(struct gsmd_atcmd *cmd, void *ctx, char *resp)
{
	int retval = 0;
	struct gsmd_user *gu = ctx;
	struct gsmd_phonebook *gp;
	struct gsmd_ucmd *ucmd;
	char *fcomma, *lcomma;
	char *ptr;

	DEBUGP("resp: %s\n", resp);

	if (cmd && (-GSM0707_CME_SIM_BUSY == cmd->ret) && do_sim_busy_retry(gu,cmd)) {
		retval = 0;
	} else {
		ucmd = gsmd_ucmd_fill(gu, sizeof(struct gsmd_phonebook), GSMD_MSG_PHONEBOOK,
					GSMD_PHONEBOOK_READ, cmd);
	
		if (valid_ucmd(ucmd)) {
			gu->gsmd->sim_busy_retry_count = 0;

			gp = (struct gsmd_phonebook *) ucmd->buf;

			/* check the record is empty or not */
			if (!strncmp(resp, "+CPBR", 5)) {
				ptr = strchr(resp, ' ');
				gp->index = atoi(ptr + 1);

				fcomma = strchr(resp, '"');
				lcomma = strchr(fcomma + 1, '"');
				strncpy(gp->numb, fcomma + 1, (lcomma - fcomma - 1));
				gp->numb[(lcomma-fcomma) - 1] = '\0';

				gp->type = atoi(lcomma + 2);

				/* TODO needs rework for gsm338 */ 
				ptr = strrchr(resp, ',');
				fcomma = ptr + 1;
				lcomma = strchr(fcomma + 1, '"');
				strncpy(gp->text, fcomma + 1, (lcomma-fcomma - 1));
				gp->text[(lcomma - fcomma) - 1] = '\0';
			}
			else
				gp->index = 0;
		}

		retval = usock_cmd_enqueue(ucmd, gu);
	}
	return retval;
}

static int phonebook_readrg_cb(struct gsmd_atcmd *cmd, void *ctx, char *resp)
{
	struct gsmd_user *gu = ctx;
	struct gsmd_ucmd *ucmd;
	int retval = 0;
	struct gsmd_phonebooks *gps;
	char *fquote, *lquote, *ptr1, *n_ptr1, *ptr2 = NULL;
	int txtlen, iterstart, iter, iterend;
	int totallen = cmd->resplen;

	DEBUGP("resp: %s(%d)\n", resp, totallen);

	if ((-GSM0707_CME_SIM_BUSY == cmd->ret) && do_sim_busy_retry(gu,cmd)) {
		retval = 0;
	} else if (totallen) {
		gu->gsmd->sim_busy_retry_count = 0;

		/*
		* [+CPBR: <index1>,<number>,<type>,<text>[[...]
		* <CR><LF>+CPBR: <index2>,<number>,<type>,<text>]]
		*
		* Beware about use of conventional C string routines, the text component
		* is in gsm338 format - NOT ascii
		*/

		if ((!cmd->ret)&&!strncmp(resp,"+CPBR: ",7)) {

			ptr1 = resp;
			while (ptr1) {
				n_ptr1 = NULL;
				for (txtlen = 0; txtlen < totallen; txtlen++) {
					if ('\n' == ptr1[txtlen]) {
						totallen -= txtlen;
						ptr1[txtlen] = '\0';
						if (txtlen+1 < totallen)
							n_ptr1 = &ptr1[txtlen+1];
					}
				}

				gps = talloc(__pb_r_ctx, struct gsmd_phonebooks);
				if (!gps) {
					gsmd_log(GSMD_NOTICE, "Failed readrg alloc\n");
					cmd->ret = -ENOMEM;
					break;
				}

				ptr2 = strchr(ptr1, ' ');
				if (!ptr2)
					break;
				gps->pb.index = atoi(++ptr2);

				/* number */
				fquote = strchr(ptr2, '"');
				if (!fquote)
					break;
				lquote = strchr(fquote+1, '"');
				if (!lquote)
					break;
				strncpy(gps->pb.numb, fquote + 1, (lquote-fquote-1));
				gps->pb.numb[(lquote - fquote) - 1] = '\0';

				ptr2 = strchr(lquote, ',');
				if (!ptr2)
					break;

				/* type */
				gps->pb.type = atoi(++ptr2);

				ptr2 = strchr(ptr2, ',');
				if (!ptr2)
					break;

				/* text (gsm338 format) */
				fquote = strchr(ptr2, '"');
				if (!fquote)
					break;
				/* find the last quote in the text */
				iterstart = fquote-ptr1+1;
				iterend = 0;
				for (iter = iterstart; iter < txtlen; iter++) {
					if ('"' == ptr1[iter]) {
						iterend = iter;
					}
				}
				if (!iterend)
					break;

				/* copy the text, ignore 0s (i.e @ in gsm338) */
				lquote = gps->pb.text;
				gps->pb.text_len = iterend - iterstart;
				if (gps->pb.text_len > GSMD_PB_TEXT_MAXLEN) {
					gsmd_log(GSMD_NOTICE, "* Truncating %d > pb txt max *\n",
						gps->pb.text_len);
					gps->pb.text_len = GSMD_PB_TEXT_MAXLEN;
					iterend = iterstart + GSMD_PB_TEXT_MAXLEN;
				}
				for (iter = iterstart; iter < iterend; iter++) {
					*lquote = ptr1[iter];
					lquote++;
				}
				*lquote = '\0';

				gsmd_log(GSMD_DEBUG, "Add contact <%s> <%s>(%d)\n",
					gps->pb.numb,gps->pb.text,gps->pb.text_len);
				llist_add_tail(&gps->list, &gu->pb_readrg_list);
				gu->pb_readrg_num++;
	
				ptr1 = n_ptr1;
			}
		}

		if (ATCMD_FINAL_CB_FLAG & cmd->flags) {
			if (gu->pb_readrg_status)
				cmd->ret = gu->pb_readrg_status;

			ucmd = gsmd_ucmd_fill(gu, sizeof(int), GSMD_MSG_PHONEBOOK,
						GSMD_PHONEBOOK_READRG, cmd);

			if (valid_ucmd(ucmd)) {
				*((int*) ucmd->buf) = gu->pb_readrg_num;
			}

			retval = usock_cmd_enqueue(ucmd, gu);
		} else {
			gu->pb_readrg_status = cmd->ret;
		}
	}
	return retval;
}

static int phonebook_write_cb(struct gsmd_atcmd *cmd, void *ctx, char *resp)
{
	int retval = 0;
	struct gsmd_user *gu = ctx;
	struct gsmd_ucmd *ucmd;

	DEBUGP("resp: %s\n", resp);

	if (cmd && (-GSM0707_CME_SIM_BUSY == cmd->ret) && do_sim_busy_retry(gu,cmd)) {
		retval = 0;
	} else {
		ucmd = gsmd_ucmd_fill(gu, strlen(resp)+1, GSMD_MSG_PHONEBOOK,
					GSMD_PHONEBOOK_WRITE, cmd);

		if (valid_ucmd(ucmd)) {
			gu->gsmd->sim_busy_retry_count = 0;

			strcpy(ucmd->buf, resp); //TODO why pass the resp back?
		}

		retval = usock_cmd_enqueue(ucmd, gu);
	}
	return retval;
}

static int phonebook_delete_cb(struct gsmd_atcmd *cmd, void *ctx, char *resp)
{
	int retval = 0;
	struct gsmd_user *gu = ctx;
	struct gsmd_ucmd *ucmd;

	DEBUGP("resp: %s\n", resp);

	if (cmd && (-GSM0707_CME_SIM_BUSY == cmd->ret) && do_sim_busy_retry(gu,cmd)) {
		retval = 0;
	} else {
		ucmd = gsmd_ucmd_fill(gu, strlen(resp)+1, GSMD_MSG_PHONEBOOK,
					GSMD_PHONEBOOK_DELETE, cmd);

		if (valid_ucmd(ucmd)) {
			gu->gsmd->sim_busy_retry_count = 0;

			strcpy(ucmd->buf, resp); //TODO why pass the resp back?
		}

		retval = usock_cmd_enqueue(ucmd, gu);
	}
	return retval;
}

static int phonebook_get_support_cb(struct gsmd_atcmd *cmd, void *ctx, char *resp)
{
	/* +CPBR: (1-100),44,16 */
	int retval = 0;
	struct gsmd_user *gu = ctx;
	struct gsmd_ucmd *ucmd;

	DEBUGP("resp: %s\n", resp);
	if (cmd && (-GSM0707_CME_SIM_BUSY == cmd->ret) && do_sim_busy_retry(gu,cmd)) {
		retval = 0;
	} else {
		gu->gsmd->sim_busy_retry_count = 0;

		ucmd = gsmd_ucmd_fill(gu, sizeof(struct gsmd_phonebook_support),
					GSMD_MSG_PHONEBOOK, GSMD_PHONEBOOK_GET_SUPPORT, cmd);

		while (valid_ucmd(ucmd)) {
			char *fcomma, *lcomma;
			char *dash;
			struct gsmd_phonebook_support *gps = (struct gsmd_phonebook_support *) ucmd->buf;
	
			gu->gsmd->sim_busy_retry_count = 0;

			dash = strchr(resp, '-');
			if (!dash) {
				gsmd_log(GSMD_NOTICE, "Unknown format <%s>\n",resp);
				ucmd->hdr.ret = -EIO;
				break;
			}
			gps->index = atoi(dash + 1);
	
			fcomma = strchr(resp, ',');
			if (!fcomma) {
				gsmd_log(GSMD_NOTICE, "Unknown format <%s>\n",resp);
				ucmd->hdr.ret = -EIO;
				break;
			}
			gps->nlength = atoi(fcomma+1);

			lcomma = strrchr(resp, ',');
			if (!lcomma) {
				gsmd_log(GSMD_NOTICE, "Unknown format <%s>\n",resp);
				ucmd->hdr.ret = -EIO;
				break;
			}
			gps->tlength = atoi(lcomma+1);
			break;
		}

		retval = usock_cmd_enqueue(ucmd, gu);
	}
	return retval;
}

static int phonebook_list_storage_cb(struct gsmd_atcmd *cmd,
		void *ctx, char *resp)
{
	/* TODO; using link list */
	int retval = 0;
	struct gsmd_user *gu = ctx;
	struct gsmd_ucmd *ucmd;

	DEBUGP("resp: %s\n", resp);

	if (cmd && (-GSM0707_CME_SIM_BUSY == cmd->ret) && do_sim_busy_retry(gu,cmd)) {
		retval = 0;
	} else {
		/*
		* +CPBS: (<storage>s)
		*/

		ucmd = gsmd_ucmd_fill(gu, sizeof(struct gsmd_phonebook_storage),
					GSMD_MSG_PHONEBOOK, GSMD_PHONEBOOK_LIST_STORAGE, cmd);

		if (valid_ucmd(ucmd)) {
			gu->gsmd->sim_busy_retry_count = 0;

			if (!strncmp(resp, "+CPBS", 5)) {
				struct gsmd_phonebook_storage *gps =
					(struct gsmd_phonebook_storage *) ucmd->buf;
				gps->num = 0;
				char* delim = "(,";
				char *ptr = strpbrk(resp, delim);
				while ( ptr ) {
					strncpy(gps->mem[gps->num].type, ptr+2, 2);
					gps->mem[gps->num].type[2] = '\0';
					ptr = strpbrk(ptr+2, delim);
					gps->num++;
				}
			} else {
				gsmd_log(GSMD_NOTICE, "Unknown format <%s>\n",resp);
				ucmd->hdr.ret = -EINVAL;
			}
		}

		retval = usock_cmd_enqueue(ucmd, gu);
	}
	return retval;
}

static int get_imsi_cb(struct gsmd_atcmd *cmd, void *ctx, char *resp)
{
	struct gsmd_user *gu = ctx;
	struct gsmd_ucmd *ucmd;

	DEBUGP("resp: %s\n", resp);

	ucmd = gsmd_ucmd_fill(gu, strlen(resp)+1, GSMD_MSG_PHONEBOOK,
				GSMD_PHONEBOOK_GET_IMSI, cmd);

	if (valid_ucmd(ucmd)) {
		strcpy(ucmd->buf, resp);
	}

	return usock_cmd_enqueue(ucmd, gu);
}


static int usock_rcv_phonebook(struct gsmd_user *gu,
		struct gsmd_msg_hdr *gph,int len)
{
	struct gsmd_atcmd *cmd = NULL;
	struct gsmd_ucmd *ucmd = NULL;
	struct gsmd_phonebook_readrg *gpr;
	struct gsmd_phonebook *gp;
	struct gsmd_phonebook_find *gpf;
	struct gsmd_phonebooks *cur, *cur2;
	int *index, *num;
	int atcmd_len, i;
	char *storage;
	char buf[1024];

	switch (gph->msg_subtype) {
	case GSMD_PHONEBOOK_LIST_STORAGE:
		cmd = atcmd_fill("AT+CPBS=?", 9 + 1,
				&phonebook_list_storage_cb,
				gu, gph);
		break;
	case GSMD_PHONEBOOK_SET_STORAGE:
		if (len < sizeof(*gph) + 3)
			return -EINVAL;

		storage = (char*) ((void *)gph + sizeof(*gph));

		/* ex. AT+CPBS="ME" */
		atcmd_len = 1 + strlen("AT+CPBS=\"") + 2 + strlen("\"");
		cmd = atcmd_fill("AT+CPBS=\"", atcmd_len,
				&simple_cmd_cb, gu, gph);

		if (!cmd)
			return -ENOMEM;

		cmd->timeout_value = 10;
		sprintf(cmd->buf, "AT+CPBS=\"%s\"", storage);
		break;
	case GSMD_PHONEBOOK_FIND:
		if(len < sizeof(*gph) + sizeof(*gpf))
			return -EINVAL;
		gpf = (struct gsmd_phonebook_find *) ((void *)gph + sizeof(*gph));

		atcmd_len = 1 + strlen("AT+CPBF=\"") +
			strlen(gpf->findtext) + strlen("\"");
		cmd = atcmd_fill("AT+CPBF=\"", atcmd_len,
				 &phonebook_find_cb, gu, gph);
		if (!cmd)
			return -ENOMEM;

		if (!llist_empty(&gu->pb_find_list)) {
			DEBUGP("Cache exists - removing items\n");
			llist_for_each_entry_safe(cur, cur2, &gu->pb_find_list, list) {
				llist_del(&cur->list);
				talloc_free(cur);
			}
		}

		cmd->timeout_value = 10;
		sprintf(cmd->buf, "AT+CPBF=\"%s\"", gpf->findtext);
		gu->pb_find_num = 0;
		gu->pb_find_status = 0;
		break;
	case GSMD_PHONEBOOK_READ:
		if(len < sizeof(*gph) + sizeof(int))
			return -EINVAL;

		index = (int *) ((void *)gph + sizeof(*gph));

		sprintf(buf, "%d", *index);

		/* ex, AT+CPBR=23 */
		atcmd_len = 1 + strlen("AT+CPBR=") + strlen(buf);
		cmd = atcmd_fill("AT+CPBR=", atcmd_len,
				 &phonebook_read_cb, gu, gph);
		if (!cmd)
			return -ENOMEM;
		cmd->timeout_value = 10;
		sprintf(cmd->buf, "AT+CPBR=%d", *index);
		break;
	case GSMD_PHONEBOOK_READRG:
		if(len < sizeof(*gph) + sizeof(*gpr))
			return -EINVAL;
		gpr = (struct gsmd_phonebook_readrg *) ((void *)gph + sizeof(*gph));

		sprintf(buf, "%d,%d", gpr->index1, gpr->index2);

		if (!llist_empty(&gu->pb_readrg_list)) {
			DEBUGP("Cache exists - removing items\n");
			llist_for_each_entry_safe(cur, cur2, &gu->pb_readrg_list, list) {
				llist_del(&cur->list);
				talloc_free(cur);
			}
		}

		/* ex, AT+CPBR=1,100 */
		atcmd_len = 1 + strlen("AT+CPBR=") + strlen(buf);
		cmd = atcmd_fill("AT+CPBR=", atcmd_len,
				 &phonebook_readrg_cb, gu, gph);
		if (!cmd)
			return -ENOMEM;
		gu->pb_readrg_num = 0;
		gu->pb_readrg_status = 0;
		cmd->timeout_value = 60; /* this can take a long time to execute */
		sprintf(cmd->buf, "AT+CPBR=%s", buf);
		break;
	case GSMD_PHONEBOOK_WRITE:
		if(len < sizeof(*gph) + sizeof(*gp))
			return -EINVAL;
		gp = (struct gsmd_phonebook *) ((void *)gph + sizeof(*gph));

		/* TODO text handling needs rework for gsm338 */ 
		sprintf(buf, "%d,\"%s\",%d,\"%s\"",
				gp->index, gp->numb, gp->type, gp->text);

		atcmd_len = 1 + strlen("AT+CPBW=") + strlen(buf);
		cmd = atcmd_fill("AT+CPBW=", atcmd_len,
				 &phonebook_write_cb, gu, gph);
		if (!cmd)
			return -ENOMEM;
		cmd->timeout_value = 10;
		sprintf(cmd->buf, "AT+CPBW=%s", buf);
		break;
	case GSMD_PHONEBOOK_DELETE:
		if(len < sizeof(*gph) + sizeof(int))
			return -EINVAL;
		index = (int *) ((void *)gph + sizeof(*gph));

		sprintf(buf, "%d", *index);

		/* ex, AT+CPBW=3*/
		atcmd_len = 1 + strlen("AT+CPBW=") + strlen(buf);
		cmd = atcmd_fill("AT+CPBW=", atcmd_len,
				 &phonebook_delete_cb, gu, gph);
		if (!cmd)
			return -ENOMEM;
		cmd->timeout_value = 10;
		sprintf(cmd->buf, "AT+CPBW=%s", buf);
		break;
	case GSMD_PHONEBOOK_GET_SUPPORT:
		cmd = atcmd_fill("AT+CPBR=?", 9+1,
				 &phonebook_get_support_cb, gu, gph);
		break;
	case GSMD_PHONEBOOK_RETRIEVE_READRG:
		{
		struct gsmd_atcmd tmp;
		if (len < sizeof(struct gsmd_msg_hdr))
			return -EINVAL;

		tmp.ret = gu->pb_readrg_status;
		tmp.flags = UCMD_FINAL_CB_FLAG;
		tmp.id = gph->id;

		ucmd = gsmd_ucmd_fill(gu, sizeof(struct gsmd_phonebook)*gu->pb_readrg_num,
				GSMD_MSG_PHONEBOOK, GSMD_PHONEBOOK_RETRIEVE_READRG, &tmp);

		if (valid_ucmd(ucmd)) {
			int iter;
			gp = (struct gsmd_phonebook*) ucmd->buf;

			if (!llist_empty(&gu->pb_readrg_list)) {

				llist_for_each_entry_safe(cur, cur2,
						&gu->pb_readrg_list, list) {
					gp->index = cur->pb.index;
					strcpy(gp->numb, cur->pb.numb);
					gp->type = cur->pb.type;
					gp->text_len = cur->pb.text_len;
					for (iter = 0; iter < gp->text_len; iter++)
						gp->text[iter] = cur->pb.text[iter];
					gp->text[iter] = '\0';
					DEBUGP("adding contact <%s> <%s>(%d)\n",
						gp->numb, gp->text, gp->text_len);
					gp++;

					llist_del(&cur->list);
					talloc_free(cur);
				}
			}
			gu->pb_readrg_num = 0;
		}

		if (usock_cmd_enqueue(ucmd, gu))
			return -ENOMEM;
		}
		break;
	case GSMD_PHONEBOOK_RETRIEVE_FIND:
		{
		struct gsmd_atcmd tmp;
		if (len < sizeof(struct gsmd_msg_hdr))
			return -EINVAL;

		tmp.ret = gu->pb_find_status;
		tmp.flags = UCMD_FINAL_CB_FLAG;
		tmp.id = gph->id;

		ucmd = gsmd_ucmd_fill(gu, sizeof(struct gsmd_phonebook)*gu->pb_find_num,
				GSMD_MSG_PHONEBOOK, GSMD_PHONEBOOK_RETRIEVE_FIND, &tmp);

		if (valid_ucmd(ucmd)) {
			int iter;
			gp = (struct gsmd_phonebook*) ucmd->buf;

			if (!llist_empty(&gu->pb_find_list)) {
				llist_for_each_entry_safe(cur, cur2, &gu->pb_find_list, list) {
					gp->index = cur->pb.index;
					strcpy(gp->numb, cur->pb.numb);
					gp->type = cur->pb.type;
					gp->text_len = cur->pb.text_len;
					for (iter = 0; iter < gp->text_len; iter++)
						gp->text[iter] = cur->pb.text[iter];
					gp->text[iter] = '\0';
					DEBUGP("adding found contact <%s> <%s>(%d)\n",
						gp->numb, gp->text, gp->text_len);
					gp++;

					llist_del(&cur->list);
					talloc_free(cur);
				}
			}
			gu->pb_find_num = 0;
		}

		if (usock_cmd_enqueue(ucmd, gu))
			return -ENOMEM;
		}
		break;
	case GSMD_PHONEBOOK_GET_IMSI:
		cmd = atcmd_fill("AT+CIMI", 7 + 1, &get_imsi_cb, gu, gph);
		break;

	default:
		return -EINVAL;
	}

	if (cmd)
		return atcmd_submit(gu->gsmd, cmd, GSMD_CMD_CHANNEL0);
	else
		return 0;
}

static int gprs_ip_cb(struct gsmd_atcmd *cmd, void *ctx, char *resp)
{
	struct gsmd_user *gu = ctx;
	struct gsmd_ucmd *ucmd;

	DEBUGP("resp: %s\n", resp);

	ucmd = gsmd_ucmd_fill(gu, strlen(resp)+1, GSMD_MSG_GPRS,
				GSMD_GPRS_GET_IP_ADDR, cmd);

	if (valid_ucmd(ucmd)) {
		/* expect +CGPADDR: %d,"w.x.y.z" */
		if (!strncmp(resp, "+CGPADDR:", 9)) {
			char *fcomma = strchr(resp, '"');
			if (fcomma) {
				char *lcomma = strchr(fcomma + 1, '"');
				strncpy(ucmd->buf, fcomma + 1, (lcomma - fcomma - 1));
				ucmd->buf[(lcomma-fcomma) - 1] = '\0';
			} else {
				ucmd->hdr.ret = -EINVAL;
			}
		} else {
			ucmd->hdr.ret = -EINVAL;
		}
	}

	return usock_cmd_enqueue(ucmd, gu);
}

static int usock_rcv_gprs(struct gsmd_user *gu, struct gsmd_msg_hdr *gph,
			   int len)
{
	struct gsmd_atcmd *cmd;
	char buf[50];
	int context;

	if (gu->gsmd->number_channels <= GSMD_GPRS_DATA_CHANNEL) {
		// Multiplexing needs to be active for simultaneous control/notifications
		// and GPRS data to work
		gsmd_log(GSMD_ERROR, "Number of channels %d is not enough for GPRS\n",
			gu->gsmd->number_channels);
		return -EINVAL;
	}

	switch (gph->msg_subtype) {
	case GSMD_GPRS_ACTIVATE:
	case GSMD_GPRS_DEACTIVATE:
	case GSMD_GPRS_CONNECT:
	case GSMD_GPRS_GET_IP_ADDR:
		/* validate context id */
		if(len < sizeof(struct gsmd_msg_hdr) + sizeof(int))
			return -EINVAL;
		context = *((int *) ((void *)gph + sizeof(struct gsmd_msg_hdr)));
		break;
	default:
		break;
	}

	switch (gph->msg_subtype) {
	case GSMD_GPRS_ATTACH:
		cmd = atcmd_fill("AT+CGATT=1", 10+1, &simple_cmd_cb, gu, gph);
		// Longer timeout, CGATT can take a while
		if (cmd) cmd->timeout_value = 120;
		break;
	case GSMD_GPRS_DETACH:
		cmd = atcmd_fill("AT+CGATT=0", 10+1, &simple_cmd_cb, gu, gph);
		break;
	case GSMD_GPRS_ACTIVATE:
		sprintf(buf, "AT+CGACT=1,%d", context);
		cmd = atcmd_fill(buf, strlen(buf) + 1, &simple_cmd_cb, gu, gph);
		// Longer timeout, CGACT can take a while
		if (cmd) cmd->timeout_value = 120;
		break;
	case GSMD_GPRS_DEACTIVATE:
		sprintf(buf, "AT+CGACT=0,%d", context);
		cmd = atcmd_fill(buf, strlen(buf) + 1, &simple_cmd_cb, gu, gph);
		break;
	case GSMD_GPRS_CONNECT:
		//sprintf(buf, "AT+CGDATA=\"PPP\",%d", context);
		// horrible back by CMJ. For some reason connecting using ATD allows IPCP
		// to assign DNS addresses, but using AT+CGDATA doesn't.
		sprintf(buf,"ATD*99***%d#", context);
		cmd = atcmd_fill(buf, strlen(buf) + 1, &simple_cmd_cb, gu, gph);
		break;
	case GSMD_GPRS_GET_IP_ADDR:
		sprintf(buf, "AT+CGPADDR=%d", context);
		cmd = atcmd_fill(buf, strlen(buf) + 1, &gprs_ip_cb, gu, gph);
		break;
	case GSMD_GPRS_SET_CONFIG:
		if (len < sizeof(struct gsmd_msg_hdr) +
			sizeof(struct gsmd_gprs_set_config))
			return -EINVAL;
		else {
			struct gsmd_gprs_set_config* config =
				(struct gsmd_gprs_set_config *)
					((void *)gph + sizeof(struct gsmd_msg_hdr));

			sprintf(buf, "AT+CGDCONT=%d,\"IP\",\"%s\"",
				config->context, config->apn);

			cmd = atcmd_fill(buf, strlen(buf) + 1, &simple_cmd_cb, gu, gph);
		}
		break;
	default:
		return -EINVAL;
	}
	if (!cmd)
		return -ENOMEM;

	return atcmd_submit(gu->gsmd, cmd, GSMD_GPRS_DATA_CHANNEL);
}

static int usock_rcv_cancel(struct gsmd_user *gu, struct gsmd_msg_hdr *gph,
			   int len)
{
	int retval;
	struct gsmd_cancel* config;

	if (len < sizeof(struct gsmd_msg_hdr) +
		sizeof(struct gsmd_cancel))
		return -EINVAL;

	config = (struct gsmd_cancel*) ((void *)gph + sizeof(struct gsmd_msg_hdr));

	DEBUGP("usock_rcv_cancel (%d,%d,%d)\n",
		config->type, config->subtype, config->id);

	retval =
		cancel_specific_atcmd(
			gu->gsmd, config->type, config->subtype, config->id);
	return retval;
}

static int usock_rcv_test(struct gsmd_user *gu, struct gsmd_msg_hdr *gph,
			   int len)
{
	struct gsmd_atcmd *cmd = NULL;
	char buf[50]; // todo

	switch (gph->msg_subtype) {
	case  GSMD_TEST_SET_SCENARIO:
		if (!gu->gsmd->dummym_enabled) {
			gsmd_log(GSMD_ERROR, "Attempting to use test scenerio cmd with real modem\n");
			return -EINVAL;
		}
		if (len < sizeof(struct gsmd_msg_hdr) +
			sizeof(struct gsmd_test_scenario))
			return -EINVAL;
		else {
			struct gsmd_test_scenario* test_scenario =
				(struct gsmd_test_scenario *)
					((void *)gph + sizeof(struct gsmd_msg_hdr));

			sprintf(buf, "TST %d,%d,%s",
				test_scenario->type,test_scenario->subtype,test_scenario->scenario);
			cmd = atcmd_fill(buf, strlen(buf) + 1, &simple_cmd_cb, gu, gph);
		}
		break;
	case GSMD_TEST_SCHEDULE_EVENT:
		if (!gu->gsmd->dummym_enabled) {
			gsmd_log(GSMD_ERROR, "Attempting to test schedule event with real modem\n");
			return -EINVAL;
		}
		if (len < sizeof(struct gsmd_msg_hdr) +
			sizeof(struct gsmd_test_event))
			return -EINVAL;
		else {
			struct gsmd_test_event* test_event =
				(struct gsmd_test_event *)
					((void *)gph + sizeof(struct gsmd_msg_hdr));

			sprintf(buf, "TSTEV %d,%d.%d",
				test_event->event_subtype,(int) test_event->rel_time.tv_sec,(int) test_event->rel_time.tv_usec);
			cmd = atcmd_fill(buf, strlen(buf) + 1, &simple_cmd_cb, gu, gph);
		}
		break;
	default:
		return -EINVAL;
	}
	if (!cmd)
		return -ENOMEM;

	return atcmd_submit(gu->gsmd, cmd, GSMD_CMD_CHANNEL0);
}

static usock_msg_handler *pcmd_type_handlers[__NUM_GSMD_MSGS] = {
	[GSMD_MSG_PASSTHROUGH]	= &usock_rcv_passthrough,
	[GSMD_MSG_EVENT]	= &usock_rcv_event,
	[GSMD_MSG_VOICECALL]	= &usock_rcv_voicecall,
	[GSMD_MSG_PIN]		= &usock_rcv_pin,
	[GSMD_MSG_PHONE]	= &usock_rcv_phone,
	[GSMD_MSG_NETWORK]	= &usock_rcv_network,
	[GSMD_MSG_SMS]		= &usock_rcv_sms,
	[GSMD_MSG_CB]		= &usock_rcv_cb,
	[GSMD_MSG_PHONEBOOK]	= &usock_rcv_phonebook,
	[GSMD_MSG_GPRS]		= &usock_rcv_gprs,
	[GSMD_MSG_TEST]		= &usock_rcv_test,
	[GSMD_MSG_USSD]		= &usock_rcv_ussd,
	[GSMD_MSG_CANCEL_CMD]	= &usock_rcv_cancel,
};

static int usock_rcv_pcmd(struct gsmd_user *gu, char *buf, int len)
{
	struct gsmd_msg_hdr *gph = (struct gsmd_msg_hdr *)buf;
	usock_msg_handler *umh;

#if ENABLE_RERUN_LOG
	rerun_log(CtG, gph, NULL, 0);
#endif
	if (gph->version != GSMD_PROTO_VERSION) {
		gsmd_log(GSMD_ERROR, "Invalid protocol %d\n",gph->version);
		return -EINVAL;
	}
	if (gph->msg_type >= __NUM_GSMD_MSGS) {
		gsmd_log(GSMD_ERROR, "msg_type %d >= %d\n",gph->msg_type,__NUM_GSMD_MSGS);
		return -EINVAL;
	}
	umh = pcmd_type_handlers[gph->msg_type];
	if (!umh) {
		gsmd_log(GSMD_ERROR, "No msg handler\n");
		return -EINVAL;
	}
	return umh(gu, gph, len);
}

static char gsmd_usock_buf[STDIN_BUF_SIZE];

/* callback for read/write on client (libgsmd) socket */
static int gsmd_usock_user_cb(int fd, unsigned int what, void *data, u_int8_t unused)
{
	struct gsmd_user *gu = data;
	(void) unused;

	/* FIXME: check some kind of backlog and limit it */

	if (what & GSMD_FD_READ) {
		int rcvlen;
		/* read data from socket, determine what he wants */
		rcvlen = read(fd, gsmd_usock_buf, sizeof(gsmd_usock_buf));
		gsmd_log(GSMD_DEBUG, "Read %d\n",rcvlen);
		if (rcvlen == 0) {
			gsmd_log(GSMD_DEBUG, "EOF, a client has just vanished\n");
			gu->gsmd->num_of_clients--;
			if (!gu->gsmd->num_of_clients) {
				gsmd_log(GSMD_DEBUG, "Last client has vanished\n");
				shutdown_modem(gu->gsmd,NULL);
			}
#if 1
			gsmd_unregister_fd(&gu->gfd);
			close(fd);
			/* finish pending atcmd's from this client thus
			 * destroying references to the user structure.  */
			atcmd_terminate_matching(gu->gsmd, gu);
			/* destroy whole user structure */
			llist_del(&gu->list);
			llist_del(&gu->pb_readrg_list);
			llist_del(&gu->pb_find_list);
			/* removed user, so reduce max needed on free list */
			if (gu->gsmd->max_free_ucmd_hdrs >= AVER_REQ_PER_USER)
				gu->gsmd->max_free_ucmd_hdrs -= AVER_REQ_PER_USER;
			/* FIXME: delete busy ucmds from finished_ucmds */
			talloc_free(gu);
#endif
			return 0;
		} else if (rcvlen < 0) {
			return rcvlen;
		} else {

			int retval = usock_rcv_pcmd(gu, gsmd_usock_buf, rcvlen);
			if (retval < 0) {
				DEBUGP("failed to send to modem <%d>\n",retval);

				/* inform user the cmd failed to be sent to the modem */
				struct gsmd_msg_hdr *gph = (struct gsmd_msg_hdr *) gsmd_usock_buf;
				quick_response(gu, gph->msg_type, gph->msg_subtype, retval);
			}
		}
	}

	if (what & GSMD_FD_WRITE) {
		/* write data from pending replies to socket */
		struct gsmd_ucmd *ucmd, *uctmp;
		struct gsmd *g = gu->gsmd;
		llist_for_each_entry_safe(ucmd, uctmp, &gu->finished_ucmds,
					  list) {
			int rc;

#if ENABLE_RERUN_LOG
			rerun_log(GtC, &ucmd->hdr, NULL, 0);
#endif
			rc = write(fd, &ucmd->hdr, sizeof(ucmd->hdr) + ucmd->hdr.len);
			if (rc < 0) {
				DEBUGP("write return %d\n", rc);
				return rc;
			}
			if (rc == 0) {
				DEBUGP("write returns zero!!\n");
				break;
			}
			if (rc != sizeof(ucmd->hdr) + ucmd->hdr.len) {
				DEBUGP("short write\n");
				break;
			}

			DEBUGP("successfully sent cmd %p to user %p\n", ucmd, gu);
			llist_del(&ucmd->list);
			if (!ucmd->hdr.len && g->num_free_ucmd_hdrs < g->max_free_ucmd_hdrs) {
				DEBUGP("add header %p to free list\n", ucmd);
				llist_add(&ucmd->list, &g->free_ucmd_hdr);
				g->num_free_ucmd_hdrs++;
			} else {
				talloc_free(ucmd);
			}
		}
		if (llist_empty(&gu->finished_ucmds))
			gu->gfd.when &= ~GSMD_FD_WRITE;
	}

	return 0;
}

/* callback for read on master-listen-socket */
static int gsmd_usock_cb(int fd, unsigned int what, void *data, u_int8_t unused)
{
	int retval = 0;
	struct gsmd *g = data;
	struct gsmd_user *newuser;
	(void) unused;

	if (what & GSMD_FD_READ) {

		/* new incoming connection */
		DEBUGP("new connection\n");
		newuser = talloc(__gu_ctx, struct gsmd_user);
		if (!newuser)
			return -ENOMEM;

		newuser->gfd.fd = accept(fd, NULL, 0);
		if (newuser->gfd.fd < 0) {
			DEBUGP("error accepting incoming conn: `%s'\n",
				strerror(errno));
			talloc_free(newuser);
		}
		newuser->gfd.when = GSMD_FD_READ;
		newuser->gfd.data = newuser;
		newuser->gfd.cb = &gsmd_usock_user_cb;
		newuser->gsmd = g;
		newuser->subscriptions = 0xffffffff;
		INIT_LLIST_HEAD(&newuser->finished_ucmds);
		INIT_LLIST_HEAD(&newuser->pb_readrg_list);
		newuser->pb_readrg_num = 0;
		newuser->pb_readrg_status = 0;
		INIT_LLIST_HEAD(&newuser->pb_find_list);
		newuser->pb_find_num = 0;
		newuser->pb_find_status = 0;

		/* a new user, so extend the free list by a bit if required */
		if (g->max_free_ucmd_hdrs < FREE_LIST_LIMIT)
			g->max_free_ucmd_hdrs += AVER_REQ_PER_USER;
		check_free_list(g);

		llist_add(&newuser->list, &g->users);
		g->num_of_clients++;
		gsmd_register_fd(&newuser->gfd);

		if (STATUS_NOT_KNOWN != g->modem_status) {
			/* inform newly joined user of current status */
			struct gsmd_ucmd* ucmd = generate_status_event(g);
			retval = usock_cmd_enqueue(ucmd, newuser);
		}
	}

	return retval;
}

/* handling of socket with incoming client connections */
int usock_init(struct gsmd *g, unsigned int instance_num)
{
	struct sockaddr_un sun;
	int fd, rc, loop;
	
	// socket has initial \0 so extra work setting up base socket name
	const char* base_name_ptr = GSMD_UNIX_SOCKET;
	char socket_name[20];
	size_t socket_name_len;
	memset((void*)socket_name,0,20);
	if (instance_num) {
		snprintf(socket_name+1,19,"%s%d",base_name_ptr+1,instance_num);
	} else {
		snprintf(socket_name+1,19,"%s",base_name_ptr+1);
	}
	socket_name_len = strlen(socket_name+1);
	socket_name_len++;

	__ucmd_ctx = talloc_named_const(gsmd_tallocs, 1, "ucmd");
	__gu_ctx = talloc_named_const(gsmd_tallocs, 1, "gsmd_user");
	__pb_r_ctx = talloc_named_const(gsmd_tallocs, 1, "pb_read_cache");
	__pb_f_ctx = talloc_named_const(gsmd_tallocs, 1, "pb_find_cache");

	fd = socket(PF_UNIX, GSMD_UNIX_SOCKET_TYPE, 0);
	if (fd < 0)
		return fd;

	memset(&sun, 0, sizeof(sun));
	sun.sun_family = AF_UNIX;
	memcpy(sun.sun_path, socket_name, socket_name_len);

	rc = bind(fd, (struct sockaddr *)&sun, sizeof(sun));
	if (rc < 0) {
		close(fd);
		return rc;
	}

	rc = listen(fd, 10);
	if (rc < 0) {
		close(fd);
		return rc;
	}
	DEBUGP("socket ready\n");

	g->gfd_sock.fd = fd;
	g->gfd_sock.when = GSMD_FD_READ | GSMD_FD_EXCEPT;
	g->gfd_sock.data = g;
	g->gfd_sock.cb = &gsmd_usock_cb;
	g->gfd_sock.channel = 0; /* not used */

	INIT_LLIST_HEAD(&g->free_ucmd_hdr);
	g->num_free_ucmd_hdrs = 0;
	g->max_free_ucmd_hdrs = 0;

	g->pin_status = STATUS_NOT_KNOWN;
	g->sim_present = 0;
	g->sim_status = STATUS_NOT_KNOWN;
	g->modem_status = STATUS_NOT_KNOWN;
	g->network_status = GSMD_NETREG_UNREG;
	g->roaming_status = 0;
	g->rssi_idx = 0;
	g->ber_idx = 0;

	return gsmd_register_fd(&g->gfd_sock);
}
