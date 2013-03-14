/* libgsmd_test.c
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

/* These API should only be included in test/debug builds */

int lgsm_test_enable_scenario(struct lgsm_handle *lh, int type, int subtype,
	const char* scenario)
{
	struct gsmd_msg_hdr *gmh;
	struct gsmd_test_scenario* gtc;

	gmh = lgsm_gmh_fill(GSMD_MSG_TEST, GSMD_TEST_SET_SCENARIO,
		sizeof(struct gsmd_test_scenario));
	if (!gmh)
		return -ENOMEM;

	gtc = (struct gsmd_test_scenario*) gmh->data;
	gtc->type = type;
	gtc->subtype = subtype;
	strcpy((char*)gtc->scenario, scenario);

	return lgsm_send_then_free_gmh(lh, gmh);
}

int lgsm_test_schedule_event(struct lgsm_handle *lh, int event_subtype,
	struct timeval relative_time)
{
	struct gsmd_msg_hdr *gmh;
	struct gsmd_test_event* gte;

	gmh = lgsm_gmh_fill(GSMD_MSG_TEST, GSMD_TEST_SCHEDULE_EVENT,
		sizeof(struct gsmd_test_event));
	if (!gmh)
		return -ENOMEM;

	gte = (struct gsmd_test_event*) gmh->data;
	gte->event_subtype = event_subtype;
	memcpy(&gte->rel_time, &relative_time, sizeof(struct timeval));

	return lgsm_send_then_free_gmh(lh, gmh);
}


