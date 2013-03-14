/* libgsmd network related functions
 *
 * (C) 2006-2007 by OpenMoko, Inc.
 * Written by Harald Welte <laforge@openmoko.org>
 * All Rights Reserved
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

#include <libgsmd/libgsmd.h>
#include <libgsmd/misc.h>

#include <gsmd/usock.h>
#include <gsmd/event.h>

#include "lgsm_internals.h"

/* Get the current network registration status */
#if 0
int lgsm_get_netreg_state(struct lgsm_handle *lh,
		enum lgsm_netreg_state *state)
{
	*state = lh->netreg_state;
	return 0;
}
#endif

int lgsm_oper_get(struct lgsm_handle *lh)
{
	return lgsm_send_simple(lh, GSMD_MSG_NETWORK, GSMD_NETWORK_OPER_GET);
}

int lgsm_opers_get(struct lgsm_handle *lh)
{
	return lgsm_send_simple(lh, GSMD_MSG_NETWORK, GSMD_NETWORK_OPER_LIST);
}

int lgsm_netreg_register(struct lgsm_handle *lh, gsmd_oper_numeric oper)
{
	struct gsmd_msg_hdr *gmh;

	gmh = lgsm_gmh_fill(GSMD_MSG_NETWORK, GSMD_NETWORK_REGISTER,
			sizeof(gsmd_oper_numeric));
	if (!gmh)
		return -ENOMEM;

	memcpy(gmh->data, oper, sizeof(gsmd_oper_numeric));

	return lgsm_send_then_free_gmh(lh, gmh);
}

int lgsm_netreg_deregister(struct lgsm_handle *lh)
{
	return lgsm_send_simple(lh, GSMD_MSG_NETWORK, GSMD_NETWORK_DEREGISTER);
}

int lgsm_signal_quality(struct lgsm_handle *lh)
{
	return lgsm_send_simple(lh, GSMD_MSG_NETWORK, GSMD_NETWORK_SIGQ_GET);
}

int lgsm_prefoper_list(struct lgsm_handle *lh)
{
	return lgsm_send_simple(lh, GSMD_MSG_NETWORK, GSMD_NETWORK_PREF_LIST);
}

int lgsm_prefoper_delete(struct lgsm_handle *lh, int index)
{
	struct gsmd_msg_hdr *gmh;

	gmh = lgsm_gmh_fill(GSMD_MSG_NETWORK, GSMD_NETWORK_PREF_DEL,
			sizeof(int));
	if (!gmh)
		return -ENOMEM;

	memcpy(gmh->data, &index, sizeof(int));

	return lgsm_send_then_free_gmh(lh, gmh);
}

int lgsm_prefoper_add(struct lgsm_handle *lh, gsmd_oper_numeric oper)
{
	struct gsmd_msg_hdr *gmh;

	gmh = lgsm_gmh_fill(GSMD_MSG_NETWORK, GSMD_NETWORK_PREF_ADD,
			sizeof(gsmd_oper_numeric));
	if (!gmh)
		return -ENOMEM;

	memcpy(gmh->data, oper, sizeof(gsmd_oper_numeric));

	return lgsm_send_then_free_gmh(lh, gmh);
}

int lgsm_prefoper_get_space(struct lgsm_handle *lh)
{
	return lgsm_send_simple(lh, GSMD_MSG_NETWORK, GSMD_NETWORK_PREF_SPACE);
}

int lgsm_get_subscriber_num(struct lgsm_handle *lh)
{
	return lgsm_send_simple(lh, GSMD_MSG_NETWORK, GSMD_NETWORK_GET_NUMBER);
}

int lgsm_get_registration_status(struct lgsm_handle *lh)
{
	return lgsm_send_simple(lh, GSMD_MSG_NETWORK, GSMD_NETWORK_GET_REG_ST);
}


int lgsm_net_cancel(struct lgsm_handle *lh, int subtype)
{
	switch(subtype) {
	case GSMD_NETWORK_REGISTER:
	case GSMD_NETWORK_SIGQ_GET:
	case GSMD_NETWORK_VMAIL_GET:
	case GSMD_NETWORK_VMAIL_SET:
	case GSMD_NETWORK_OPER_GET:
	case GSMD_NETWORK_OPER_LIST:
	case GSMD_NETWORK_CIND_GET:
	case GSMD_NETWORK_DEREGISTER:
	case GSMD_NETWORK_GET_NUMBER:
	case GSMD_NETWORK_PREF_LIST:
	case GSMD_NETWORK_PREF_DEL:
	case GSMD_NETWORK_PREF_ADD:
	case GSMD_NETWORK_PREF_SPACE:
		return lgsm_cancel(lh, GSMD_MSG_NETWORK, subtype, 0);
	}
	return -EINVAL;
}

