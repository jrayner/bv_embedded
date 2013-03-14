/* libgsmd phone related functions
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

int lgsm_phone_power(struct lgsm_handle *lh, int power)
{
	int subtype, retval;

	if (power)
		subtype = GSMD_PHONE_POWERUP;
	else
		subtype = GSMD_PHONE_POWERDOWN;

	return lgsm_send_simple(lh, GSMD_MSG_PHONE, subtype);
}

int lgsm_modem_powersave(struct lgsm_handle *lh)
{
	return lgsm_send_simple(lh, GSMD_MSG_PHONE, GSMD_PHONE_POWERSAVE);
}

int lgsm_modem_flightmode(struct lgsm_handle *lh)
{
	return lgsm_send_simple(lh, GSMD_MSG_PHONE, GSMD_PHONE_FLIGHTMODE);
}

int lgsm_modem_shutdown(struct lgsm_handle *lh)
{
	return lgsm_send_simple(lh, GSMD_MSG_PHONE, GSMD_PHONE_SHUTDOWN);
}

int lgsm_phone_suspend(struct lgsm_handle *lh)
{
	return lgsm_send_simple(lh, GSMD_MSG_PHONE, GSMD_PHONE_SUSPEND);
}

int lgsm_phone_resume(struct lgsm_handle *lh)
{
	return lgsm_send_simple(lh, GSMD_MSG_PHONE, GSMD_PHONE_RESUME);
}

int lgsm_phone_av_current(struct lgsm_handle *lh)
{
	return lgsm_send_simple(lh, GSMD_MSG_PHONE, GSMD_PHONE_AV_CURRENT);
}

int lgsm_phone_voltage(struct lgsm_handle *lh)
{
	return lgsm_send_simple(lh, GSMD_MSG_PHONE, GSMD_PHONE_VOLTAGE);
}

int lgsm_phone_get_info(struct lgsm_handle *lh,
			 enum lgsm_info_type type)
{
	switch(type) {
	case LGSM_INFO_TYPE_MANUF:
		return lgsm_send_simple(lh, GSMD_MSG_PHONE, GSMD_PHONE_GET_MANUF);
	case LGSM_INFO_TYPE_MODEL:
		return lgsm_send_simple(lh, GSMD_MSG_PHONE, GSMD_PHONE_GET_MODEL);
	case LGSM_INFO_TYPE_REVISION:
		return lgsm_send_simple(lh, GSMD_MSG_PHONE, GSMD_PHONE_GET_REVISION);
	case LGSM_INFO_TYPE_SERIAL:
		return lgsm_send_simple(lh, GSMD_MSG_PHONE, GSMD_PHONE_GET_IMEI);
	default:
		break;
	}
	return -EINVAL;
}

int lgsm_phone_cancel(struct lgsm_handle *lh, int subtype)
{
	switch(subtype) {
	case GSMD_PHONE_GET_MANUF:
	case GSMD_PHONE_GET_MODEL:
	case GSMD_PHONE_GET_REVISION:
	case GSMD_PHONE_GET_IMEI:
	case GSMD_PHONE_POWERUP:
	case GSMD_PHONE_POWERDOWN:
	case GSMD_PHONE_POWERSAVE:
	case GSMD_PHONE_FLIGHTMODE:
	case GSMD_PHONE_AV_CURRENT:
	case GSMD_PHONE_VOLTAGE:
		return lgsm_cancel(lh, GSMD_MSG_PHONE, subtype, 0);
	}
	return -EINVAL;
}

