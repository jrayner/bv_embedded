/* libgsmd_gprs.c
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
#include <string.h>
#include <stdio.h>

#include <sys/types.h>
#include <gsmd/event.h>
#include <libgsmd/libgsmd.h>

#include "lgsm_internals.h"

int lgsm_gprs_attach(struct lgsm_handle *lh)
{
	return lgsm_send_simple(lh,GSMD_MSG_GPRS, GSMD_GPRS_ATTACH);
}

int lgsm_gprs_detach(struct lgsm_handle *lh)
{
	return lgsm_send_simple(lh,GSMD_MSG_GPRS, GSMD_GPRS_DETACH);
}

static int send_gprs_request(struct lgsm_handle *lh, int subtype,
	int context)
{
	struct gsmd_msg_hdr *gmh;
	int rc;

	gmh = lgsm_gmh_fill(GSMD_MSG_GPRS,
			subtype, sizeof(int));
	if (!gmh)
		return -ENOMEM;
	*(int *) gmh->data = context;

	return lgsm_send_then_free_gmh(lh, gmh);
}

int lgsm_gprs_activate_context(struct lgsm_handle *lh, int context)
{
	return send_gprs_request(lh, GSMD_GPRS_ACTIVATE, context);
}

int lgsm_gprs_deactivate_context(struct lgsm_handle *lh, int context)
{
	return send_gprs_request(lh, GSMD_GPRS_DEACTIVATE, context);
}

int lgsm_gprs_connect_context(struct lgsm_handle *lh, int context)
{
	return send_gprs_request(lh, GSMD_GPRS_CONNECT, context);
}

int lgsm_gprs_get_context_ip_address(struct lgsm_handle *lh, int context)
{
	return send_gprs_request(lh, GSMD_GPRS_GET_IP_ADDR, context);
}

int lgsm_gprs_set_context_config(struct lgsm_handle *lh, int context,
	char* access_point_name)
{
	struct gsmd_msg_hdr *gmh;
	struct gsmd_gprs_set_config* config;
	int rc;

	if (!access_point_name)
		return -EINVAL;

	gmh = lgsm_gmh_fill(GSMD_MSG_GPRS, GSMD_GPRS_SET_CONFIG,
		sizeof(struct gsmd_gprs_set_config)); // TODO size of context & apn
	if (!gmh)
		return -ENOMEM;

	config = (struct gsmd_gprs_set_config*) gmh->data;
	config->context = context;
	strcpy((char*) config->apn, access_point_name);

	return lgsm_send_then_free_gmh(lh, gmh);
}

int lgsm_gprs_cancel(struct lgsm_handle *lh, int subtype)
{
	switch(subtype) {
	case GSMD_GPRS_ATTACH:
	case GSMD_GPRS_DETACH:
	case GSMD_GPRS_ACTIVATE:
	case GSMD_GPRS_DEACTIVATE:
	case GSMD_GPRS_CONNECT:
	case GSMD_GPRS_GET_IP_ADDR:
	case GSMD_GPRS_SET_CONFIG:
		return lgsm_cancel(lh, GSMD_MSG_GPRS, subtype, 0);
	}
	return -EINVAL;
}


