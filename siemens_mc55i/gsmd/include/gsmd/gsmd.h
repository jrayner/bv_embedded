#ifndef _GSMD_H
#define _GSMD_H

#ifdef __GSMD__

#include <sys/types.h>
#include <sys/ioctl.h>

#include <common/linux_list.h>

#include <gsmd/machineplugin.h>
#include <gsmd/vendorplugin.h>
#include <gsmd/select.h>
#include <gsmd/state.h>

void *gsmd_tallocs;

/* Refer to 3GPP TS 07.07 v 7.8.0, Chapter 4.1 */
#define LGSM_ATCMD_F_EXTENDED	0x01	/* as opposed to basic */
#define LGSM_ATCMD_F_PARAM	0x02	/* as opposed to action */
#define LGSM_ATCMD_F_LFCR	0x04	/* accept LFCR as a line terminator */
#define LGSM_ATCMD_F_LFLF	0x08	/* accept LFLF as a line terminator */

#define ATCMD_WAKEUP_MODEM  0x20
#define ATCMD_PIN_SENSITIVE 0x40	/* delay the command if sim, pin or puk is required */
#define ATCMD_FINAL_CB_FLAG 0x80

#define PIN_DEPENDENT 1
#define NO_PIN_DEPEND 0

struct gsmd_atcmd {
	struct llist_head list;
	void *ctx;
	int (*cb)(struct gsmd_atcmd *cmd, void *ctx, char *resp);
	char *resp;
	u_int32_t resplen;
	int32_t ret;
	u_int32_t buflen;
	u_int16_t id;
	u_int8_t flags;
	u_int8_t timeout_value;
	u_int8_t initial_delay_secs;
	u_int8_t cmd_retries;
	char *cur;
	struct gsmd_msg_hdr* gph;
	char buf[];
};

enum llparse_state {
	LLPARSE_STATE_IDLE,		/* idle, not parsing a response */
	LLPARSE_STATE_IDLE_CR,		/* CR before response (V1) */
	LLPARSE_STATE_IDLE_LF,		/* LF before response (V1) */
	LLPARSE_STATE_RESULT,		/* within result payload */
	LLPARSE_STATE_RESULT_CR,	/* CR after result */
	LLPARSE_STATE_RESULT_LF,	/* LF after result */
	LLPARSE_STATE_PROMPT,		/* within a "> " prompt */
	LLPARSE_STATE_PROMPT_SPC,	/* a complete "> " prompt */
	LLPARSE_STATE_ERROR,		/* something went wrong */
					/* ... idle again */
	LLPARSE_STATE_QUOTE,		/* within a " quote" */
};

/* we can't take any _single_ response bigger than this: */
#define LLPARSE_BUF_SIZE	1024

/* we can't parse a mutiline response bigger than this: */
#define MLPARSE_BUF_SIZE	65535

struct llparser {
	enum llparse_state state;
	unsigned int len;
	unsigned int flags;
	void *ctx;
	int (*cb)(const char *buf, int len, void *ctx, u_int8_t channel);
	int (*prompt_cb)(void *ctx, u_int8_t channel);
	char *cur;
	char buf[LLPARSE_BUF_SIZE];
	u_int8_t channel;
};

/***********************************************************************
 * timer handling
 ***********************************************************************/

struct gsmd_timer {
	struct llist_head list;
	struct timeval expires;
	void (*cb)(struct gsmd_timer *tmr, void *data);
	void *data;
};

int gsmd_timer_init(void);
void gmsd_timer_check_n_run(void);

struct gsmd_timer *gsmd_timer_alloc(void);
int gsmd_timer_register(struct gsmd_timer *timer);
void gsmd_timer_unregister(struct gsmd_timer *timer);

struct gsmd_timer *gsmd_timer_create(struct timeval *expires,
				     void (*cb)(struct gsmd_timer *tmr, void *data), void *data);

int gsmd_timer_set(struct gsmd_timer *tmr,
				     struct timeval *expires,
				     void (*cb)(struct gsmd_timer *tmr, void *data),
				     void *data);

void remove_all_timers();

#define gsmd_timer_free(x) talloc_free(x)

#define GSMD_FLAG_V0		0x0001	/* V0 responses to be expected from TA */
#define GSMD_FLAG_SMS_FMT_TEXT	0x0002	/* TODO Use TEXT rather than PDU mode */

#define GSMD_MODEM_WAKEUP_TIMEOUT     3

#define GSMD_MAX_CHANNELS 3	/* Siemens MC55x has a max of 3 chnls */
#define GSMD_CMD_CHANNEL0 0	/* This should always be zero, carries majority of cmds */

/* Channel config for Infracharge - using only 2 channels */
#define GSMD_NOTIFS_CHANNEL 0
#define GSMD_ATH_CMD_CHANNEL 0
#define GSMD_GPRS_DATA_CHANNEL 1



#define GSMD_ALIVE_CMD_CHANNEL 1 /* TODO remove the horrible gsmd alive timer code */

static int mux_disc_index = N_MOUSE; /* TODO needs own ldisc id if moved upstream */

struct gsmd {
	unsigned int flags;
	unsigned int number_channels;
	int interpreter_ready;
	struct gsmd_fd gfd_uart[GSMD_MAX_CHANNELS];
	int dummym_enabled;
	struct gsmd_fd gfd_dummym[GSMD_MAX_CHANNELS];
	struct gsmd_fd gfd_sock;
	struct llparser llp[GSMD_MAX_CHANNELS];
	struct llist_head users;
	struct llist_head pending_atcmds[GSMD_MAX_CHANNELS];	/* our pending gsmd_atcmds */
	struct llist_head busy_atcmds[GSMD_MAX_CHANNELS];	/* our busy gsmd_atcmd (should only be one per channel) */
	struct gsmd_timer timeout[GSMD_MAX_CHANNELS];
	struct gsmd_timer chl_100ms_wait[GSMD_MAX_CHANNELS]; /* Siemens recommend 100ms between end of a cmd on a chl and sending the next one */
	struct gsmd_machine_plugin *machinepl;
	struct gsmd_vendor_plugin *vendorpl;
	struct gsmd_device_state dev_state;

	struct llist_head operators;		/* cached list of operator names */
	unsigned char *mlbuf[GSMD_MAX_CHANNELS];		/* ml_parse buffer */
	unsigned int mlbuf_len[GSMD_MAX_CHANNELS];
	int mlunsolicited[GSMD_MAX_CHANNELS];
	/* struct gsmd_timer *wakeup_timer; */
	struct llist_head free_ucmd_hdr;   /* ucmd hdrs */
	u_int8_t num_free_ucmd_hdrs;
	u_int8_t max_free_ucmd_hdrs;
	struct gsmd_timer *reset_timer;
	struct gsmd_timer *nak_timer;
	/* cached state  - TODO move these to gsmd_device_state */
	int num_of_clients;
	int modem_status;
	int running;
	int suspended;
	int modem_fd;
	int pin_status;
	int pin_sensitive_cmds_waiting;
	int sim_present;
	int sim_status;
	u_int8_t network_status;
	u_int8_t roaming_status;
	u_int8_t rssi_idx;
	u_int8_t ber_idx;
	u_int8_t sim_inserted_retry_count;
	u_int8_t sim_busy_retry_count;
	struct gsmd_timer *sim_inserted_retry_timer;
	struct gsmd_timer *sim_busy_retry_timer;
	struct gsmd_atcmd *sim_busy_dreq;
};

/* rough (tune-able) number of simultaneous request/confirms per user */
#define AVER_REQ_PER_USER 5
#define FREE_LIST_LIMIT 50

struct gsmd_user {
	struct llist_head list;		/* our entry in the global list */
	struct llist_head finished_ucmds;	/* our busy gsmd_ucmds */
	struct gsmd *gsmd;
	struct gsmd_fd gfd;				/* the socket */
	u_int32_t subscriptions;		/* bitmaks of subscribed event groups */

	struct llist_head pb_readrg_list;	/* our READRG phonebook list */
	u_int32_t pb_readrg_num;
	int pb_readrg_status;
	struct llist_head pb_find_list;		/* our FIND phonebook list */
	u_int32_t pb_find_num;
	int pb_find_status;
};

#define GSMD_DEBUG	1	/* debugging information */
#define GSMD_INFO	3
#define GSMD_NOTICE	5	/* abnormal/unexpected condition */
#define GSMD_ERROR	7	/* error condition, requires user action */
#define GSMD_FATAL	8	/* fatal, program aborted */

extern int gsmdlog_init(const char *path);
/* write a message to the daemons' logfile */
void __gsmd_log(int level, const char *file, int line, const char *function, const char *message, ...)
	__attribute__ ((__format__ (__printf__, 5, 6)));
/* macro for logging including filename and line number */
#define gsmd_log(level, format, args ...) \
	__gsmd_log(level, __FILE__, __LINE__, __FUNCTION__, format, ## args)

#define DEBUGP(x, args ...)	gsmd_log(GSMD_DEBUG, x, ## args)

#define GtM "GtM"
#define MtG "MtG"
#define GtC "GtC"
#define CtG "CtG"
void rerun_log(char* token, struct gsmd_msg_hdr* hdr, unsigned char* at, int atlen);

extern int gsmd_simplecmd(struct gsmd *gsmd, char *cmdtxt, int pin_sensitive);
extern int gsmd_notification_cmd(struct gsmd *gsmd, char *cmdtxt, int pin_sensitive);

extern void gsmd_close_channel(int channel);
extern void gsmd_close_channels();
extern void gsmd_eat_garbage();

#endif /* __GSMD__ */

#endif /* _GSMD_H */
