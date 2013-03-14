/* gsmd AT command interpreter / parser / constructor
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

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <termios.h>
#include <fcntl.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <common/linux_list.h>

#include "gsmd.h"

#include <gsmd/ts0705.h>
#include <gsmd/ts0707.h>
#include <gsmd/gsmd.h>
#include <gsmd/atcmd.h>
#include <gsmd/talloc.h>
#include <gsmd/unsolicited.h>
#include <gsmd/usock.h>

static void *__atcmd_ctx, *__gph_ctx;

#define ENABLE_TIMEOUTS 1

#define DEFAULT_TIMEOUT 5
#define TIMEOUT_ERRORCODE -ECANCELED

#define MAX_SIM_INSERTED_RETRIES 5
#define SIM_INSERTED_RETRY_DELAY 2

enum final_result_codes {
	GSMD_RESULT_OK = 0,
	GSMD_RESULT_ERR = 1,
	NUM_FINAL_RESULTS,
};

static const char *final_results[] = {
	"OK",
	"ERROR",
	"+CME ERROR:",
	"+CMS ERROR:",
};

/* we basically implement a parse that can deal with
 * - receiving and queueing commands from higher level of libgmsd
 * - optionally combining them into one larger command (; appending)
 * - sending those commands to the TA, receiving and parsing responses
 * - calling back application on completion, or waiting synchronously
 *   for a response
 * - dealing with intermediate and unsolicited resultcodes by calling
 *   back into the application / higher levels
 */

static inline int llparse_append(struct llparser *llp, char byte)
{
	if (llp->cur < llp->buf + llp->len) {
		*(llp->cur++) = byte;
		return 0;
	} else {
		DEBUGP("llp->cur too big!!!\n");
		return -EFBIG;
	}
}

static inline void llparse_endline(struct llparser *llp)
{
	/* re-set cursor to start of buffer */
	llp->cur = llp->buf;
	llp->state = LLPARSE_STATE_IDLE;
	memset(llp->buf, 0, LLPARSE_BUF_SIZE);
}

static int llparse_byte(struct llparser *llp, char byte)
{
	int ret = 0;

	switch (llp->state) {
	case LLPARSE_STATE_IDLE:
	case LLPARSE_STATE_PROMPT_SPC:
		if (llp->flags & LGSM_ATCMD_F_EXTENDED) {
			if (byte == '\n')
				break;
			else if (byte == '\r')
				llp->state = LLPARSE_STATE_IDLE_CR;
			else if (byte == '>')
				llp->state = LLPARSE_STATE_PROMPT;
			else {
#ifdef STRICT
				llp->state = LLPARSE_STATE_ERROR;
#else
				llp->state = LLPARSE_STATE_RESULT;
				ret = llparse_append(llp, byte);
#endif
			}
		} else {
			llp->state = LLPARSE_STATE_RESULT;
			ret = llparse_append(llp, byte);
		}
		break;
	case LLPARSE_STATE_IDLE_CR:
		if (byte == '\n')
			llp->state = LLPARSE_STATE_IDLE_LF;
		else
			llp->state = LLPARSE_STATE_ERROR;
		break;
	case LLPARSE_STATE_IDLE_LF:
		if (byte == '\r') {
			/* can we really go directly into result_cr ? */
			DEBUGP("** IDLE_LF -> RESULT_CR? **\n");
			llp->state = LLPARSE_STATE_RESULT_CR;
		} else if (byte == '>') {
			llp->state = LLPARSE_STATE_PROMPT;
		} else {
			llp->state = LLPARSE_STATE_RESULT;
			ret = llparse_append(llp, byte);
		}
		break;
	case LLPARSE_STATE_RESULT:
		if (byte == '\r') {
			llp->state = LLPARSE_STATE_RESULT_CR;
		} else if ((llp->flags & LGSM_ATCMD_F_LFCR) && byte == '\n') {
			llp->state = LLPARSE_STATE_RESULT_LF;
		} else if ((llp->flags & LGSM_ATCMD_F_LFLF) && byte == '\n') {
			llp->state = LLPARSE_STATE_RESULT_CR;
		} else {
			if (byte == '"') 
				llp->state = LLPARSE_STATE_QUOTE;
			ret = llparse_append(llp, byte);
		}
		break;
	case LLPARSE_STATE_QUOTE:
		/* We allow line feeds (\n) in quote enclosed strings */
		if (byte == '"') {
			/* Potentially the end quote */
			llp->state = LLPARSE_STATE_RESULT;
		} else if (byte == '\r') {
			llp->state = LLPARSE_STATE_RESULT_CR;
			break;
		}
		ret = llparse_append(llp, byte);
		break;
	case LLPARSE_STATE_RESULT_CR:
		if (byte == '\n') {
			llparse_endline(llp);
		}
		break;
	case LLPARSE_STATE_RESULT_LF:
		if (byte == '\r') {
			llparse_endline(llp);
		}
		break;
	case LLPARSE_STATE_PROMPT:
		if (byte == ' ')
			llp->state = LLPARSE_STATE_PROMPT_SPC;
		else {
			/* this was not a real "> " prompt */
			llparse_append(llp, '>');
			ret = llparse_append(llp, byte);
			llp->state = LLPARSE_STATE_RESULT;
		}
		break;
	case LLPARSE_STATE_ERROR:
		break;
	}

	return ret;
}

static int llparse_string(struct llparser *llp, char *buf, unsigned int len)
{
	while (len--) {
		int rc = llparse_byte(llp, *(buf++));
		if (rc < 0)
			return rc;

		/* if _after_ parsing the current byte we have finished,
		 * let the caller know that there is something to handle */
		if (llp->state == LLPARSE_STATE_RESULT_CR) {
			/* FIXME: what to do with return value ? */
			llp->cb(llp->buf, llp->cur - llp->buf, llp->ctx, llp->channel);
		}

		/* if a full SMS-style prompt was received, poke the select */
		if (llp->state == LLPARSE_STATE_PROMPT_SPC)
			llp->prompt_cb(llp->ctx, llp->channel);
	}

	return 0;
}

static int llparse_init(struct llparser *llp)
{
	llp->state = LLPARSE_STATE_IDLE;
	return 0;
}

/* mid-level parser */

static int parse_final_result(const char *res)
{
	int i;
	for (i = 0; i < NUM_FINAL_RESULTS; i++) {
		if (!strcmp(res, final_results[i]))
			return i;
	}

	return -1;
}

static int atcmd_free(struct gsmd_atcmd *cmd)
{
	if (cmd->gph)
		talloc_free(cmd->gph);
	return talloc_free(cmd);
}

inline void atcmd_wake_pending_queue (struct gsmd *g, u_int8_t channel)
{
	if (g->dummym_enabled) {
		g->gfd_dummym[channel].when |= GSMD_FD_WRITE;
	} else {
		g->gfd_uart[channel].when |= GSMD_FD_WRITE;
	}
}

inline void atcmd_wait_pending_queue (struct gsmd *g, u_int8_t channel)
{
	if (g->dummym_enabled) {
		g->gfd_dummym[channel].when &= ~GSMD_FD_WRITE;
	} else {
		g->gfd_uart[channel].when &= ~GSMD_FD_WRITE;
	}
}

static void remove_channel_timeout(struct gsmd *g, u_int8_t channel)
{
	if (g->timeout[channel].cb)
		gsmd_timer_unregister(&g->timeout[channel]);
}

static int check_channel_cb(struct gsmd_atcmd *cmd, void *ctx, char *resp)
{
	struct gsmd *gsmd = ctx;
	DEBUGP("check_channel_cb %d\n", cmd->ret);
	if (TIMEOUT_ERRORCODE == cmd->ret)
		modem_status_changed(gsmd, TIMEOUT_ERRORCODE);
	return 0;
}

static void wait_pending_timeout(struct gsmd_timer *tmr, void *data)
{
	struct gsmd *g = data;
	struct gsmd_atcmd *cmd = NULL;
	u_int8_t channel = 0;
	int found = 0;

	for (channel = 0; channel < GSMD_MAX_CHANNELS; channel++) {
		if (&g->chl_100ms_wait[channel] == tmr) {
			found = 1;
			break;
		}
	}
	if (!found) {
		gsmd_log(GSMD_ERROR, "Unknown 100ms timeout\n");
		return;
	}
	g->chl_100ms_wait[channel].cb = NULL;

	if (!llist_empty(&g->pending_atcmds[channel])) {
		atcmd_wake_pending_queue(g,channel);
	} else {
		DEBUGP("Nothing more to send\n");
	}
}

static void wake_pending_after_delay(struct gsmd *g, u_int8_t channel,
	u_int32_t initial_delay_secs)
{
	struct timeval tv;

	if (g->chl_100ms_wait[channel].cb) {
		gsmd_log(GSMD_NOTICE, "wait already set\n");
	} else {
		if (initial_delay_secs) {
			tv.tv_sec = initial_delay_secs;
			tv.tv_usec = 0;
			gsmd_log(GSMD_NOTICE,
				"long initial delay %d secs\n",initial_delay_secs);
		} else {
			/* Siemens specify a minimum 100ms delay before
			   sending the next command in a sequence */
			tv.tv_sec = 0;
			tv.tv_usec = 100000;
		}
		if (gsmd_timer_set(&g->chl_100ms_wait[channel], &tv, &wait_pending_timeout, g)) {
			gsmd_log(GSMD_ERROR, "failed to set pending timeout\n");
			g->chl_100ms_wait[channel].cb = NULL;
		}
	}
}

static void cancel_pending_timeout(struct gsmd *g, u_int8_t channel)
{
	if (g->chl_100ms_wait[channel].cb) {
		gsmd_log(GSMD_NOTICE, "cancelled 100ms wait\n");
		gsmd_timer_unregister(&g->chl_100ms_wait[channel]);
		g->chl_100ms_wait[channel].cb = NULL;
	}
}

static void check_channel(struct gsmd *gsmd, u_int8_t channel)
{
	struct gsmd_atcmd *cmd = atcmd_fill("AT", 3, &check_channel_cb, gsmd, NULL);
	if (cmd)
		atcmd_submit_highpriority(gsmd, cmd, channel);
}

static int atcmd_done(struct gsmd *g, struct gsmd_atcmd *cmd,
	const char *buf, u_int8_t channel)
{
	int rc = 0;
#if ENABLE_TIMEOUTS
	remove_channel_timeout(g, channel);
#endif
	if (!cmd) {
		gsmd_log(GSMD_ERROR, "* Null cmd? *\n");
		return -1;
	}

	if (!cmd->cb) {
		gsmd_log(GSMD_NOTICE, "command without cb!!!\n");
	} else {
		cmd->flags = ATCMD_FINAL_CB_FLAG;
		/* send final result code if there is no information
		 * response in mlbuf */
		if (g->mlbuf_len[channel]) {
			cmd->resp = g->mlbuf[channel];
			cmd->resplen = g->mlbuf_len[channel];
			cmd->resp[cmd->resplen] = 0;
		} else {
			cmd->resp = (char*) buf;
			cmd->resplen = strlen(buf);
		}
		DEBUGP("Calling final cmd->cb() %d <%s>(%d) <%d>\n",
			g->mlbuf_len[channel],cmd->resp,cmd->resplen,cmd->ret);
		rc = cmd->cb(cmd, cmd->ctx, cmd->resp);
		if (rc < 0) {
			gsmd_log(GSMD_ERROR, "Failed to create response for client\n");
		}
		DEBUGP("Clearing mlbuf\n");
		g->mlbuf_len[channel] = 0;
		g->mlbuf[channel][0] = 0;
	}

	/* remove from list of currently executing cmds */
	llist_del(&cmd->list);
#if ENABLE_TIMEOUTS
	if (TIMEOUT_ERRORCODE == cmd->ret && STATUS_OK == g->modem_status)
		check_channel(g,channel);
#endif
	atcmd_free(cmd);

	/* We're finished with the current command, but if still have pending
	* command(s) then pop off the first pending */
	if (llist_empty(&g->busy_atcmds[channel])) {
		struct gsmd_atcmd *cur = NULL;
		u_int32_t initial_delay_secs = 0;
		if (!llist_empty(&g->pending_atcmds[channel])) {
			gsmd_log(GSMD_INFO, "cmds pending\n");
			cur = llist_entry(g->pending_atcmds[channel].next,
				struct gsmd_atcmd, list);
			if (cur) {
				initial_delay_secs = cur->initial_delay_secs;
			} else {
				gsmd_log(GSMD_ERROR, "First pending is null?\n");
			}
		}
		if (g->pin_status) {
			if (cur) {
				if (cur->flags & ATCMD_PIN_SENSITIVE) {
					gsmd_log(GSMD_INFO, "pin_status %d\n",g->pin_status);
					if (g->sim_status == GSM0707_CME_SIM_NOT_INSERTED) {
						gsmd_log(GSMD_INFO, "sim not inserted\n");
						/* allow the modem to fail the cmd */
						wake_pending_after_delay(
							g,channel,initial_delay_secs);
					} else {
						gsmd_log(GSMD_INFO, "* pin sensitive cmd delayed *\n");
						g->pin_sensitive_cmds_waiting = 1;
					}
				} else {
					gsmd_log(GSMD_INFO, "wake pending after %d\n", initial_delay_secs);
					wake_pending_after_delay(g,channel,initial_delay_secs);
				}
			}
		} else {
			if (g->pin_sensitive_cmds_waiting) {
				if (cur && (cur->flags & ATCMD_PIN_SENSITIVE)) {
					u_int8_t ch_iter = 0;
					gsmd_log(GSMD_INFO, "chk chnls for pin delayed cmds\n");
					for (ch_iter = 0; ch_iter < g->number_channels; ch_iter++) {
						if (ch_iter == channel)
							continue;
						if (!llist_empty(&g->pending_atcmds[ch_iter])) {
							struct gsmd_atcmd *cur2 =
								llist_entry(g->pending_atcmds[ch_iter].next,
									struct gsmd_atcmd, list);
							if (cur2 && (cur2->flags & ATCMD_PIN_SENSITIVE)) {
								gsmd_log(GSMD_INFO, "* waking chnl %d *\n",
									ch_iter);
								wake_pending_after_delay(
									g, ch_iter, initial_delay_secs);
							}
						}
					}
					g->pin_sensitive_cmds_waiting = 0;
				}
			}

			gsmd_log(GSMD_INFO, "wake pending after %d secs\n", initial_delay_secs);
			wake_pending_after_delay(g, channel,initial_delay_secs);
		}
	}
	return rc;
}

#if ENABLE_TIMEOUTS
static void channel_timeout(struct gsmd_timer *tmr, void *data)
{
	struct gsmd *g = data;
	struct gsmd_atcmd *cmd = NULL;
	u_int8_t channel = 0;
	int found = 0;

	for (channel = 0; channel < GSMD_MAX_CHANNELS; channel++) {
		if (&g->timeout[channel] == tmr) {
			found = 1;
			break;
		}
	}
	if (!found) {
		gsmd_log(GSMD_ERROR, "Unknown timeout\n");
		return;
	}
	gsmd_log(GSMD_DEBUG, "channel_timeout chnl %d\n", channel, tmr);
	g->timeout[channel].cb = NULL;

	if (!llist_empty(&g->busy_atcmds[channel]))
		cmd = llist_entry(g->busy_atcmds[channel].next,
				  struct gsmd_atcmd, list);

	if (cmd) {
		gsmd_log(GSMD_DEBUG, "Cancelling cmd chnl %d\n", channel);
		cmd->flags = 0;
		cmd->ret = TIMEOUT_ERRORCODE;
		atcmd_done(g, cmd, "TIMEOUT", channel);
	}
}
#endif

static void sim_inserted_retry_timeout(struct gsmd_timer *tmr, void *data)
{
	struct gsmd *g = data;

	DEBUGP("do sim retry\n");
	g->sim_inserted_retry_count++;

	gsmd_timer_free(g->sim_inserted_retry_timer);
	g->sim_inserted_retry_timer=NULL;

	gsmd_initsettings2(g);
}

static int ml_parse(const char *buf, int len, void *ctx, u_int8_t channel)
{
	struct gsmd *g = ctx;
	struct gsmd_atcmd *cmd = NULL;
	int rc = 0;

	DEBUGP("chnl %d buf=`%s'(%d)\n", channel, buf, len);

	/* FIXME: This needs to be part of the vendor plugin. If we receive
	 * an empty string or that 'ready' string, we need to init the modem */
	if (buf && !strcmp(buf, "^SYSSTART")) {
		g->interpreter_ready = 1;
		g->sim_inserted_retry_count = 0;
		if (g->sim_inserted_retry_timer) {
			DEBUGP("reset sim retry\n");
			gsmd_timer_unregister(g->sim_inserted_retry_timer);
			gsmd_timer_free(g->sim_inserted_retry_timer);
			g->sim_inserted_retry_timer=NULL;
		}
		cancel_all_atcmds(g);
		if (STATUS_OK == g->modem_status) {
			struct gsmd_ucmd* status_event;
			DEBUGP("reset status\n");
			g->pin_status = STATUS_NOT_KNOWN;
			g->sim_present = 0;
			g->sim_status = STATUS_NOT_KNOWN;
			g->modem_status = STATUS_NOT_KNOWN;
			g->network_status = GSMD_NETREG_UNREG;
			g->roaming_status = 0;
			g->rssi_idx = 0;
			g->ber_idx = 0;
			status_event = generate_status_event(g);
			usock_evt_send(g, status_event, GSMD_EVT_STATUS);
		}
		gsmd_initsettings2(g);
		return 0;
	}

	/* TCL SL40 specific SIM problem - unknown cause
	 * Retry a certain number of times with a delay between each retry */
	if (buf && !strcmp(buf, "+CME ERROR: 10")) {
		if (!g->sim_inserted_retry_timer && g->sim_inserted_retry_count < MAX_SIM_INSERTED_RETRIES) {
			struct timeval tv;
			tv.tv_sec = SIM_INSERTED_RETRY_DELAY;
			tv.tv_usec = 0;
			DEBUGP("no sim inserted error (retry %d)\n",g->sim_inserted_retry_count);
			g->interpreter_ready = 1;
			cancel_all_atcmds(g);
			g->sim_inserted_retry_timer = gsmd_timer_create(&tv,&sim_inserted_retry_timeout,g);
			if (!g->sim_inserted_retry_timer) {
				gsmd_log(GSMD_ERROR, "failed to create timeout\n");
			} else {
				return 0;
			}
		}
	}

	/* responses come in order, so first response has to be for first
	 * command we sent, i.e. first entry in list */
	if (!llist_empty(&g->busy_atcmds[channel]))
		cmd = llist_entry(g->busy_atcmds[channel].next,
				  struct gsmd_atcmd, list);

	if (cmd) {
		cmd->flags = 0;
		cmd->ret = 0;
	}

	if (cmd && !strcmp(buf, cmd->buf)) {
		DEBUGP("ignoring echo\n");
		return 0;
	}

	/* we have to differentiate between the following cases:
	 *
	 * A) an information response ("+whatever: ...")
	 *    we just pass it up the callback
	 * B) an unsolicited message ("+whateverelse: ... ")
	 *    we call the unsolicited.c handlers
	 * C) a final response ("OK", "+CME ERROR", ...)
	 *    in this case, we need to check whether we already sent some
	 *    previous data to the callback (information response).  If yes,
	 *    we do nothing.  If no, we need to call the callback.
	 * D) an intermediate response ("CONNECTED", "BUSY", "NO DIALTONE")
	 *    TBD
	 */

	if (buf[0] == '+' || strchr(g->vendorpl->ext_chars, buf[0])) {
		/* an extended response */
		const char *colon = strchr(buf, ':');
		if (!colon) {
			gsmd_log(GSMD_ERROR, "no colon in extd response `%s'\n",
				buf);
			return -EINVAL;
		}
		if (!strncmp(buf+1, "CME ERROR", 9)) {
			/* Part of Case 'C' */
			unsigned long err_nr;
			err_nr = strtoul(colon+1, NULL, 10);
			DEBUGP("cme error number %lu\n", err_nr);
			if (cmd)
				cmd->ret = -err_nr;
			generate_event_from_cme(g, err_nr);

			goto final_cb;
		}
		if (!strncmp(buf+1, "CMS ERROR", 9)) {
			/* Part of Case 'C' */
			unsigned long err_nr;
			err_nr = strtoul(colon+1, NULL, 10);
			DEBUGP("cms error number %lu\n", err_nr);
			if (cmd)
				cmd->ret = -err_nr;
			generate_event_from_cms(g, err_nr);

			goto final_cb;
		}

		if (!cmd || strncmp(buf, &cmd->buf[2], colon-buf)) {
			/* Assuming Case 'B' */
			DEBUGP("extd reply `%s' to cmd `%s', must be "
				   "unsolicited\n", buf, cmd ? &cmd->buf[2] : "NONE");
			colon++;
			if (colon > buf+len)
				colon = NULL;
			rc = unsolicited_parse(g, (char*) buf, len, colon, channel);
			if (rc == -EAGAIN) {
				/* The parser wants one more line of
				 * input.  Wait for the next line, concatenate
				 * and resumbit to unsolicited_parse().  */
				DEBUGP("Multiline unsolicited code\n");
				g->mlbuf_len[channel] = len;
				memcpy(g->mlbuf[channel], buf, len);
				g->mlunsolicited[channel] = 1;
				return 0;
			}
			/* if unsolicited parser didn't handle this 'reply', then we
			 * need to continue and try harder and see what it is */
			if (rc != -ENOENT) {
				/* Case 'B' finished */
				return rc;
			}
			/* continue, not 'B' */
		}

		if (cmd) {
			if (cmd->buf[2] != '+' && strchr(g->vendorpl->ext_chars, cmd->buf[2]) == NULL) {
				gsmd_log(GSMD_ERROR, "extd reply to non-extd command?\n");
				return -EINVAL;
			}

			/* if we survive till here, it's a valid extd response
			 * to an extended command and thus Case 'A' */

			/* it might be a multiline response, so if there's a previous
			   response, send out mlbuf and start afresh with an empty buffer */
			if (g->mlbuf_len[channel]) {
				if (!cmd->cb) {
					gsmd_log(GSMD_NOTICE, "command without cb!!!\n");
				} else {
					cmd->resp = g->mlbuf[channel];
					cmd->resplen = g->mlbuf_len[channel];
					cmd->resp[cmd->resplen] = 0;
					DEBUGP("Multiline response. Calling cmd->cb() <%s>(%d) <%d>\n",
						cmd->resp,cmd->resplen,cmd->ret);
					rc = cmd->cb(cmd, cmd->ctx, cmd->resp);
					DEBUGP("Clearing mlbuf\n");
				}
				g->mlbuf_len[channel] = 0;
				g->mlbuf[channel][0] = 0;
			}

			/* the current buf will be appended to mlbuf below */
		}
	} else {
		if (!strcmp(buf, "RING") ||
			((g->flags & GSMD_FLAG_V0) && buf[0] == '2')) {
			/* this is the only non-extended unsolicited return
			 * code, part of Case 'B' */
			return unsolicited_parse(g, (char*) buf, len, NULL, channel);
		}

		if (!strcmp(buf, "ERROR") ||
			((g->flags & GSMD_FLAG_V0) && buf[0] == '4')) {
			/* Part of Case 'C' */
			DEBUGP("unspecified error\n");
			if (cmd)
				cmd->ret = -4; // TODO magic number
			goto final_cb;
		}

		if (!strncmp(buf, "OK", 2) ||
			((g->flags & GSMD_FLAG_V0) && buf[0] == '0')) {
			/* Part of Case 'C' */
			if (cmd)
				cmd->ret = 0;
			goto final_cb;
		}

		if (!strncmp(buf, "CONNECT",7)) {
			/* Part of Case 'C' */
			if (cmd)
				cmd->ret = 0;
			goto final_cb;
		}

		/* FIXME: handling of those special commands in response to
		 * ATD / ATA */
		if (!strncmp(buf, "NO CARRIER", 10) ||
			((g->flags & GSMD_FLAG_V0) && buf[0] == '3')) {
			/* Part of Case 'D' */
			if (GSMD_GPRS_DATA_CHANNEL == channel) {
				DEBUGP("received no carrier on data channel\n");
				return unsolicited_parse(g, (char*) buf, len, NULL, channel);
			}
			goto final_cb;
		}

		if (!strncmp(buf, "BUSY", 4) ||
			((g->flags & GSMD_FLAG_V0) && buf[0] == '7')) {
			/* Part of Case 'D' */
			goto final_cb;
		}
	}

	/* we reach here, if we are at an information response that needs to be
	 * passed on */

	if (g->mlbuf_len[channel])
		g->mlbuf[channel][g->mlbuf_len[channel]++] = '\n';
	DEBUGP("Appending buf len %d to mlbuf %d\n",len,g->mlbuf_len[channel]);
	if (len > MLPARSE_BUF_SIZE - g->mlbuf_len[channel]) {
		len = MLPARSE_BUF_SIZE - g->mlbuf_len[channel];
		gsmd_log(GSMD_NOTICE, "g->mlbuf[%d] overrun\n",channel);
	}
	memcpy(g->mlbuf[channel] + g->mlbuf_len[channel], buf, len);
	g->mlbuf_len[channel] += len;

	if (g->mlunsolicited[channel]) {
		g->mlbuf[channel][g->mlbuf_len[channel]] = 0;
		rc = unsolicited_parse(g, g->mlbuf[channel], g->mlbuf_len[channel],
				strchr(g->mlbuf[channel], ':') + 1, channel);
		if (rc == -EAGAIN) {
			/* The parser wants one more line of
			 * input.  Wait for the next line, concatenate
			 * and re-submit to unsolicited_parse().  */
			DEBUGP("Multiline unsolicited code\n");
			return 0;
		}
		g->mlunsolicited[channel] = 0;
		g->mlbuf_len[channel] = 0;
		return rc;
	}
	return 0;

final_cb:
	/* if reach here, the final result code of a command has been reached */

	if (!cmd) {
		gsmd_log(GSMD_NOTICE, "No cmd cb\n");
		return rc;
	}

	return atcmd_done(g, cmd, buf, channel);
}

/* called when the modem asked for a new line of a multiline atcmd */
static int atcmd_prompt(void *data, u_int8_t channel)
{
	struct gsmd *g = data;
	DEBUGP("atcmd_prompt\n");

	usleep(20000); // 20ms Jim hack for 549 cms error, too quick a reply after prompt
	atcmd_wake_pending_queue(g,channel);
	return 0;
}

static int dummym_write(int fd, struct gsmd_atcmd *cmd, int len)
{
	int rc = len;

	struct dummym_data {
		int total_len;
		struct gsmd_msg_hdr hdr;
	} __attribute__ ((packed));
	struct dummym_data* out = NULL;

	int out_len = sizeof(struct dummym_data) + len;
	if (cmd->gph) {
		out_len += cmd->gph->len;
	}
	out = malloc(out_len);

	if (out) {
		int idx = 0;

		memset(out, 0, out_len);
		out->total_len = out_len;

		DEBUGP("out_len %d\n",out_len);

		if (cmd->gph) {
			DEBUGP("type %d subtype %d\n",cmd->gph->msg_type, cmd->gph->msg_subtype);
			DEBUGP("gph len %d\n",cmd->gph->len);
			memcpy(&out->hdr, cmd->gph, sizeof(struct gsmd_msg_hdr) + cmd->gph->len);
			idx = cmd->gph->len;
		}
		memcpy(&out->hdr.data[idx], cmd->cur, len);
		if (write(fd, out, out_len) < 0) {
			DEBUGP("write failed\n");
		}

		free(out);

	} else {
		rc = 0;
	}
	return rc;
}

/* callback to be called if [virtual] UART has some data for us */
static int atcmd_select_cb(int fd, unsigned int what, void *data, u_int8_t channel)
{
	int len, rc;
	static char rxbuf[1024];
	struct gsmd *g = data;
	char *cr;

	if (what & GSMD_FD_READ) {
		memset(rxbuf, 0, sizeof(rxbuf));
		while ((len = read(fd, rxbuf, sizeof(rxbuf)))) {
			if (len < 0) {
				if (errno == EAGAIN)
					return 0;
				gsmd_log(GSMD_NOTICE, "ERROR reading from fd %u: %d (%s)\n", fd, len,
					strerror(errno));
					return len;
			}
			if (g->suspended) {
				gsmd_log(GSMD_DEBUG, "Suspended state - received \"%s\" (%d)\n",rxbuf,len);
			} else {
#if ENABLE_RERUN_LOG
				rerun_log(MtG, NULL, rxbuf, len);
#endif
				rc = llparse_string(&g->llp[channel], rxbuf, len);
				if (rc < 0) {
					gsmd_log(GSMD_ERROR, "ERROR during llparse_string: %d\n", rc);
					return rc;
				}
			}
		}
	}

	/* write pending commands to UART */
	if ((what & GSMD_FD_WRITE) && g->interpreter_ready) {
		struct gsmd_atcmd *pos, *pos2;
		llist_for_each_entry_safe(pos, pos2, &g->pending_atcmds[channel], list) {
			cr = strchr(pos->cur, '\n');
			if (cr)
				len = cr - pos->cur;
			else
				len = pos->buflen - 1;  /* assuming zero-terminated strings */

			if (len > 1024) {
				gsmd_log(GSMD_ERROR, "Corrupt len %d > 1024 ?\n",len);
				break;
			}
			if (pos->gph && pos->gph->len > 1024) {
				gsmd_log(GSMD_ERROR, "Corrupt gph->len %d > 1024 ?\n",pos->gph->len);
				break;
			}

			pos->cur[len] = '\r';
#if ENABLE_RERUN_LOG
			rerun_log(GtM, pos->gph, pos->cur, len+1);
#endif

			if (g->dummym_enabled) {
				rc = dummym_write(fd, pos, len+1);
			} else {
				rc = write(fd, pos->cur, len+1);
			}
			if (rc == 0) {
				gsmd_log(GSMD_ERROR, "write returns 0, aborting\n");
				break;
			} else if (rc < 0) {
				gsmd_log(GSMD_ERROR, "error during write to fd %d: %d\n",
					fd, rc);
				return rc;
			}
			rc--;

			if (!cr || rc == len)
				rc ++;	/* Skip the \n or \0 */
			pos->buflen -= rc;
			pos->cur += rc;

			if (!pos->buflen) {
				/* success: remove from global list of
				 * to-be-sent atcmds */
				llist_del(&pos->list);
				/* append to global list of executing atcmds */
				llist_add_tail(&pos->list, &g->busy_atcmds[channel]);
#if ENABLE_TIMEOUTS
				g->timeout[channel].cb = NULL;
				if (pos->timeout_value) {
					struct timeval tv;
					tv.tv_sec = pos->timeout_value;
					tv.tv_usec = 0;

					if (gsmd_timer_set(&g->timeout[channel], &tv, &channel_timeout, g)) {
						gsmd_log(GSMD_ERROR, "failed to set timeout\n");
						g->timeout[channel].cb = NULL;
					}

					gsmd_log(GSMD_DEBUG, "chnl %d timeout in %d secs\n",channel,pos->timeout_value);
				}
#endif

				/* we only send one cmd */
				break;
			} else {
				/* The write was short or the atcmd has more
				 * lines to send after a "> ".  */
				if (rc < len)
					return 0;
				break;
			}
		}

		/* Either pending_atcmds is empty or a command has to wait */
		atcmd_wait_pending_queue(g,channel);
	}

	return 0;
}


struct gsmd_atcmd *atcmd_fill(const char *cmd, int rlen,
				  atcmd_cb_t cb, void *ctx, struct gsmd_msg_hdr* gph)
{
	int buflen = strlen(cmd);
	struct gsmd_atcmd *atcmd;

	if (rlen > buflen)
		buflen = rlen;

	atcmd = talloc_size(__atcmd_ctx, sizeof(*atcmd)+ buflen);
	if (!atcmd)
		return NULL;

	atcmd->id = 0;
	atcmd->gph = NULL;

	if (gph) {
		int len = sizeof(struct gsmd_msg_hdr) + gph->len;
		if (len < 1025) {
			atcmd->gph = talloc_size(__gph_ctx, len);
			if (atcmd->gph)
				memcpy(atcmd->gph, gph, len);
			atcmd->id = gph->id;
		} else {
			gsmd_log(GSMD_ERROR, "Corrupt len %d > 1024 ?\n",len);
		}
	}

	atcmd->ctx = ctx;
	atcmd->flags = 0;
	atcmd->timeout_value = DEFAULT_TIMEOUT;
	atcmd->initial_delay_secs = 0;
	atcmd->cmd_retries = 0;
	atcmd->ret = 0;
	atcmd->buflen = buflen;
	atcmd->buf[buflen-1] = '\0';
	atcmd->cur = atcmd->buf;
	atcmd->cb = cb;
	atcmd->resp = NULL;
	atcmd->resplen = 0;

	/* TODO if buflen = rlen, then strncpy is copying too much */
	strncpy(atcmd->buf, cmd, buflen-1);

	return atcmd;
}

#if 0
static int null_wakeup_cb(struct gsmd_atcmd *cmd, void *ctx, char *resp)
{
	struct gsmd *g = ctx;
	if (g->wakeup_timer) {
		DEBUGP("modem is awake, remove timer!\n");
		gsmd_timer_unregister(g->wakeup_timer);
		gsmd_timer_free(g->wakeup_timer);
		g->wakeup_timer=NULL;
	} else {
		DEBUGP("ERROR!! The wake up response comes too late!!\n");
	}
	return 0;
}

static void wakeup_timeout(struct gsmd_timer *tmr, void *data)
{
	struct gsmd *g=data;
	struct gsmd_atcmd *cmd=NULL;
	DEBUGP("Wakeup time out!!\n");
	if (g->wakeup_timer != tmr) {
		DEBUGP("ERROR!! g->wakeup_timer != tmr\n");
		return;
	}
	gsmd_timer_free(g->wakeup_timer);
	g->wakeup_timer=NULL;
	if (!llist_empty(&g->busy_atcmds[GSMD_CMD_CHANNEL0])) {
		cmd = llist_entry(g->busy_atcmds[GSMD_CMD_CHANNEL0].next,struct gsmd_atcmd, list);
	}
	if (!cmd) {
		DEBUGP("ERROR!! busy_atcmds is NULL\n");
		return;
	}
	// It's a wakeup command
	if ( cmd->buf[0]==' ') {
		llist_del(&cmd->list);
		atcmd_free(cmd);
		// discard the wakeup command, and pass the real command.
	} else {
		DEBUGP("ERROR!! Wakeup timeout and cmd->buf is not wakeup command!! %s\n",cmd->buf);
	}
	if (llist_empty(&g->busy_atcmds[GSMD_CMD_CHANNEL0]) &&
		!llist_empty(&g->pending_atcmds[GSMD_CMD_CHANNEL0])) {
		atcmd_wake_pending_queue(g,GSMD_CMD_CHANNEL0);
	}
}

void wakeup_timer (struct gsmd *g)
{
	struct timeval tv;
	struct gsmd_timer *timer;
	tv.tv_sec = GSMD_MODEM_WAKEUP_TIMEOUT;
	tv.tv_usec = 0;
	timer=gsmd_timer_create(&tv,&wakeup_timeout,g);
	g->wakeup_timer=timer;
}

/// adding a null '\r' before real at command.
struct gsmd_atcmd * atcmd_wakeup_modem(struct gsmd *g)
{
	if (!g->wakeup_timer) {
		DEBUGP("try to wake up\n");
		struct gsmd_atcmd * cmd= atcmd_fill(" \r",2,&null_wakeup_cb,g,NULL);
		//wakeup_timer(g);
		if (llist_empty(&g->pending_atcmds[GSMD_CMD_CHANNEL0])) {
			atcmd_wake_pending_queue(g,GSMD_CMD_CHANNEL0);
		}
		llist_add_tail(&cmd->list, &g->pending_atcmds[GSMD_CMD_CHANNEL0]);
	}
}
#endif

/* submit an atcmd in the global queue of pending atcmds */
int atcmd_submit(struct gsmd *g, struct gsmd_atcmd *cmd, u_int8_t channel)
{
	int empty;
	//if (cmd->flags & ATCMD_WAKEUP_MODEM)
	//	atcmd_wakeup_modem(g);

	empty = llist_empty(&g->pending_atcmds[channel]);
	llist_add_tail(&cmd->list, &g->pending_atcmds[channel]);
	if (llist_empty(&g->busy_atcmds[channel])) {
		DEBUGP("chnl %d is free\n", channel);
		if (empty) {
			DEBUGP("pending list is empty, submit cmd `%s'\n", cmd->buf);
			if (cmd->initial_delay_secs) {
				/* command requires an initial delay before execution */
				cancel_pending_timeout(g, channel);
				wake_pending_after_delay(g, channel,cmd->initial_delay_secs);
			} else if (g->chl_100ms_wait[channel].cb) {
				/* pending list will be dealt with by the 100ms timeout */
			} else {
				atcmd_wake_pending_queue(g,channel);
			}
		} else {
			// presumably 2 part cmd (requiring prompt) or a number of cmds
			// added at the same time (during initialisation or reset)
			gsmd_log(GSMD_NOTICE, "Busy empty, pending !empty, added `%s'\n", cmd->buf);
		}
	} else {
		DEBUGP("chnl %d, cmd `%s' added to pending cmds\n", channel, cmd->buf);
	}
	return 0;
}

int atcmd_submit_highpriority(struct gsmd *g, struct gsmd_atcmd *cmd, u_int8_t channel)
{
	int empty;

	// TODO maybe potential problem here if insert cmd before pdu part of a 2 part sms send
	empty = llist_empty(&g->pending_atcmds[channel]);
	llist_add(&cmd->list, &g->pending_atcmds[channel]);
	if (llist_empty(&g->busy_atcmds[channel])) {
		DEBUGP("chnl %d is free\n", channel);
		if (empty) {
			DEBUGP("pending list is empty, submit high priority cmd `%s'\n", cmd->buf);
		} else {
			gsmd_log(GSMD_NOTICE,
				"Busy empty, pending !empty, added high pri `%s'\n", cmd->buf);
		}
		if (cmd->initial_delay_secs) {
			/* command requires an initial delay before execution */
			cancel_pending_timeout(g, channel);
			wake_pending_after_delay(g, channel,cmd->initial_delay_secs);
		} else if (g->chl_100ms_wait[channel].cb) {
			/* pending list will be dealt with by the 100ms timeout */
		} else {
			atcmd_wake_pending_queue(g,channel);
		}
	} else {
		DEBUGP("chnl %d, high priority cmd `%s' added to pending cmds\n", channel, cmd->buf);
	}
	return 0;
}

/* cancel a currently executing atcmd by issuing the command given as
 * parameter, usually AT or ATH.  */
int cancel_current_atcmd(struct gsmd *g, struct gsmd_atcmd *cmd, u_int8_t channel)
{
	struct gsmd_atcmd *cur;
	if (llist_empty(&g->busy_atcmds[channel])) {
		return atcmd_submit(g, cmd, channel);
	}
	cur = llist_entry(g->busy_atcmds[channel].next, struct gsmd_atcmd, list);
	DEBUGP("cancelling command `%s' with an `%s'\n", cur->buf, cmd->buf);

	if (g->mlbuf_len[channel]) {
		DEBUGP("Discarding mlbuf: %.*s\n", g->mlbuf_len[channel], g->mlbuf[channel]);
		g->mlbuf_len[channel] = 0;
	}

	llist_add(&cmd->list, &g->pending_atcmds[channel]);
	return atcmd_done(g, cur, "OK", channel);
}

int cancel_specific_atcmd(struct gsmd *g, int type, int subtype, int id)
{
	struct gsmd_atcmd *cmd, *pos;
	u_int8_t channel;

	DEBUGP("cancel_specific_atcmd (%d,%d,%d)\n", type, subtype, id);

	for (channel = GSMD_CMD_CHANNEL0; channel < g->number_channels; channel++){

		llist_for_each_entry_safe(cmd, pos, &g->pending_atcmds[channel], list){
			if (cmd->gph) {
				if ((cmd->gph->msg_type == type)&&
					(cmd->gph->msg_subtype == subtype)&&
					(cmd->gph->id == id)) {
					gsmd_log(GSMD_DEBUG, "Removing cmd (%d,%d,%d)"
						" from pending queue\n", type,subtype,id);

					llist_del(&cmd->list);
					cmd->ret = -ECANCELED;
					cmd->cb(cmd, cmd->ctx, " ");
					atcmd_free(cmd);
					return 0;
				}
			}
		}

		/* TODO there should not be more than one atcmd busy */
		llist_for_each_entry_safe(cmd, pos, &g->busy_atcmds[channel], list) {
			if (cmd->gph) {
				if ((cmd->gph->msg_type == type)&&
					(cmd->gph->msg_subtype == subtype)&&
					(cmd->gph->id == id)) {
					struct gsmd_atcmd *cancel_cmd;
					gsmd_log(GSMD_DEBUG, "Cancelling active cmd (%d,%d,%d)\n",
						type,subtype,id);

					/* free up busy_atcmds for AT cmd but don't do callback yet */
					llist_del(&cmd->list);
					remove_channel_timeout(g, channel);

					/* callback will happen on receiving result of AT cancel */
					cancel_cmd = atcmd_fill("AT", 3, cmd->cb, cmd->ctx, cmd->gph);
					cancel_cmd->ret = -ECANCELED;

					return atcmd_submit_highpriority(g, cancel_cmd, channel);
				}
			}
		}
	}
	/* could not cancel - most likely already completed */
	return -EINVAL;
}

void cancel_pending_atcmds(struct gsmd *g, u_int8_t channel)
{
	struct gsmd_atcmd *cmd, *pos;
	llist_for_each_entry_safe(cmd, pos, &g->pending_atcmds[channel], list){
		llist_del(&cmd->list);
		if (cmd->gph) {
			gsmd_log(GSMD_DEBUG, "Removing pending cmd (%d,%d,%d) chl %d\n",
				cmd->gph->msg_type,cmd->gph->msg_subtype,cmd->gph->id,channel);

			cmd->ret = -ECANCELED;
			if (cmd->cb)
				cmd->cb(cmd, cmd->ctx, " ");
		}
		atcmd_free(cmd);
	}
}

void cancel_all_atcmds(struct gsmd *g)
{
	/* The modem has been lost, cancel all cmds */
	struct gsmd_atcmd *cmd, *pos;
	u_int8_t channel;

	DEBUGP("cancel_all_atcmds\n");

	cleanup_sim_busy(g, -ECANCELED);

	for (channel = GSMD_CMD_CHANNEL0; channel < g->number_channels; channel++){

		cancel_pending_atcmds(g, channel);

		/* TODO there should not be more than one atcmd busy */
		llist_for_each_entry_safe(cmd, pos, &g->busy_atcmds[channel], list) {
			llist_del(&cmd->list);
			remove_channel_timeout(g, channel);
			if (cmd->gph) {
				gsmd_log(GSMD_DEBUG, "Removing active cmd (%d,%d,%d)\n",
					cmd->gph->msg_type,cmd->gph->msg_subtype,cmd->gph->id);

				cmd->ret = -ECANCELED;
				if (cmd->cb)
					cmd->cb(cmd, cmd->ctx, " ");
			}
			atcmd_free(cmd);
		}

		/* ensure that pending wait is cancelled if active for the channel */
		cancel_pending_timeout(g, channel);
	}
}

void atcmd_drain(int fd)
{
	int rc;
	struct termios t;
	rc = tcflush(fd, TCIOFLUSH);
	rc = tcgetattr(fd, &t);
	DEBUGP("c_iflag = 0x%08x, c_oflag = 0x%08x, c_cflag = 0x%08x, c_lflag = 0x%08x\n",
		t.c_iflag, t.c_oflag, t.c_cflag, t.c_lflag);
	t.c_iflag = t.c_oflag = 0;
	cfmakeraw(&t);
	rc = tcsetattr(fd, TCSANOW, &t);
}

int atcmd_mux(struct gsmd *g, int ttyfd)
{
	u_int8_t channel;
	int retval;
	char buf[101];
	char cmux[] = "AT+CMUX=0\r";
	int finish = 0;

	retval = write(ttyfd, "\r", 1);
	if (retval < 0) {
		fprintf(stderr,"initial write failed %d",retval);
	}
	sleep(1);
	atcmd_drain(ttyfd);

	for (channel = GSMD_CMD_CHANNEL0; channel < g->number_channels; channel++)
		g->gfd_uart[channel].fd = 0;

	/* simplistic parsing for result of cmux cmd */
	retval = write(ttyfd, cmux, strlen(cmux));
	if (retval < 0) {
		fprintf(stderr,"write failed %d",retval);
	} else {
		int retry_count = 0;
		char input_buf[31];
		int cmux_echoed = 0;
	
		fd_set rfds;
		struct timeval tv;
		int maxfd = ttyfd + 1;
		int check, rdlen;

		buf[0] = '\0';

		while (retry_count < 15) {

			FD_ZERO(&rfds);
			FD_SET(ttyfd, &rfds);
			tv.tv_sec = 1;
			tv.tv_usec = 0;

			// TODO
			// In certain circumstances the CPU on both PC & Arm goes to 100% unless
			// have this 1ms sleep here - reason why is currently unclear
			usleep(1000);
		
			retval = select(maxfd, &rfds, NULL, NULL, &tv);
			if (retval == 0) {
				retry_count++;
				if (!cmux_echoed) {
					/* read timed out - reissue cmux */
					retval = write(ttyfd, cmux, strlen(cmux));
					if (retval < 0) {
						fprintf(stderr,"write failed %d",retval);
						break;
					}
				}
				continue;
			}
			if (FD_ISSET(ttyfd, &rfds)) {
				rdlen=read(ttyfd,input_buf,30);
				if (rdlen) {
					char* endptr = strchr(input_buf,'\r');
					input_buf[rdlen] = '\0';
					DEBUGP("input_buf %d <%s>\n",rdlen,input_buf);
					if (endptr) {
						char* startptr = input_buf;
						char* newptr = NULL;
						while (endptr) {
							*endptr = '\0';
							newptr = ++endptr;
							if (strlen(buf) + strlen(startptr) > 100) {
								DEBUGP("buf overrun\n");
								buf[0] = '\0';
							}
							strcat(buf,startptr);
							DEBUGP("retry %d buf <%s>\n",retry_count,buf);
							if (strstr(buf,"^SYSSTART")) {
								DEBUGP("sysstart\n");
								/* reissue the cmux */
								if (write(ttyfd, cmux, strlen(cmux))) {
									DEBUGP("write failed\n");
								}
								cmux_echoed = 0;
							} else if (strstr(buf,"OK")) {
								retval = 0;
								finish = 1;
							} else if (strstr(buf,"ERROR")) {
								retval = -1;
								finish = 1;
							} else if (strstr(buf,"AT+CMUX")) {
								cmux_echoed = 1;
							}
							startptr = newptr;
							endptr = strchr(startptr,'\r');
							buf[0] = '\0';
						}
						buf[0] = '\0';
						if (finish) {
							break;
						} else if (newptr && *newptr) {
							strcpy(buf,startptr);
						}
					} else {
						if (strlen(buf) + rdlen > 100) {
							DEBUGP("buf overrun\n");
							buf[0] = '\0';
						}

						strcat(buf,input_buf);
						DEBUGP("accum buf <%s>\n",buf);
						if (strstr(buf,"OK")) {
							retval = 0;
							break;
						}
					}
				}
			}
		}
	}

	if (finish && !retval) {
		DEBUGP("cmux OK\n");
		retval = ioctl(ttyfd, TIOCSETD, &mux_disc_index);
		if (retval < 0) {
			/* if the line disp module is not loaded there isn't much that
			   can be done: load the mod and either power cycle the modem
			   or wait 5 seconds for the modem to reset */
			fprintf(stderr,"TIOCSETD failed %d",-errno);
			fprintf(stderr," have you insmod the line discipline?\n");
		} else {
			/* immediately open ports to ensure that mux cmd channel
			 * is setup within time limit allowed by the baseband */
			for (channel = GSMD_CMD_CHANNEL0;
				channel < g->number_channels; channel++) {
				sprintf(buf, "/dev/mux%i", channel+1);
				g->gfd_uart[channel].fd =
					open(buf, O_RDWR | O_NOCTTY | O_NDELAY);
				DEBUGP("channel %s fd %d\n", buf, g->gfd_uart[channel].fd);
				if (g->gfd_uart[channel].fd == -1) {
					fprintf(stderr,
						"Failed to open %s does it exist and have user rw\n",
						buf);
					retval = -1;
					break;
				}
			}
			if (retval) {
				// This is a serious failure - the modem is in mux mode and cannot
				// be interacted with.
				fprintf(stderr,"Serious failure\n");
				atcmd_revert_to_single_port_mode(g);
			}
		}
	} else {
		retval = -1;
	}

	return retval;
}

int atcmd_revert_to_single_port_mode(struct gsmd *g)
{
	int retval = 0;
	if (g->number_channels > 1) {
		int chnl;
		int n_tty = N_TTY;
		DEBUGP("revert to single port\n");
		/* close channel 0 last */
		for (chnl = g->number_channels - 1; chnl >= GSMD_CMD_CHANNEL0; chnl--) {
			gsmd_close_channel(chnl);
		}
		g->number_channels = 1;

		/* switch back to normal line discipline */
		retval = ioctl(g->modem_fd, TIOCSETD, &n_tty);
		if (retval < 0) {
			DEBUGP("TIOCSETD back to N_TTY failed %d",retval);
		} else {
			/* not a sync world, remove any potential leftover mux data */
			sleep(1);
			atcmd_drain(g->modem_fd);

			/* register fd */
			g->gfd_uart[GSMD_CMD_CHANNEL0].when	= GSMD_FD_READ;
			g->gfd_uart[GSMD_CMD_CHANNEL0].data	= g;
			g->gfd_uart[GSMD_CMD_CHANNEL0].cb	= &atcmd_select_cb;
			g->gfd_uart[GSMD_CMD_CHANNEL0].channel	= GSMD_CMD_CHANNEL0;
			g->gfd_uart[GSMD_CMD_CHANNEL0].fd	= g->modem_fd;
			gsmd_register_fd(&g->gfd_uart[GSMD_CMD_CHANNEL0]);
		}
	}
	return retval;
}

#define DUMMYM_UNIX_SOCKET "\0dummym"

static int dummym_init(struct gsmd_fd* handle)
{
	int rc;
	struct sockaddr_un sun;

	/* use unix domain socket to dummy modem */
	handle->fd = socket(PF_UNIX, GSMD_UNIX_SOCKET_TYPE, 0);
	if (handle->fd < 0)
		return handle->fd;

	memset(&sun, 0, sizeof(sun));
	sun.sun_family = AF_UNIX;
	memcpy(sun.sun_path, DUMMYM_UNIX_SOCKET, sizeof(DUMMYM_UNIX_SOCKET));

	rc = connect(handle->fd, (struct sockaddr *)&sun, sizeof(sun));
	if (rc < 0) {
		close(handle->fd);
		handle->fd = -1;
		return rc;
	}
	return 0;
}

/* init atcmd parser */
int atcmd_init(struct gsmd *g, int ttyfd)
{
	u_int8_t channel;
	int retval = 0;

	__atcmd_ctx = talloc_named_const(gsmd_tallocs, 1, "atcmds");
	__gph_ctx = talloc_named_const(gsmd_tallocs, 1, "gph");

	if (g->dummym_enabled) {

		DEBUGP("dummy modem enabled\n");

		for (channel = GSMD_CMD_CHANNEL0; channel < g->number_channels; channel++) {
			if (dummym_init(&g->gfd_dummym[channel])) {
				fprintf(stderr,"failed to connect to dummy modem\n");
				return -1;
			}
		}

	} else {
		if (g->number_channels > 1) {

			DEBUGP("init mux\n");

			if (atcmd_mux(g, ttyfd)) {
				fprintf(stderr,"mux failed - returning to single channel mode\n");
				g->number_channels = 1;
			} else {
				atexit(gsmd_close_channels);
			}
		}

		if (g->number_channels == 1) {
			g->gfd_uart[GSMD_CMD_CHANNEL0].fd = ttyfd;
		}
	}

	/* g->wakeup_timer = NULL; */

	for (channel = GSMD_CMD_CHANNEL0; channel < g->number_channels; channel++) {

		DEBUGP("channel %d\n", channel);

		llparse_init (&g->llp[channel]);

		g->mlbuf_len[channel] = 0;
		g->mlunsolicited[channel] = 0;

		g->llp[channel].cur = g->llp[channel].buf;
		g->llp[channel].len = sizeof(g->llp[channel].buf);
		g->llp[channel].cb = &ml_parse;
		g->llp[channel].prompt_cb = &atcmd_prompt;
		g->llp[channel].channel = channel;
		g->llp[channel].ctx = g;
		g->llp[channel].flags = LGSM_ATCMD_F_EXTENDED;

		if (g->dummym_enabled) {
			g->gfd_dummym[channel].when = GSMD_FD_READ | GSMD_FD_EXCEPT;
			g->gfd_dummym[channel].data = g;
			g->gfd_dummym[channel].cb = &atcmd_select_cb;
			g->gfd_dummym[channel].channel = channel;
			retval = gsmd_register_fd(&g->gfd_dummym[channel]);
		} else {
			g->gfd_uart[channel].when = GSMD_FD_READ;
			g->gfd_uart[channel].data = g;
			g->gfd_uart[channel].cb = &atcmd_select_cb;
			g->gfd_uart[channel].channel = channel;
			retval = gsmd_register_fd(&g->gfd_uart[channel]);
		}

		INIT_LLIST_HEAD(&g->pending_atcmds[channel]);
		INIT_LLIST_HEAD(&g->busy_atcmds[channel]);

		if (retval) {
			gsmd_log(GSMD_ERROR, "failed (%d) to register chnl %d\n", retval, channel);
			break;
		}
	}
	return retval;
}

/* remove from the queues any command whose .ctx matches given */
int atcmd_terminate_matching(struct gsmd *g, void *ctx)
{
	int num = 0;
	struct gsmd_atcmd *cmd, *pos;
	u_int8_t channel;

	for (channel = GSMD_CMD_CHANNEL0; channel < g->number_channels; channel++) {

		llist_for_each_entry_safe(cmd, pos, &g->busy_atcmds[channel], list)
			if (cmd->ctx == ctx) {
				// a cmd for the non existent user is busy, reset the cb to NULL
				remove_channel_timeout(g, channel);
				cmd->cb = NULL;
				cmd->ctx = NULL;
				num ++;
			}

		llist_for_each_entry_safe(cmd, pos, &g->pending_atcmds[channel], list)
			if (cmd->ctx == ctx) {
				llist_del(&cmd->list);
				atcmd_free(cmd);
				num ++;
			}
	}

	return num;
}

