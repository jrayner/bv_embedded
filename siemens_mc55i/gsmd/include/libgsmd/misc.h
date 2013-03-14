#ifndef _LIBGSMD_MISC_H
#define _LIBGSMD_MISC_H

/* libgsmd.h - Library API for gsmd, the GSM Daemon
 * (C) 2006 by Harald Welte <hwelte@hmw-consulting.de>
 * Development funded by First International Computers, Inc.
 */

#include <libgsmd/libgsmd.h>

extern int lgsm_phone_power(struct lgsm_handle *lh, int power);

extern int lgsm_modem_powersave(struct lgsm_handle *lh);
extern int lgsm_modem_flightmode(struct lgsm_handle *lh);
extern int lgsm_modem_shutdown(struct lgsm_handle *lh);

extern int lgsm_phone_suspend(struct lgsm_handle *lh);
extern int lgsm_phone_resume(struct lgsm_handle *lh);

extern int lgsm_phone_av_current(struct lgsm_handle *lh);
extern int lgsm_phone_voltage(struct lgsm_handle *lh);

enum lgsm_info_type {
	LGSM_INFO_TYPE_NONE		= 0,
	LGSM_INFO_TYPE_MANUF		= 1,
	LGSM_INFO_TYPE_MODEL		= 2,
	LGSM_INFO_TYPE_REVISION		= 3,
	LGSM_INFO_TYPE_SERIAL		= 4
};

/* Get some information about the handset */
extern int lgsm_phone_get_info(struct lgsm_handle *lh,
			 enum lgsm_info_type type);

extern int lgsm_phone_cancel(struct lgsm_handle *lh, int subtype);

/* General Commands */

/* Get Signal Strength (Chapter 8.5) */
extern int lgsm_signal_quality(struct lgsm_handle *h);

#if 0
/* Not impl */
/* Set voice mail number */
extern int lgsm_voicemail_set(struct lgsm_handle *lh,
			      struct lgsm_addr *addr);

/* Get currently configured voice mail number */
extern int lgsm_voicemail_get(struct lgsm_handle *lh,
			      struct lgsm_addr *addr);
#endif

/* Operator Selection, Network Registration */
extern int lgsm_oper_get(struct lgsm_handle *lh);
extern int lgsm_opers_get(struct lgsm_handle *lh);
extern int lgsm_netreg_register(struct lgsm_handle *lh,
		gsmd_oper_numeric oper);
extern int lgsm_netreg_deregister(struct lgsm_handle *lh);

enum lgsm_netreg_state {
	LGSM_NETREG_ST_NOTREG		= 0,
	LGSM_NETREG_ST_REG_HOME		= 1,
	LGSM_NETREG_ST_NOTREG_SEARCH	= 2,
	LGSM_NETREG_ST_DENIED		= 3,
	LGSM_NETREG_ST_UNKNOWN		= 4,
	LGSM_NETREG_ST_REG_ROAMING	= 5,
};

#if 0
/* Bad impl */
/* Get the current network registration status */
extern int lgsm_get_netreg_state(struct lgsm_handle *lh,
				 enum lgsm_netreg_state *state);
#endif

extern int lgsm_get_registration_status(struct lgsm_handle *lh);

/* Preferred operator list management */
extern int lgsm_prefoper_list(struct lgsm_handle *lh);
extern int lgsm_prefoper_delete(struct lgsm_handle *lh, int index);
extern int lgsm_prefoper_add(struct lgsm_handle *lh, gsmd_oper_numeric oper);
extern int lgsm_prefoper_get_space(struct lgsm_handle *lh);

extern int lgsm_net_cancel(struct lgsm_handle *lh, int subtype);

/* Get subscriber's own phone number */
extern int lgsm_get_subscriber_num(struct lgsm_handle *lh);

/* CLIP, CLIR, COLP, Call Forwarding, Call Waiting, Call Deflecting */
/* TBD */

/* GPRS related functions */
/* TBD */


#endif
