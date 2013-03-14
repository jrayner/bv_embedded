/* Siemens TC65/MC55/MC55i compatible gsmd plugin
 *
 * Copyright (C) 2007-2009 Jim Rayner <jimr@beyondvoice.com>
 *
 */

#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#include "gsmd.h"

#include <gsmd/gsmd.h>
#include <gsmd/usock.h>
#include <gsmd/event.h>
#include <gsmd/talloc.h>
#include <gsmd/extrsp.h>
#include <gsmd/atcmd.h>
#include <gsmd/vendorplugin.h>
#include <gsmd/unsolicited.h>
#include <gsmd/ts0707.h>

#define MAX_SIM_NOTIF_RETRIES 3
#define SIM_NOTIF_DELAY 4

static int received_valid_slcc = 0;

static int sysstart(char *buf, int len, const char *param,
			 struct gsmd *gsmd)
{

	return 0;
}

static int siemens_call_status(char *buf, int len, const char *param,
			 struct gsmd *gsmd)
{
	//^SLCC: 1,0,0,0,0,1,"01223303528",129,""
	// idx dir stat mode mpty tch number type alpha

	int retval = 0;
	char *comma = strchr(param, ',');
	gsmd_log(GSMD_DEBUG, "received call status (%s)\n",param);

	if (comma && ++comma) {
		unsigned char val = atoi(comma);
		received_valid_slcc = 1;
		gsmd_log(GSMD_NOTICE, "dir <%d>\n",val);
		comma = strchr(comma, ',');
		if (comma && ++comma) {
			struct gsmd_ucmd *ucmd =
				usock_build_event(GSMD_MSG_EVENT, GSMD_EVT_OUT_STATUS,
					sizeof(struct gsmd_evt_auxdata));

			val = atoi(comma);
			gsmd_log(GSMD_NOTICE, "stat <%d>\n",val);

			if (valid_ucmd(ucmd)) {
				struct gsmd_evt_auxdata *eaux =
					((void *)ucmd) + sizeof(struct gsmd_ucmd);
				char* out_ptr = (char*) &eaux->u.call_status.addr.number;
				switch (val) {
					case 0:
						gsmd_log(GSMD_DEBUG, "active\n");
						eaux->u.call_status.prog = GSMD_CALLPROG_CONNECTED;
						break;
					case 1:
						gsmd_log(GSMD_DEBUG, "held\n");
						break;
					case 2:
						gsmd_log(GSMD_DEBUG, "dialling\n");
						eaux->u.call_status.prog = GSMD_CALLPROG_SETUP;
						break;
					case 3:
						gsmd_log(GSMD_DEBUG, "remote end is ringing\n");
						eaux->u.call_status.prog = GSMD_CALLPROG_PROGRESS;
						break;
					case 4:
						gsmd_log(GSMD_DEBUG, "incoming call\n");
						eaux->u.call_status.prog = GSMD_CALLPROG_ALERT;
						break;
					case 5:
						gsmd_log(GSMD_DEBUG, "call waiting\n");
						break;
				}
				comma = strchr(comma, '"');
				if (comma && ++comma) {
					char* t_ptr = comma;
					int loop;
					gsmd_log(GSMD_DEBUG, "t_ptr %s\n",t_ptr);
					if (t_ptr) {
						for (loop = 0; loop <= GSMD_ADDR_MAXLEN; loop++) {
							if (*t_ptr) {
								if (*t_ptr == '"')
									break;
								*out_ptr++ = *t_ptr++;
							} else {
								break;
							}
						}
					}
				}
				*out_ptr = 0;
				gsmd_log(GSMD_DEBUG, "call %s\n",eaux->u.call_status.addr.number);
			}

			retval = usock_evt_send(gsmd, ucmd, GSMD_EVT_OUT_STATUS);
		}
	} else {
		if (received_valid_slcc) {
			// Ignore if valid SLCC received prior to this SLCC
			gsmd_log(GSMD_DEBUG, "Ignoring blank SLCC\n");
			received_valid_slcc = 0;
		} else {
			// Disconnected
			struct gsmd_ucmd *ucmd =
				usock_build_event(GSMD_MSG_EVENT, GSMD_EVT_OUT_STATUS,
					sizeof(struct gsmd_evt_auxdata));

			if (valid_ucmd(ucmd)) {
				struct gsmd_evt_auxdata *eaux =
					((void *)ucmd) + sizeof(struct gsmd_ucmd);
				eaux->u.call_status.prog = GSMD_CALLPROG_RELEASE;
				eaux->u.call_status.addr.number[0] = 0;
			}
			retval = usock_evt_send(gsmd, ucmd, GSMD_EVT_OUT_STATUS);
		}
	}

	return retval;
}


static const struct gsmd_unsolicit siemens_unsolicit[] = {
	{ "^SYSSTART",	&sysstart },
	{ "^SLCC",  &siemens_call_status },
};

static int siemens_detect(struct gsmd *g)
{
	/* FIXME: do actual detection of vendor if we have multiple vendors */
	return 1;
}

static int query_network_cb(struct gsmd_atcmd *cmd, void *ctx, char *resp)
{
	if (!strncmp(resp, "+CREG:", 6)) {
		struct gsmd *g = ctx;
		const char *comma = strchr(resp+6, ',');
		if (++comma && *comma) {
			/* pass rest on to be parsed as an unsolicited */
			creg_parse(resp, strlen(resp), comma, g);
		}
	}
	return 0;
}

int query_network_status(struct gsmd *gsmd)
{
	struct gsmd_atcmd *cmd;
	cmd = atcmd_fill("AT+CREG?", 8+1, &query_network_cb, gsmd, NULL);
	if (!cmd)
		return -ENOMEM;
	cmd->timeout_value = 10; /* in case modem is slow */

	return atcmd_submit(gsmd, cmd, GSMD_CMD_CHANNEL0);
}

static int sim_notification_cb(struct gsmd_atcmd *cmd, void *ctx, char *resp);

static int sim_notification_cmd(struct gsmd *gsmd, int retries, int retry_delay)
{
	struct gsmd_atcmd *cmd;
	int retval = 0;
	int notif_channel = GSMD_CMD_CHANNEL0;

	cmd = atcmd_fill("AT+CSMS=1", 9+1, &sim_notification_cb, gsmd, NULL);
	if (!cmd)
		return -ENOMEM;
	cmd->timeout_value = 10;
	cmd->flags |= ATCMD_PIN_SENSITIVE;
	cmd->cmd_retries = retries;
	cmd->initial_delay_secs = retry_delay;

	if (gsmd->number_channels > GSMD_NOTIFS_CHANNEL)
		notif_channel = GSMD_NOTIFS_CHANNEL;

	if (retries) {
		/* high priority to ensure the retried cmd goes at start of
		   the pending list */
		retval = atcmd_submit_highpriority(gsmd, cmd, notif_channel);
	} else {
		retval = atcmd_submit(gsmd, cmd, notif_channel);
	}

	return retval;
}

static int sim_notification_cb(struct gsmd_atcmd *cmd, void *ctx, char *resp)
{
	DEBUGP("AT+CSMS=1 returned %s\n", resp);
	if (cmd->ret) {
		struct gsmd *g = ctx;
		/* The MC55i in the SL40 often gives SIM busy errors */
		if (-GSM0707_CME_SIM_BUSY == cmd->ret &&
			cmd->cmd_retries < MAX_SIM_NOTIF_RETRIES) {
			DEBUGP("Resubmitting +CSMS=1 (retry %d)\n",
				cmd->cmd_retries);
			sim_notification_cmd(
				g, cmd->cmd_retries+1,SIM_NOTIF_DELAY);
		}
	}
	return cmd->ret;
}

static int siemens_initsettings(struct gsmd *gsmd)
{
	int rc = 0;
	struct gsmd_atcmd *cmd;
	u_int8_t channel;

	//rc |= gsmd_simplecmd(gsmd, "AT&F"); // JIM resets the TC65 - cannot be done from siemens plugin

	// Added by Jim - need to be in place from the start
	/* use +CSMS: to set phase 2+  */

	// Call status event reporting
	rc |= gsmd_notification_cmd(gsmd, "AT^SLCC=1", PIN_DEPENDENT);
	rc |= gsmd_notification_cmd(gsmd, "AT^SIND=\"audio\",1", PIN_DEPENDENT);

#ifdef USING_TDC_DEV_BOARD
	// dev boards use default AT^SAIC=2,1,1
#else
	// TCL SL40 uses analog, mic 2 & earpiece 2
	rc |= gsmd_simplecmd(gsmd, "AT^SNFS=2", NO_PIN_DEPEND);
	// factory defaults for SNFS 2 should same as
	// rc |= gsmd_simplecmd(gsmd, "AT^SAIC=2,2,2", NO_PIN_DEPEND);
#endif
	// Set the default DTMF tone duration (10 10ths i.e 1 second)
	rc |= gsmd_simplecmd(gsmd, "AT+VTD=10", PIN_DEPENDENT);

	// AT+CSMS=1 must be passed down the same channel as the AT+CNMI
	// This SIM command may be delayed if the SIM is busy
	rc |= sim_notification_cmd(gsmd, 0,0);
	//JIM - for now we don't process cell broadcast messages
	rc |= gsmd_notification_cmd(gsmd, "AT+CNMI=2,2,0,1,1",PIN_DEPENDENT);

	// Force a creg lookup. MC55i doesn't appear to always give a CREG notification after
	// pin verification and auto operator selection
	rc |= query_network_status(gsmd);

	for (channel = GSMD_CMD_CHANNEL0; channel < gsmd->number_channels; channel++) {
		/* Siemens sometimes sends LFLF instead of CRLF */
		gsmd->llp[channel].flags |= LGSM_ATCMD_F_LFLF;
	}

	return rc;
}

struct gsmd_vendor_plugin gsmd_vendor_plugin = {
	.name = "siemens",
	.ext_chars = "^",
	.num_unsolicit = ARRAY_SIZE(siemens_unsolicit),
	.unsolicit = siemens_unsolicit,
	.detect = &siemens_detect,
	.initsettings = &siemens_initsettings,
};
