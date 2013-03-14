#ifndef _LIBGSMD_USSD_H
#define _LIBGSMD_USSD_H

#include <libgsmd/libgsmd.h>

/* USSD */
extern int lgsm_send_ussd(struct lgsm_handle *lh, const char* data);

extern int lgsm_ussd_response(struct lgsm_handle *lh, const char* response);

/* Cancel specified ussd request */
extern int lgsm_ussd_cancel(struct lgsm_handle *lh, int subtype);

#endif
