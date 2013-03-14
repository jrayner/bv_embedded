#ifndef _LGSM_PIN_H
#define _LGSM_PIN_H

extern const char *lgsm_pin_name(enum gsmd_pin_type ptype);

extern int lgsm_enter_pin(struct lgsm_handle *lh, unsigned int type, const char *pin);
extern int lgsm_enter_puk(struct lgsm_handle *lh, unsigned int type, const char *puk, const char *newpin);

extern int lgsm_chg_lock(struct lgsm_handle *lh, enum gsmd_sim_lock lock, const char *pin);

extern int lgsm_chg_pin(struct lgsm_handle *lh, const char *pin, const char *newpin);

#endif
