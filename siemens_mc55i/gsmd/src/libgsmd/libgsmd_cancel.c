/* libgsmd_cancel.c
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
#include <libgsmd/libgsmd.h>

#include "lgsm_internals.h"

int lgsm_cancel(struct lgsm_handle *lh, int type, int subtype, int id)
{
	struct gsmd_msg_hdr *gmh;
	struct gsmd_cancel* cancel;
	int rc;

	gmh = lgsm_gmh_fill(GSMD_MSG_CANCEL_CMD, 0, sizeof(struct gsmd_cancel));
	if (!gmh)
		return -ENOMEM;

	cancel = (struct gsmd_cancel*) gmh->data;
	cancel->type = type;
	cancel->subtype = subtype;
	cancel->id = id;

	return lgsm_send_then_free_gmh(lh, gmh);
}

