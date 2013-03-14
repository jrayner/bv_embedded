/* libgsmd_ussd.c
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

#include <libgsmd/ussd.h>

#include "lgsm_internals.h"

int lgsm_send_ussd(struct lgsm_handle *lh, const char* data)
{
	struct gsmd_msg_hdr *gmh;

	gmh = lgsm_gmh_fill(GSMD_MSG_USSD,
			    GSMD_USSD_SEND, strlen(data)+1);
	if (!gmh)
		return -ENOMEM;

	strcpy(gmh->data,data);

	return lgsm_send_then_free_gmh(lh, gmh);
}

int lgsm_ussd_response(struct lgsm_handle *lh, const char* response)
{
	struct gsmd_msg_hdr *gmh;
	int resp_len = 0;
	if (response)
		resp_len = strlen(response)+1;

	gmh = lgsm_gmh_fill(GSMD_MSG_USSD,
			    GSMD_USSD_RSP, resp_len);
	if (!gmh)
		return -ENOMEM;

	if (resp_len)
		strcpy(gmh->data,response);

	return lgsm_send_then_free_gmh(lh, gmh);
}


int lgsm_ussd_cancel(struct lgsm_handle *lh, int subtype)
{
	switch(subtype) {
	case GSMD_USSD_SEND:
		return lgsm_cancel(lh, GSMD_MSG_USSD, subtype, 0);
	}
	return -EINVAL;
}
