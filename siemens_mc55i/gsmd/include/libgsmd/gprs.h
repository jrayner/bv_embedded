#ifndef _LGSM_GPRS_H
#define _LGSM_GPRS_H

extern int lgsm_gprs_attach(struct lgsm_handle *lh);

extern int lgsm_gprs_detach(struct lgsm_handle *lh);

extern int lgsm_gprs_activate_context(struct lgsm_handle *lh, int context);

extern int lgsm_gprs_deactivate_context(struct lgsm_handle *lh, int context);

extern int lgsm_gprs_connect_context(struct lgsm_handle *lh, int context);

extern int lgsm_gprs_get_context_ip_address(struct lgsm_handle *lh, int context);

extern int lgsm_gprs_set_context_config(struct lgsm_handle *lh, int context,
	char* access_point_name);

extern int lgsm_gprs_cancel(struct lgsm_handle *lh, int subtype);

#endif
