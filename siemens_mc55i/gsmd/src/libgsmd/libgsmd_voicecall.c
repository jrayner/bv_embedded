/* libgsmd voice call support
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

#include <libgsmd/voicecall.h>

#include "lgsm_internals.h"

int lgsm_voice_out_init(struct lgsm_handle *lh,
			const struct lgsm_addr *number)
{
	struct gsmd_msg_hdr *gmh;
	struct gsmd_addr *ga;

	gmh = lgsm_gmh_fill(GSMD_MSG_VOICECALL,
			    GSMD_VOICECALL_DIAL, sizeof(*ga));
	if (!gmh)
		return -ENOMEM;

	ga = (struct gsmd_addr *) gmh->data;
	ga->type = number->type;	/* FIXME: is this correct? */
	memcpy(ga->number, number->addr, sizeof(ga->number));
	ga->number[sizeof(ga->number)-1] = '\0';

	return lgsm_send_then_free_gmh(lh, gmh);
}

int lgsm_voice_dtmf(struct lgsm_handle *lh, char dtmf_char)
{
	struct gsmd_msg_hdr *gmh;
	struct gsmd_dtmf *gd;

	gmh = lgsm_gmh_fill(GSMD_MSG_VOICECALL,
			    GSMD_VOICECALL_DTMF, sizeof(*gd)+1);
	if (!gmh)
		return -ENOMEM;

	gd = (struct gsmd_dtmf *) gmh->data;
	gd->len = 1;
	gd->dtmf[0] = dtmf_char;

	return lgsm_send_then_free_gmh(lh, gmh);
}

int lgsm_voice_in_accept(struct lgsm_handle *lh)
{
	return lgsm_send_simple(lh, GSMD_MSG_VOICECALL, GSMD_VOICECALL_ANSWER);
}

int lgsm_voice_hangup(struct lgsm_handle *lh)
{
	return lgsm_send_simple(lh, GSMD_MSG_VOICECALL, GSMD_VOICECALL_HANGUP);
}

int lgsm_voice_volume_set(struct lgsm_handle *lh, int volume)
{
	/* FIXME: we need to pass along the parameter */
	return lgsm_send_simple(lh, GSMD_MSG_VOICECALL, GSMD_VOICECALL_VOL_SET);
}

int lgsm_voice_cancel(struct lgsm_handle *lh, int subtype)
{
	switch(subtype) {
	case GSMD_VOICECALL_DIAL:
	case GSMD_VOICECALL_HANGUP:
	case GSMD_VOICECALL_ANSWER:
	case GSMD_VOICECALL_DTMF:
	case GSMD_VOICECALL_VOL_SET:
		return lgsm_cancel(lh, GSMD_MSG_VOICECALL, subtype, 0);
	}
	return -EINVAL;
}

