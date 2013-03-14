#ifndef __GSMD_UNSOLICITED_H
#define __GSMD_UNSOLICITED_H

#ifdef __GSMD__

#include <gsmd/gsmd.h>

struct gsmd_unsolicit {
	const char *prefix;
	int (*parse)(char *unsol, int len, const char *param, struct gsmd *gsmd);
};

extern int unsolicited_parse(struct gsmd *g, char *buf, int len, const char *param, u_int8_t channel);
extern int map_cme_error_to_pin_type(unsigned int cme_error);
extern int generate_event_from_cme(struct gsmd *g, int neg_cme_error_num);
extern struct gsmd_ucmd* generate_status_event(struct gsmd *g);
extern int pin_status_changed(struct gsmd *g, int new_pin_status);
extern int sim_present_changed(struct gsmd *g, int new_sim_present);
extern int sim_status_changed(struct gsmd *g, int new_sim_status);
extern int modem_status_changed(struct gsmd *g, int new_modem_status);
extern int network_status_changed(struct gsmd *g, int new_network_status);
extern int signal_strength_changed(struct gsmd *g,
    unsigned char rssi, unsigned char ber);

extern void unsolicited_generic_init(struct gsmd *g);
extern int unsolicited_register_array(const struct gsmd_unsolicit *arr,
		int len);

extern int creg_parse(char *buf, int len, const char *param, struct gsmd *gsmd);

#endif /* __GSMD__ */

#endif
