/* libgsmd_pin.c
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

static const char *pin_type_names[__NUM_GSMD_PIN] = {
	[GSMD_PIN_NONE]		= "NONE",
	[GSMD_PIN_SIM_PIN]	= "SIM PIN",
	[GSMD_PIN_SIM_PUK]	= "SIM PUK",
	[GSMD_PIN_PH_SIM_PIN]	= "Phone-to-SIM PIN",
	[GSMD_PIN_PH_FSIM_PIN]	= "Phone-to-very-first SIM PIN",
	[GSMD_PIN_PH_FSIM_PUK]	= "Phone-to-very-first SIM PUK",
	[GSMD_PIN_SIM_PIN2]	= "SIM PIN2",
	[GSMD_PIN_SIM_PUK2]	= "SIM PUK2",
	[GSMD_PIN_PH_NET_PIN]	= "Network personalisation PIN",
	[GSMD_PIN_PH_NET_PUK]	= "Network personalisation PUK",
	[GSMD_PIN_PH_NETSUB_PIN]= "Network subset personalisation PIN",
	[GSMD_PIN_PH_NETSUB_PUK]= "Network subset personalisation PUK",
	[GSMD_PIN_PH_SP_PIN]	= "Service provider personalisation PIN",
	[GSMD_PIN_PH_SP_PUK]	= "Service provider personalisation PUK",
	[GSMD_PIN_PH_CORP_PIN]	= "Corporate personalisation PIN",
	[GSMD_PIN_PH_CORP_PUK]	= "Corporate personalisation PUK",
};

const char *lgsm_pin_name(enum gsmd_pin_type ptype)
{
	if (ptype >= __NUM_GSMD_PIN)
		return "unknown";

	return pin_type_names[ptype];
}

int lgsm_enter_pin(struct lgsm_handle *lh, unsigned int stype, const char *pin)
{
	int subtype = GSMD_PIN_INPUT;
	struct gsmd_msg_hdr *gmh;
	struct gsmd_pin* gp;

	if (!pin || !*pin || strlen(pin) > GSMD_PIN_MAXLEN || 
		stype >= __NUM_GSMD_PIN) {
		return -EINVAL;
	}
	if (stype == GSMD_PIN_SIM_PIN2)
		subtype = GSMD_PIN2_INPUT;

	gmh = (void *) lgsm_gmh_fill(GSMD_MSG_PIN, subtype, sizeof(struct gsmd_pin));
	if (!gmh)
		return -ENOMEM;

	gp = (struct gsmd_pin*) gmh->data;
	gp->type = stype;
	strcat(gp->pin, pin);
	gp->newpin[0] = '\0'; /* not used */

	return lgsm_send_then_free_gmh(lh, gmh);
}

int lgsm_enter_puk(struct lgsm_handle *lh, unsigned int stype, const char *puk, const char *newpin)
{
	int subtype = GSMD_PIN_INPUT;
	struct gsmd_msg_hdr *gmh;
	struct gsmd_pin* gp;

	if (!puk || !newpin || strlen(puk) != GSMD_PIN_MAXLEN || 
		strlen(newpin) > GSMD_PIN_MAXLEN || stype >= __NUM_GSMD_PIN) {
		return -EINVAL;
	}
	if (stype == GSMD_PIN_SIM_PUK2)
		subtype = GSMD_PIN2_INPUT;

	gmh = (void *) lgsm_gmh_fill(GSMD_MSG_PIN, subtype, sizeof(struct gsmd_pin));
	if (!gmh)
		return -ENOMEM;

	gp = (struct gsmd_pin*) gmh->data;
	gp->type = stype;
	strcat(gp->pin, puk);
	strcat(gp->newpin, newpin);

	return lgsm_send_then_free_gmh(lh, gmh);
}

int lgsm_chg_lock(struct lgsm_handle *lh, enum gsmd_sim_lock lock, const char *pin)
{
	struct gsmd_msg_hdr *gmh;
	struct gsmd_pin* gp;

	if (!pin || !*pin || strlen(pin) > GSMD_PIN_MAXLEN)
		return -EINVAL;

	gmh = (void *) lgsm_gmh_fill(GSMD_MSG_PIN, GSMD_CHG_LOCK, sizeof(struct gsmd_pin));
	if (!gmh)
		return -ENOMEM;

	gp = (struct gsmd_pin*) gmh->data;
	gp->lock = lock;
	strcat(gp->pin, pin);

	return lgsm_send_then_free_gmh(lh, gmh);
}

int lgsm_chg_pin(struct lgsm_handle *lh, const char *pin, const char *newpin)
{
	struct gsmd_msg_hdr *gmh;
	struct gsmd_pin* gp;

	if (!pin || !*pin || strlen(pin) > GSMD_PIN_MAXLEN ||
		!newpin || !*newpin || strlen(newpin) > GSMD_PIN_MAXLEN) {
		return -EINVAL;
	}
	gmh = (void *) lgsm_gmh_fill(GSMD_MSG_PIN, GSMD_CHG_PIN, sizeof(struct gsmd_pin));
	if (!gmh)
		return -ENOMEM;

	gp = (struct gsmd_pin*) gmh->data;
	strcat(gp->pin, pin);
	strcat(gp->newpin, newpin);

	return lgsm_send_then_free_gmh(lh, gmh);
}


