#ifndef __GSMD_ATCMD_H
#define __GSMD_ATCMD_H

#ifdef __GSMD__

#include <gsmd/gsmd.h>

typedef int atcmd_cb_t(struct gsmd_atcmd *cmd, void *ctx, char *resp);

extern struct gsmd_atcmd *atcmd_fill(const char *cmd, int rlen, atcmd_cb_t *cb,
	void *ctx, struct gsmd_msg_hdr* gph);
extern int atcmd_submit_highpriority(struct gsmd *g, struct gsmd_atcmd *cmd,
	u_int8_t channel);
extern int atcmd_submit(struct gsmd *g, struct gsmd_atcmd *cmd,
	u_int8_t channel);
extern int cancel_current_atcmd(struct gsmd *g, struct gsmd_atcmd *cmd,
	u_int8_t channel);
extern int cancel_specific_atcmd(struct gsmd *g, int type, int subtype, int id);
extern void cancel_pending_atcmds(struct gsmd *g, u_int8_t channel);
extern void cancel_all_atcmds(struct gsmd *g);
extern int atcmd_revert_to_single_port_mode(struct gsmd *g);
extern int atcmd_init(struct gsmd *g, int sockfd);
extern void atcmd_drain(int fd);
extern int atcmd_terminate_matching(struct gsmd *g, void *ctx);
extern void atcmd_wake_pending_queue (struct gsmd *g, u_int8_t channel);
extern void atcmd_wait_pending_queue (struct gsmd *g, u_int8_t channel);

#endif /* __GSMD__ */

#endif
