/* gsmd unsolicited message handling
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
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>

#include "gsmd.h"

#include <gsmd/usock.h>
#include <gsmd/event.h>
#include <gsmd/extrsp.h>
#include <gsmd/ts0707.h>
#include <gsmd/unsolicited.h>
#include <gsmd/talloc.h>

struct gsmd_ucmd *usock_build_event(u_int8_t type, u_int8_t subtype, u_int16_t len)
{
	struct gsmd_ucmd *ucmd = ucmd_alloc(len);

	if (!ucmd)
		return NULL;

	ucmd->hdr.msg_type = type;
	ucmd->hdr.msg_subtype = subtype;
	ucmd->hdr.ret = 0;
	ucmd->hdr.flags = 0;

	return ucmd;
}

static struct gsmd_ucmd *ucmd_copy(const struct gsmd_ucmd *orig)
{
	struct gsmd_ucmd *copy = ucmd_alloc(orig->hdr.len);

	/* copy header and data */
	if (copy)
		memcpy(copy, orig, sizeof(struct gsmd_ucmd) + orig->hdr.len);

	return copy;
}

int usock_evt_send(struct gsmd *gsmd, struct gsmd_ucmd *ucmd, u_int32_t evt)
{
	struct gsmd_user *gu;
	int num_sent = 0;

	DEBUGP("entering evt=%u\n", evt);

	llist_for_each_entry(gu, &gsmd->users, list) {
		if (gu->subscriptions & (1 << evt)) {
			if (num_sent == 0)
				usock_cmd_enqueue(ucmd, gu);
			else {
				struct gsmd_ucmd *cpy = ucmd_copy(ucmd);
				if (!cpy) {
					fprintf(stderr,
						"can't allocate memory for "
						"copy of ucmd\n");
					return num_sent;
				}
				usock_cmd_enqueue(cpy, gu);
			}
			num_sent++;
		}
	}

	if (num_sent == 0)
		talloc_free(ucmd);

	return num_sent;
}

static int no_carrier_parse(char *buf, int len, const char *param,
		      struct gsmd *gsmd)
{
	struct gsmd_ucmd *ucmd = usock_build_event(GSMD_MSG_EVENT, GSMD_EVT_DISC_GPRS,
			sizeof(struct gsmd_evt_auxdata));

	return usock_evt_send(gsmd, ucmd, GSMD_EVT_DISC_GPRS);
}

static int ring_parse(char *buf, int len, const char *param,
		      struct gsmd *gsmd)
{
	/* FIXME: generate ring event */
	struct gsmd_ucmd *ucmd = usock_build_event(GSMD_MSG_EVENT, GSMD_EVT_IN_CALL,
						   sizeof(struct gsmd_evt_auxdata));

	if (valid_ucmd(ucmd)) {
		struct gsmd_evt_auxdata *aux = (struct gsmd_evt_auxdata *) ucmd->buf;
		aux->u.call.type = GSMD_CALL_UNSPEC;
	}

	return usock_evt_send(gsmd, ucmd, GSMD_EVT_IN_CALL);
}

static int cring_parse(char *buf, int len, const char *param, struct gsmd *gsmd)
{
	struct gsmd_ucmd *ucmd = usock_build_event(GSMD_MSG_EVENT, GSMD_EVT_IN_CALL,
						   sizeof(struct gsmd_evt_auxdata));

	if (valid_ucmd(ucmd)) {
		struct gsmd_evt_auxdata *aux = (struct gsmd_evt_auxdata *) ucmd->buf;

		if (!strcmp(param, "VOICE")) {
			/* incoming voice call */
			aux->u.call.type = GSMD_CALL_VOICE;
		} else if (!strcmp(param, "SYNC")) {
			aux->u.call.type = GSMD_CALL_DATA_SYNC;
		} else if (!strcmp(param, "REL ASYNC")) {
			aux->u.call.type = GSMD_CALL_DATA_REL_ASYNC;
		} else if (!strcmp(param, "REL SYNC")) {
			aux->u.call.type = GSMD_CALL_DATA_REL_SYNC;
		} else if (!strcmp(param, "FAX")) {
			aux->u.call.type = GSMD_CALL_FAX;
		} else if (!strncmp(param, "GPRS ", 5)) {
			/* FIXME: change event type to GPRS */
			talloc_free(ucmd);
			return 0;
		}
		/* FIXME: parse all the ALT* profiles, Chapter 6.11 */
	}

	return usock_evt_send(gsmd, ucmd, GSMD_EVT_IN_CALL);
}

/* Chapter 7.2, network registration */
int creg_parse(char *buf, int len, const char *param, struct gsmd *gsmd)
{
	struct gsmd_ucmd *ucmd = usock_build_event(GSMD_MSG_EVENT, GSMD_EVT_NETREG,
						 sizeof(struct gsmd_evt_auxdata));

	if (valid_ucmd(ucmd)) {
		const char *comma = strchr(param, ',');
		struct gsmd_evt_auxdata *aux = (struct gsmd_evt_auxdata *) ucmd->buf;

		aux->u.netreg.state = atoi(param);
		aux->u.netreg.lac = aux->u.netreg.ci = 0;
		if (comma) {
			if (++comma && (*comma == '"')) {
				/* we also have location area code and cell id to parse (hex) */
				aux->u.netreg.lac = strtoul(comma+1, NULL, 16);
				comma = strchr(comma, ',');
				if (!comma) {
					gsmd_log(GSMD_NOTICE, "Unknown format <%s>\n",param);
					ucmd->hdr.ret = -EINVAL;
				} else if (++comma && (*comma == '"')) {
					aux->u.netreg.ci = strtoul(comma+1, NULL, 16);
				}
			}
		}

		/* Intialise things that depend on network registration */
		if (aux->u.netreg.state == GSMD_NETREG_REG_HOME ||
				aux->u.netreg.state == GSMD_NETREG_REG_ROAMING) {
			sms_cb_network_init(gsmd);
		}
	}

	return usock_evt_send(gsmd, ucmd, GSMD_EVT_NETREG);
}

/* Chapter 7.11, call waiting */
static int ccwa_parse(char *buf, int len, const char *param,
		      struct gsmd *gsmd)
{
	struct gsmd_ucmd *ucmd = usock_build_event(GSMD_MSG_EVENT, GSMD_EVT_CALL_WAIT,
						 sizeof(struct gsmd_addr));

	if (valid_ucmd(ucmd)) {
		const char *token = strtok(buf, ",");
		unsigned int type;
		struct gsmd_addr *gaddr = (struct gsmd_addr *) ucmd->buf;

		memset(gaddr, 0, sizeof(*gaddr));

		/* parse address (phone number) */
		if (!token) {
			gsmd_log(GSMD_NOTICE, "Unknown format <%s>\n",buf);
			ucmd->hdr.ret = -EINVAL;
		} else {
			strncpy(gaddr->number, token, GSMD_ADDR_MAXLEN);

			/* parse type */
			token = strtok(NULL, ",");/* FIXME NULL ? */
			if (!token) {
				gsmd_log(GSMD_NOTICE, "Unknown format <%s>\n",buf);
				ucmd->hdr.ret = -EINVAL;
			} else {
				type = atoi(token) & 0xff;
				gaddr->type = type;

				/* FIXME: parse class */
				token = strtok(NULL, ",");/* FIXME NULL ? */
			}
		}
	}

	return usock_evt_send(gsmd, ucmd, GSMD_EVT_CALL_WAIT);
}

/* Chapter 7.14, unstructured supplementary service data */
static int cusd_parse(char *buf, int len, const char *param,
		      struct gsmd *gsmd)
{
	int retval = 0;
	int cleanup = 1;
	struct gsmd_ucmd *ucmd = usock_build_event(GSMD_MSG_EVENT, GSMD_EVT_USSD,
						sizeof(struct gsmd_evt_auxdata) +
						sizeof(struct gsmd_ussd_notif));

	if (valid_ucmd(ucmd)) {
		const char *quote = strchr(param, '"');
		struct gsmd_evt_auxdata *aux = (struct gsmd_evt_auxdata *) ucmd->buf;
		struct gsmd_ussd_notif *gussd = (struct gsmd_ussd_notif *) aux->data;

		memset(gussd, 0, sizeof(*gussd));

		if (!quote) {
			gsmd_log(GSMD_NOTICE, "Unknown format <%s>\n",buf);
			ucmd->hdr.ret = -EINVAL;
		} else {
			const char* next_field = quote+1;
			int action = atoi(param);
			int cancel_pending_on_chnl = 0;
			gussd->action_reqd = (GSMD_USSD_ACTION_REQD == action);
			if (gussd->action_reqd) {
				gsmd_log(GSMD_NOTICE, "Ussd response reqd\n");
				/* response gets priority on the channel,
				   ensure any pending cmds are cleared */
				cancel_pending_on_chnl = 1;
			}
			if (next_field) {
				/* TODO Assumption here is that the network USSD string
				   does not contain a quote character */
				char *next_quote = strchr(next_field, '"');
				if (next_quote) {
					int copy_len = next_quote - next_field;
					if (copy_len > GSMD_USSD_MAXLEN)
						copy_len = GSMD_USSD_MAXLEN;
					strncpy(gussd->data, next_field, copy_len);
				} else {
					gsmd_log(GSMD_NOTICE, "Missing end quote\n");
				}

				/* Only transfer USSD to client if it contains data */
				retval = usock_evt_send(gsmd, ucmd, GSMD_EVT_USSD);
				if (cancel_pending_on_chnl && !retval) 
					retval = -ECANCELED;
				cleanup = 0;
			}
		}
	}
	if (cleanup)
		talloc_free(ucmd);

	return retval;
}

/* Chapter 7.15, advise of charge */
static int cccm_parse(char *buf, int len, const char *param,
		      struct gsmd *gsmd)
{
	/* FIXME: parse */
	return 0;
}

/* Chapter 10.1.13, GPRS event reporting */
static int cgev_parse(char *buf, int len, const char *param,
		      struct gsmd *gsmd)
{
	/* FIXME: parse */
	return 0;
}

/* Chapter 10.1.14, GPRS network registration status */
static int cgreg_parse(char *buf, int len, const char *param,
		       struct gsmd *gsmd)
{
	/* FIXME: parse */
	return 0;
}

/* Chapter 7.6, calling line identification presentation */
static int clip_parse(char *buf, int len, const char *param,
		      struct gsmd *gsmd)
{
	struct gsmd_ucmd *ucmd = usock_build_event(GSMD_MSG_EVENT, GSMD_EVT_IN_CLIP,
						 sizeof(struct gsmd_evt_auxdata));

	if (valid_ucmd(ucmd)) {
		struct gsmd_evt_auxdata *aux = (struct gsmd_evt_auxdata *) ucmd->buf;
		const char *comma = strchr(param, ',');

		if (!comma) {
			gsmd_log(GSMD_NOTICE, "Unknown format <%s>\n",param);
			ucmd->hdr.ret = -EINVAL;
		} else if (comma - param > GSMD_ADDR_MAXLEN) {
			gsmd_log(GSMD_NOTICE, "Max len exceeded <%s>\n",param);
			ucmd->hdr.ret = -EINVAL;
		} else {
			aux->u.clip.addr.number[0] = '\0';
			strncat(aux->u.clip.addr.number, param, comma-param);
			/* FIXME: parse of subaddr, etc. */
		}
	}

	return usock_evt_send(gsmd, ucmd, GSMD_EVT_IN_CLIP);
}

/* Chapter 7.9, calling line identification presentation */
static int colp_parse(char *buf, int len, const char *param,
		      struct gsmd *gsmd)
{
	struct gsmd_ucmd *ucmd = usock_build_event(GSMD_MSG_EVENT, GSMD_EVT_OUT_COLP,
						 sizeof(struct gsmd_evt_auxdata));

	if (valid_ucmd(ucmd)) {
		struct gsmd_evt_auxdata *aux = (struct gsmd_evt_auxdata *) ucmd->buf;
		const char *comma = strchr(param, ',');

		if (!comma) {
			gsmd_log(GSMD_NOTICE, "Unknown format <%s>\n",param);
			ucmd->hdr.ret = -EINVAL;
		} else if (comma - param > GSMD_ADDR_MAXLEN) {
			gsmd_log(GSMD_NOTICE, "Max len exceeded <%s>\n",param);
			ucmd->hdr.ret = -EINVAL;
		} else {
			aux->u.colp.addr.number[0] = '\0';
			strncat(aux->u.colp.addr.number, param, comma-param);
			/* FIXME: parse of subaddr, etc. */
		}
	}

	return usock_evt_send(gsmd, ucmd, GSMD_EVT_OUT_COLP);
}

static int ctzv_parse(char *buf, int len, const char *param,
		      struct gsmd *gsmd)
{
	struct gsmd_ucmd *ucmd = usock_build_event(GSMD_MSG_EVENT, GSMD_EVT_TIMEZONE,
						 sizeof(struct gsmd_evt_auxdata));

	if (valid_ucmd(ucmd)) {
		struct gsmd_evt_auxdata *aux = (struct gsmd_evt_auxdata *) ucmd->buf;

		/* timezones are expressed in quarters of hours +/- GMT (-48...+48) */
		int tz = atoi(param);

		if (tz < -48  || tz > 48) {
			gsmd_log(GSMD_NOTICE, "Unknown timezone <%s>\n",param);
			ucmd->hdr.ret = -EINVAL;
		} else
		   aux->u.timezone.tz = tz;
	}

	return usock_evt_send(gsmd, ucmd, GSMD_EVT_TIMEZONE);
}

static int copn_parse(char *buf, int len, const char *param,
		      struct gsmd *gsmd)
{
	struct gsm_extrsp *er = extrsp_parse(gsmd_tallocs, param);
	int rc = 0;

	if (!er)
		return -ENOMEM;

	extrsp_dump(er);

	if (er->num_tokens == 2 &&
		er->tokens[0].type == GSMD_ECMD_RTT_STRING &&
		er->tokens[1].type == GSMD_ECMD_RTT_STRING)
		rc = gsmd_opname_add(gsmd, er->tokens[0].u.string,
					 er->tokens[1].u.string);

	talloc_free(er);

	return rc;
}

static int ciev_parse(char *buf, int len, const char *param,
		      struct gsmd *gsmd)
{
	int retval = 0;
	const char *comma = strchr(param, ',');
	unsigned char val = 0;
	if (comma) {
		val = atoi(comma+1);
		gsmd_log(GSMD_NOTICE, "val <%d>\n",val);
	}

	if (!strncmp(param, "battchg",7)) { /*battery charge level (0‑5)*/
		if (5 == val) {
			gsmd_log(GSMD_INFO, "No battery connected\n");
		}
	} else if (!strncmp(param, "signal",6)) {

		/*signal bit error rate (0‑7)*/
		signal_strength_changed(gsmd,gsmd->rssi_idx, val);

	} else if (!strncmp(param, "service",7)) { /*service availability (0‑1)*/
		if (!val) {
			gsmd_log(GSMD_INFO, "Unregistered\n");
			retval = network_status_changed(gsmd, GSMD_NETREG_UNREG);
		}
		if (1 == val) {
			if (gsmd->roaming_status) {
				gsmd_log(GSMD_INFO, "Registered to network (roaming)\n");
				retval = network_status_changed(gsmd, GSMD_NETREG_REG_ROAMING);
			} else {
				gsmd_log(GSMD_INFO, "Registered to network (home)\n");
				retval = network_status_changed(gsmd, GSMD_NETREG_REG_HOME);
			}
		}
	} else if (!strncmp(param, "sounder",7)) { /*sounder activity (0‑1)*/
	} else if (!strncmp(param, "message",7)) { /*message received (0‑1)*/
	} else if (!strncmp(param, "call",4)) { /*call in progress (0‑1)*/
	} else if (!strncmp(param, "vox",3)) { /*transmit activated by voice activity (0‑1)*/
	} else if (!strncmp(param, "roam",4)) { /*roaming indicator (0‑1) */
		/* use service for ntwk registration status */
		if (!val) {
			gsmd_log(GSMD_INFO, "Not roaming\n");
			gsmd->roaming_status = 0;
		}
		if (1 == val) {
			gsmd_log(GSMD_INFO, "Roaming\n");
			gsmd->roaming_status = 1;
		}
	} else if (!strncmp(param, "smsfull",7)) {
		if (1 == val) {
			gsmd_log(GSMD_INFO, "SIM is full\n");
		}
	} else if (!strncmp(param, "rssi",4)) {
		retval = signal_strength_changed(gsmd,val, gsmd->ber_idx);
	} else {
		gsmd_log(GSMD_NOTICE, "Unknown +CIEV param <%s>\n",param);
		comma = NULL;
	}
	return retval;
}

static const struct gsmd_unsolicit gsm0707_unsolicit[] = {
	{ "NO CARRIER",	&no_carrier_parse },
	{ "RING",	&ring_parse },
	{ "+CRING", 	&cring_parse },
	{ "+CREG",	&creg_parse },	/* Network registration */
	{ "+CCWA",	&ccwa_parse },	/* Call waiting */
	{ "+CUSD",	&cusd_parse },	/* Unstructured supplementary data */
	{ "+CCCM",	&cccm_parse },	/* Advice of Charge */
	{ "+CGEV",	&cgev_parse },	/* GPRS Event */
	{ "+CGREG",	&cgreg_parse },	/* GPRS Registration */
	{ "+CLIP",	&clip_parse },
	{ "+COLP",	&colp_parse },
	{ "+CTZV",	&ctzv_parse },	/* Timezone */
	{ "+COPN",	&copn_parse },  /* operator names, treat as unsolicited */
	{ "+CIEV",	&ciev_parse },
	/*
	{ "+CKEV",	&ckev_parse },
	{ "+CDEV",	&cdev_parse },
	{ "+CIEV",	&ciev_parse },
	{ "+CLAV",	&clav_parse },
	{ "+CCWV",	&ccwv_parse },
	{ "+CLAV",	&clav_parse },
	{ "+CSSU",	&cssu_parse },
	*/
};

static struct gsmd_unsolicit unsolicit[256] = {{ 0, 0 }};

/* called by midlevel parser if a response seems unsolicited */
int unsolicited_parse(struct gsmd *g, char *buf, int len, const char *param, u_int8_t channel)
{
	struct gsmd_unsolicit *i;
	int rc;
	struct gsmd_vendor_plugin *vpl = g->vendorpl;

	/* call unsolicited code parser */
	for (i = unsolicit; i->prefix; i ++) {
		const char *colon;
		if (strncmp(buf, i->prefix, strlen(i->prefix)))
			continue;

		colon = strchr(buf, ':') + 2;
		if (colon > buf+len)
			colon = NULL;

		rc = i->parse(buf, len, colon, g);
		if (-ECANCELED == rc) {
			cancel_pending_atcmds(g, channel);
			rc = 0;
		}
		if (rc == -EAGAIN)
			return rc;
		if (rc < 0)
			gsmd_log(GSMD_ERROR, "error %d during parsing of "
				 "an unsolicited response `%s'\n",
				 rc, buf);
		return rc;
	}

	gsmd_log(GSMD_NOTICE, "no parser for unsolicited response `%s'\n", buf);

	return -ENOENT;
}

int unsolicited_register_array(const struct gsmd_unsolicit *arr, int len)
{
	int curlen = 0;

	while (unsolicit[curlen ++].prefix);
	if (len + curlen > ARRAY_SIZE(unsolicit))
		return -ENOMEM;

	/* Add at the beginning for overriding to be possible */
	memmove(&unsolicit[len], unsolicit,
			sizeof(struct gsmd_unsolicit) * curlen);
	memcpy(unsolicit, arr,
			sizeof(struct gsmd_unsolicit) * len);

	return 0;
}

void unsolicited_init(struct gsmd *g)
{
	struct gsmd_vendor_plugin *vpl = g->vendorpl;

	/* register generic unsolicited code parser */
	unsolicited_register_array(gsm0707_unsolicit,
			ARRAY_SIZE(gsm0707_unsolicit));

	/* register vendor-specific unsolicited code parser */
	if (vpl && vpl->num_unsolicit)
		if (unsolicited_register_array(vpl->unsolicit,
					vpl->num_unsolicit))
			gsmd_log(GSMD_ERROR, "registering vendor-specific "
					"unsolicited responses failed\n");
}

static unsigned int errors_creating_events[] = {
	GSM0707_CME_PHONE_FAILURE,
	GSM0707_CME_PHONE_NOCONNECT,
	GSM0707_CME_PHONE_ADAPT_RESERVED,
	GSM0707_CME_PH_SIM_PIN_REQUIRED,
	GSM0707_CME_PH_FSIM_PIN_REQUIRED,
	GSM0707_CME_PH_FSIM_PUK_REQUIRED,
	GSM0707_CME_SIM_NOT_INSERTED,
	GSM0707_CME_SIM_PIN_REQUIRED,
	GSM0707_CME_SIM_PUK_REQUIRED,
	GSM0707_CME_SIM_FAILURE,
	GSM0707_CME_SIM_BUSY,
	GSM0707_CME_SIM_WRONG,
	GSM0707_CME_INCORRECT_PASSWORD,
	GSM0707_CME_SIM_PIN2_REQUIRED,
	GSM0707_CME_SIM_PUK2_REQUIRED,
	GSM0707_CME_MEMORY_FULL,
	GSM0707_CME_MEMORY_FAILURE,
	GSM0707_CME_NETPERS_PIN_REQUIRED,
	GSM0707_CME_NETPERS_PUK_REQUIRED,
	GSM0707_CME_NETSUBSET_PIN_REQUIRED,
	GSM0707_CME_NETSUBSET_PUK_REQUIRED,
	GSM0707_CME_PROVIDER_PIN_REQUIRED,
	GSM0707_CME_PROVIDER_PUK_REQUIRED,
	GSM0707_CME_CORPORATE_PIN_REQUIRED,
	GSM0707_CME_CORPORATE_PUK_REQUIRED,
};

static int is_in_array(unsigned int val, unsigned int *arr, unsigned int arr_len)
{
	unsigned int i;

	for (i = 0; i < arr_len; i++) {
		if (arr[i] == val)
			return 1;
	}

	return 0;
}

int map_cme_error_to_pin_type(unsigned int cme_error)
{
	int type = GSMD_PIN_NONE;
	switch (cme_error) {
	case GSM0707_CME_PH_SIM_PIN_REQUIRED:
		type = GSMD_PIN_PH_SIM_PIN;
		break;
	case GSM0707_CME_PH_FSIM_PIN_REQUIRED:
		type = GSMD_PIN_PH_FSIM_PIN;
		break;
	case GSM0707_CME_PH_FSIM_PUK_REQUIRED:
		type = GSMD_PIN_PH_FSIM_PUK;
		break;
	case GSM0707_CME_SIM_PIN_REQUIRED:
		type = GSMD_PIN_SIM_PIN;
		break;
	case GSM0707_CME_SIM_PUK_REQUIRED:
		type = GSMD_PIN_SIM_PUK;
		break;
	case GSM0707_CME_SIM_PIN2_REQUIRED:
		type = GSMD_PIN_SIM_PIN2;
		break;
	case GSM0707_CME_SIM_PUK2_REQUIRED:
		type = GSMD_PIN_SIM_PUK2;
		break;
	case GSM0707_CME_NETPERS_PIN_REQUIRED:
		type = GSMD_PIN_PH_NET_PIN;
		break;
	case GSM0707_CME_NETPERS_PUK_REQUIRED:
		type = GSMD_PIN_PH_NET_PUK;
		break;
	case GSM0707_CME_NETSUBSET_PIN_REQUIRED:
		type = GSMD_PIN_PH_NETSUB_PIN;
		break;
	case GSM0707_CME_NETSUBSET_PUK_REQUIRED:
		type = GSMD_PIN_PH_NETSUB_PUK;
		break;
	case GSM0707_CME_PROVIDER_PIN_REQUIRED:
		type = GSMD_PIN_PH_SP_PIN;
		break;
	case GSM0707_CME_PROVIDER_PUK_REQUIRED:
		type = GSMD_PIN_PH_SP_PUK;
		break;
	case GSM0707_CME_CORPORATE_PIN_REQUIRED:
		type = GSMD_PIN_PH_CORP_PIN;
		break;
	case GSM0707_CME_CORPORATE_PUK_REQUIRED:
		type = GSMD_PIN_PH_CORP_PUK;
		break;
	default:
		break;
	}
	return type;
}

int generate_event_from_cme(struct gsmd *g, int cme_error)
{
	int retval = 0;
	struct gsmd_ucmd *ucmd = NULL;

	if (!is_in_array(cme_error, errors_creating_events,
		ARRAY_SIZE(errors_creating_events))) {

		ucmd =
			usock_build_event(GSMD_MSG_EVENT, GSMD_EVT_IN_ERROR,
				sizeof(struct gsmd_evt_auxdata));

		if (valid_ucmd(ucmd)) {
			struct gsmd_evt_auxdata *eaux =
				((void *)ucmd) + sizeof(struct gsmd_ucmd);
			eaux->u.cme_err.number = cme_error;
		}
		retval = usock_evt_send(g, ucmd, GSMD_EVT_IN_ERROR);
	}
	else
	{
		switch (cme_error) {
		case GSM0707_CME_SIM_FAILURE:
		case GSM0707_CME_SIM_WRONG:
		case GSM0707_CME_SIM_NOT_INSERTED:
			retval = sim_status_changed(g, cme_error);
			break;
		case GSM0707_CME_MEMORY_FULL:
		case GSM0707_CME_MEMORY_FAILURE:
		case GSM0707_CME_PHONE_FAILURE:
		case GSM0707_CME_PHONE_NOCONNECT:
		case GSM0707_CME_PHONE_ADAPT_RESERVED:
			retval = modem_status_changed(g, cme_error);
			break;
		case GSM0707_CME_SIM_PIN_REQUIRED:
		case GSM0707_CME_SIM_PUK_REQUIRED:
		case GSM0707_CME_SIM_PIN2_REQUIRED:
		case GSM0707_CME_SIM_PUK2_REQUIRED:
			retval = pin_status_changed(g, cme_error);
			break;
		case GSM0707_CME_PH_SIM_PIN_REQUIRED:
		case GSM0707_CME_PH_FSIM_PIN_REQUIRED:
		case GSM0707_CME_PH_FSIM_PUK_REQUIRED:
		case GSM0707_CME_NETPERS_PIN_REQUIRED:
		case GSM0707_CME_NETPERS_PUK_REQUIRED:
		case GSM0707_CME_NETSUBSET_PIN_REQUIRED:
		case GSM0707_CME_NETSUBSET_PUK_REQUIRED:
		case GSM0707_CME_PROVIDER_PIN_REQUIRED:
		case GSM0707_CME_PROVIDER_PUK_REQUIRED:
		case GSM0707_CME_CORPORATE_PIN_REQUIRED:
		case GSM0707_CME_CORPORATE_PUK_REQUIRED:
			retval = pin_status_changed(g, cme_error);
			break;
		case GSM0707_CME_SIM_BUSY:
			gsmd_log(GSMD_INFO, "sim busy\n");
			break;
		default:
			gsmd_log(GSMD_ERROR, "unhandled cme error %d\n", cme_error);
			break;
		}
	}
	return retval;
}

int generate_event_from_cms(struct gsmd *g,  int cms_error)
{
	struct gsmd_ucmd *ucmd =
		usock_build_event(GSMD_MSG_EVENT, GSMD_EVT_IN_ERROR,
			sizeof(struct gsmd_evt_auxdata));

	if (valid_ucmd(ucmd)) {
		struct gsmd_evt_auxdata *eaux =
			((void *)ucmd) + sizeof(struct gsmd_ucmd);
		eaux->u.cms_err.number = cms_error;
	}

	return usock_evt_send(g, ucmd, GSMD_EVT_IN_ERROR);
}

struct gsmd_ucmd* generate_status_event(struct gsmd *g)
{
	struct gsmd_ucmd* ucmd =
		usock_build_event(GSMD_MSG_EVENT, GSMD_EVT_STATUS,
			sizeof(struct gsmd_evt_auxdata));

	if (valid_ucmd(ucmd)) {
		struct gsmd_evt_auxdata *eaux =
			((void *)ucmd) + sizeof(struct gsmd_ucmd);
		eaux->u.evt_status.modem_status = g->modem_status;
		eaux->u.evt_status.sim_present = g->sim_present;
		eaux->u.evt_status.sim_status = g->sim_status;
		eaux->u.evt_status.pin_status = g->pin_status;
		eaux->u.evt_status.network_status = g->network_status;
		gsmd_log(GSMD_INFO, "Status: modem %d simpres %d sim %d pin %d ntwk %d\n",
			g->modem_status, g->sim_present, g->sim_status,
			g->pin_status, g->network_status);
	}

	return ucmd;
}

int pin_status_changed(struct gsmd *g, int new_pin_status)
{
	int retval = 0;
	if (g->pin_status != new_pin_status) {

		struct gsmd_ucmd* status_event;
		struct gsmd_ucmd *pin_event;
		int type;

		gsmd_log(GSMD_INFO, "pin status changed %d -> %d\n",
			g->pin_status, new_pin_status);

		g->pin_status = new_pin_status;
		type = map_cme_error_to_pin_type(new_pin_status);

		pin_event =
			usock_build_event(GSMD_MSG_EVENT, GSMD_EVT_PIN,
				sizeof(struct gsmd_evt_auxdata));

		if (valid_ucmd(pin_event)) {
			struct gsmd_evt_auxdata *eaux =
				((void *)pin_event) + sizeof(struct gsmd_ucmd);
			eaux->u.pin.type = type;
		}

		retval = usock_evt_send(g, pin_event, GSMD_EVT_PIN);

		status_event = generate_status_event(g);
		retval |= usock_evt_send(g, status_event, GSMD_EVT_STATUS);
	}
	return retval;
}

int sim_present_changed(struct gsmd *g, int new_sim_present)
{
	int retval = 0;
	if (g->sim_present != new_sim_present) {

		struct gsmd_ucmd* status_event;
		gsmd_log(GSMD_INFO, "sim present changed %d -> %d\n",
			g->sim_present, new_sim_present);

		g->sim_present = new_sim_present;

		status_event = generate_status_event(g);
		retval = usock_evt_send(g, status_event, GSMD_EVT_STATUS);
	}
	return retval;
}

int sim_status_changed(struct gsmd *g, int new_sim_status)
{
	int retval = 0;
	if (g->sim_status != new_sim_status) {

		struct gsmd_ucmd* status_event;
		gsmd_log(GSMD_INFO, "sim status changed %d -> %d\n",
			g->sim_status, new_sim_status);

		g->sim_status = new_sim_status;

		status_event = generate_status_event(g);
		retval = usock_evt_send(g, status_event, GSMD_EVT_STATUS);
	}
	return retval;
}

int modem_status_changed(struct gsmd *g, int new_modem_status)
{
	int retval = 0;
	if (g->modem_status != new_modem_status) {

		struct gsmd_ucmd* status_event;
		gsmd_log(GSMD_INFO, "modem status changed %d -> %d\n",
			g->modem_status, new_modem_status);

		g->modem_status = new_modem_status;

		status_event = generate_status_event(g);
		retval = usock_evt_send(g, status_event, GSMD_EVT_STATUS);
	}
	return retval;
}

int network_status_changed(struct gsmd *g, int new_network_status)
{
	int retval = 0;
	if (g->network_status != new_network_status) {

		struct gsmd_ucmd* status_event;
		gsmd_log(GSMD_INFO, "network status changed %d -> %d\n",
			g->network_status, new_network_status);

		g->network_status = new_network_status;

		status_event = generate_status_event(g);
		retval = usock_evt_send(g, status_event, GSMD_EVT_STATUS);
	}
	return retval;
}

int signal_strength_changed(struct gsmd *g, unsigned char rssi, unsigned char ber)
{
	int retval = 0;
	if (rssi > 6) rssi = 6;
	if (ber > 7) ber = 7;
	if (g->rssi_idx != rssi) { // bit error rates are not that interesting ||(g->ber_idx != ber)) {

		struct gsmd_ucmd *ucmd = usock_build_event(GSMD_MSG_EVENT, GSMD_EVT_SIGNAL,
							 sizeof(struct gsmd_evt_auxdata));
		g->rssi_idx = rssi;
		g->ber_idx = ber;

		if (valid_ucmd(ucmd)) {
			struct gsmd_evt_auxdata *aux = (struct gsmd_evt_auxdata *) ucmd->buf;
			static int rssi_table[] = { 0,5,10,15,20,25,99 };

			// TODO what is the best rssi notation, 0 to 6 would seem to be easier for the UI
			aux->u.signal.sigq.rssi = rssi; //rssi_table[rssi];
			aux->u.signal.sigq.ber = ber;
		}

		retval = usock_evt_send(g, ucmd, GSMD_EVT_SIGNAL);
	}
	return retval;
}


