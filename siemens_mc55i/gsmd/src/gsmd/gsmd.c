/* gsmd core
 *
 * (C) 2006-2007 by OpenMoko, Inc.
 * Written by Harald Welte <laforge@openmoko.org>
 * All Rights Reserved
 *
 * Copyright (C) 2007-2009 Jim Rayner <jimr@beyondvoice.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <termios.h>
#include <signal.h>

#define _GNU_SOURCE
#include <getopt.h>

#include <sys/types.h>
#include <sys/stat.h>

#include "gsmd.h"
#include "gsmd-version.h"

#include <gsmd/gsmd.h>
#include <gsmd/atcmd.h>
#include <gsmd/select.h>
#include <gsmd/usock.h>
#include <gsmd/vendorplugin.h>
#include <gsmd/talloc.h>
#include <gsmd/unsolicited.h>

#define GSMD_ALIVECMD		"AT"
#define GSMD_ALIVE_INTERVAL	5*60
#define GSMD_ALIVE_TIMEOUT	30

#define MAX_PIN_CMD_RETRIES	5
#define PIN_CMD_RETRY_DELAY	2

/* Siemens specific */
#define SIEMENS_SIM_OPER_TEMP_NOT_ALLOWED -256
#define SIEMENS_SIM_BLOCKED -262

static struct gsmd g;
static int daemonize = 0;

/* alive checking
 * either OK or ERROR is allowed since, both mean the modem still responds
 */


struct gsmd_alive_priv {
	struct gsmd *gsmd;
	int alive_responded;
};

static int gsmd_alive_cb(struct gsmd_atcmd *cmd, void *ctx, char *resp)
{
	struct gsmd_alive_priv *alp = ctx;

	if (!strcmp(resp, "OK") || !strcmp(resp, "ERROR") ||
		((alp->gsmd->flags & GSMD_FLAG_V0) && resp[0] == '0'))
		alp->alive_responded = 1;
	return 0;
}

static void alive_tmr_cb(struct gsmd_timer *tmr, void *data)
{
	struct gsmd_alive_priv *alp = data;

	DEBUGP("gsmd_alive timer expired\n", alp);

	if (alp->alive_responded == 0) {
		gsmd_log(GSMD_FATAL, "modem dead!\n");
		exit(3);
	} else
		gsmd_log(GSMD_INFO, "modem alive!\n");

	/* FIXME: update some global state */

	gsmd_timer_free(tmr);
	talloc_free(alp);
}

/* TODO remove this gsmd alive timer code */
/* start */
static int gsmd_modem_alive(struct gsmd *gsmd)
{
	struct gsmd_atcmd *cmd;
	struct gsmd_alive_priv *alp;
	struct timeval tv;
	int retval = 0;

	alp = talloc(gsmd_tallocs, struct gsmd_alive_priv);
	if (!alp)
		return -ENOMEM;

	alp->gsmd = gsmd;
	alp->alive_responded = 0;

	cmd = atcmd_fill(GSMD_ALIVECMD, strlen(GSMD_ALIVECMD)+1,
			 &gsmd_alive_cb, alp, NULL);
	if (!cmd) {
		talloc_free(alp);
		return -ENOMEM;
	}

	tv.tv_sec = GSMD_ALIVE_TIMEOUT;
	tv.tv_usec = 0;
	gsmd_timer_create(&tv, &alive_tmr_cb, alp);

	if (gsmd->number_channels > GSMD_ALIVE_CMD_CHANNEL)
		retval = atcmd_submit(gsmd, cmd, GSMD_ALIVE_CMD_CHANNEL);
	else
		retval = atcmd_submit(gsmd, cmd, GSMD_CMD_CHANNEL0);
	return retval;
}

static void alive_interval_tmr_cb(struct gsmd_timer *tmr, void *data)
{
	struct gsmd *gsmd = data;

	DEBUGP("interval expired, starting next alive inquiry\n");

	/* start a new alive check iteration */
	gsmd_modem_alive(gsmd);

	/* re-add the timer for the next interval */
	tmr->expires.tv_sec = GSMD_ALIVE_INTERVAL;
	tmr->expires.tv_usec = 0;

	gsmd_timer_register(tmr);
}

int gmsd_alive_start(struct gsmd *gsmd)
{
	struct timeval tv;

	tv.tv_sec = GSMD_ALIVE_INTERVAL;
	tv.tv_usec = 0;

	if (!gsmd_timer_create(&tv, &alive_interval_tmr_cb, gsmd))
		return -1;

	return 0;
}
/* end */


/* initial startup code */

static int gsmd_test_atcb(struct gsmd_atcmd *cmd, void *ctx, char *resp)
{
	DEBUGP("`%s' returned `%s'\n", cmd->buf, resp);
	if (!cmd->ret)
		g.modem_status = STATUS_OK;
	return 0;
}

int gsmd_initchannels(struct gsmd *gsmd)
{
	struct gsmd_atcmd *cmd;
	int retval = 0;
	u_int8_t channel;

	for (channel = GSMD_CMD_CHANNEL0; channel < gsmd->number_channels;
		channel++) {

		cmd = atcmd_fill("ATE0V1", strlen("ATE0V1")+1, &gsmd_test_atcb,
				NULL, NULL);
		if (!cmd)
			return -ENOMEM;

		retval = atcmd_submit(gsmd, cmd, channel);
		if (retval)
			break;
	}
	return retval;
}

static int gsmd_sim_present_cb(struct gsmd_atcmd *cmd, void *ctx, char *resp)
{
	int sim_found = 0;
	DEBUGP("^SCKS? returned <%s>\n", resp);
	if ((!cmd->ret)&&!strncmp(resp,"^SCKS: ",7)) {
		char* txt = &resp[7];
		if (*txt) {
			/* ignore mode */
			if (!strncmp(++txt,",1",2)) {
				DEBUGP("sim found\n");
				sim_found = 1;
			}
		}
		g.modem_status = STATUS_OK;
	}
	sim_present_changed(&g, sim_found);
	return 0;
}

static int gsmd_sim_present()
{
	struct gsmd_atcmd *cmd;
	int retval = 0;

	cmd = atcmd_fill("AT^SCKS?", strlen("AT^SCKS?")+1, &gsmd_sim_present_cb,
			NULL, NULL);
	if (!cmd)
		return -ENOMEM;
	cmd->timeout_value = 10; /* in case modem is slow */

	return atcmd_submit(&g, cmd, GSMD_CMD_CHANNEL0);
}

static int initExtendErrors(struct gsmd *gsmd)
{
	/* Specify extended error reporting on all channels */
	struct gsmd_atcmd *cmd;
	int retval = 0;
	u_int8_t channel;

	for (channel = GSMD_CMD_CHANNEL0; channel < gsmd->number_channels;
		channel++) {

		cmd = atcmd_fill("AT+CMEE=1", strlen("AT+CMEE=1")+1,
			&gsmd_test_atcb, NULL, NULL);
		cmd->timeout_value = 10;
		/* +CMEE is not pin sensitive */
		if (!cmd)
			return -ENOMEM;

		retval = atcmd_submit(gsmd, cmd, channel);
		if (retval)
			break;
	}
	return retval;
}

int gsmd_simplecmd(struct gsmd *gsmd, char *cmdtxt, int pin_sensitive)
{
	struct gsmd_atcmd *cmd;
	cmd = atcmd_fill(cmdtxt, strlen(cmdtxt)+1, &gsmd_test_atcb, NULL, NULL);
	if (!cmd)
		return -ENOMEM;
	cmd->timeout_value = 10; /* in case modem is slow */
	if (pin_sensitive)
		cmd->flags |= ATCMD_PIN_SENSITIVE;

	return atcmd_submit(gsmd, cmd, GSMD_CMD_CHANNEL0);
}

int gsmd_notification_cmd(struct gsmd *gsmd, char *cmdtxt, int pin_sensitive)
{
	struct gsmd_atcmd *cmd;
	int retval = 0;

	cmd = atcmd_fill(cmdtxt, strlen(cmdtxt)+1, &gsmd_test_atcb, NULL, NULL);
	if (!cmd)
		return -ENOMEM;
	cmd->timeout_value = 10; /* in case modem is slow */
	if (pin_sensitive)
		cmd->flags |= ATCMD_PIN_SENSITIVE;

	if (gsmd->number_channels > GSMD_NOTIFS_CHANNEL)
		retval = atcmd_submit(gsmd, cmd, GSMD_NOTIFS_CHANNEL);
	else
		retval = atcmd_submit(gsmd, cmd, GSMD_CMD_CHANNEL0);
	return retval;
}

static int test_pin_cb(struct gsmd_atcmd *cmd, void *ctx, char *resp)
{
	int retval = 0;
	if ((!cmd->ret)&&!strncmp(resp,"+CPIN: ",7)) {
		char* txt=&resp[7];
		struct gsmd_ucmd *gu = NULL;
		unsigned int cme_error = GSM0707_CME_UNKNOWN;
		DEBUGP("Got pin response <%s>\n", resp);
		if (!strncmp(txt,"SIM PIN",7)) {
			cme_error = GSM0707_CME_SIM_PIN_REQUIRED;
		} else if (!strncmp(txt,"SIM PUK",7)) {
			cme_error = GSM0707_CME_SIM_PUK_REQUIRED;
		} else if (!strncmp(txt,"SIM PIN2",8)) {
			cme_error = GSM0707_CME_SIM_PIN2_REQUIRED;
		} else if (!strncmp(txt,"SIM PUK2",8)) {
			cme_error = GSM0707_CME_SIM_PUK2_REQUIRED;
		} else if (!strncmp(txt,"READY",5)) {
			/* no pin/puk required */
			cme_error = 0;
		}
		if (cme_error != GSM0707_CME_UNKNOWN) {
			if (STATUS_NOT_KNOWN == g.sim_status) {
				/* no need to notify since pin change will do that */
				gsmd_log(GSMD_INFO, "sim status changed %d -> 0\n",
					STATUS_NOT_KNOWN);
				g.sim_status = 0;
			}
			retval = pin_status_changed(&g, cme_error);
		} else {
			gsmd_log(GSMD_INFO, "unexpected response '%s' to cpin?\n", resp);
		}
	} else {
		DEBUGP("test_pin_cb <%d>\n", cmd->ret);
		if (-GSM0707_CME_SIM_NOT_INSERTED == cmd->ret) {
			DEBUGP("no sim\n");
			retval = sim_status_changed(&g, GSM0707_CME_SIM_NOT_INSERTED);
		} else if (((SIEMENS_SIM_OPER_TEMP_NOT_ALLOWED == cmd->ret)||
				(SIEMENS_SIM_BLOCKED == cmd->ret))&&
				(cmd->cmd_retries < MAX_PIN_CMD_RETRIES)) {
			/* Siemens specific */
			/* MC55i in the SL40 appears to suffer from large number of SIM blocked
			   errors even though the SIM is ok. Retry the command after a delay.*/
			struct gsmd_atcmd *retry_cmd =
				atcmd_fill("AT+CPIN?", 9, &test_pin_cb, NULL, NULL);
			if (!retry_cmd) {
				retval = -ENOMEM;
			} else {
				DEBUGP("Resubmitting pin cmd (retry %d)\n",cmd->cmd_retries);
				retry_cmd->cmd_retries = cmd->cmd_retries + 1;
				retry_cmd->initial_delay_secs = PIN_CMD_RETRY_DELAY;

				/* high priority to ensure the cmd goes at start of
				   the pending list */
				retval = atcmd_submit_highpriority(
					&g, retry_cmd, GSMD_CMD_CHANNEL0);
			}
		} else {
			/* modem is not able to process the command - assume pin okay for now */
			/* if pin is required later than notification code will handle it */
			DEBUGP("unexpected pin error %d - assume pin ok for now\n",cmd->ret);
			if (STATUS_NOT_KNOWN == g.sim_status) {
				/* no need to notify since pin change will do that */
				gsmd_log(GSMD_INFO, "sim status changed %d -> 0\n",
					STATUS_NOT_KNOWN);
				g.sim_status = 0;
			}
			retval = pin_status_changed(&g, 0);
		}
	}

	return retval;
}

static int gsmd_testpin_cmd(struct gsmd *gsmd)
{
	struct gsmd_atcmd *cmd;
	cmd = atcmd_fill("AT+CPIN?", 9, &test_pin_cb, NULL, NULL);
	if (!cmd)
		return -ENOMEM;

	return atcmd_submit(gsmd, cmd, GSMD_CMD_CHANNEL0);
}


int gsmd_initsettings2(struct gsmd *gsmd)
{
	int rc;

	/* flush the comms */
	rc = gsmd_simplecmd(gsmd, "\rAT",NO_PIN_DEPEND);
	/* echo off, verbose */
	rc |= gsmd_initchannels(gsmd);
	/* do again since MC55 sometimes misses(?) the ATE0 */
	rc |= gsmd_initchannels(gsmd);
	/* Siemens specific SIM holder status*/
	rc |= gsmd_sim_present();
	/* use +CRING instead of RING */
	rc |= gsmd_notification_cmd(gsmd, "AT+CRC=1",NO_PIN_DEPEND);
	/* enable +CREG: unsolicited response if registration status changes */
	rc |= gsmd_notification_cmd(gsmd, "AT+CREG=2",NO_PIN_DEPEND);
	/* use +CME ERROR: instead of ERROR */
	rc |= initExtendErrors(gsmd);
	/* find out early on if a pin is required */
	/* after pin verification will automatically try and register to a network */
	rc |= gsmd_testpin_cmd(gsmd);
	/* use +CLIP: to indicate CLIP */
	rc |= gsmd_notification_cmd(gsmd, "AT+CLIP=1",NO_PIN_DEPEND);
	rc |= gsmd_notification_cmd(gsmd, "AT+CMER=2,0,0,2",PIN_DEPENDENT);
	/* use +COLP: to indicate COLP */
	rc |= gsmd_notification_cmd(gsmd, "AT+COLP=1",PIN_DEPENDENT);

	/* configure message format as PDU mode*/
	sms_cb_init(gsmd);

	if (gsmd->vendorpl && gsmd->vendorpl->initsettings)
		rc |= gsmd->vendorpl->initsettings(gsmd);

	return rc;
}

static int firstcmd_count = 0;

static int firstcmd_atcb(struct gsmd_atcmd *cmd, void *ctx, char *resp)
{
	struct gsmd *gsmd = &g;

	if (!gsmd->running)
		return 0;

	if (!strstr(resp,"ATZ") && !strstr(resp,"OK")) {
		cmd->ret = -ECANCELED;
		gsmd_log(GSMD_ERROR, "unexpected response '%s' to initial command\n", resp);
	}

	if (cmd->ret == -ECANCELED) {
		gsmd_log(GSMD_ERROR, "no modem detected <%d>\n", cmd->ret);
		if (3 == firstcmd_count) {
			gsmd_log(GSMD_ERROR, "giving up - no modem - gsmd will shutdown\n");
			modem_status_changed(gsmd, -ECANCELED);
			return cmd->ret;
		} else {
			int ret = gsmd_initsettings(gsmd);
			if (ret) {
				gsmd_log(GSMD_ERROR, "reposting atz failed %d\n", ret);
				modem_status_changed(gsmd, ret);
				return -1;
			}
			firstcmd_count++;
			return 0;
		}
	}

	g.modem_status = STATUS_OK;

	if (daemonize) {
		if (fork()) {
			exit(0);
		}
		fclose(stdout);
		fclose(stderr);
		fclose(stdin);
		setsid();
	}

	return gsmd_initsettings2(gsmd);
}

int gsmd_initsettings(struct gsmd *gsmd)
{
	struct gsmd_atcmd *cmd;
	struct timeval tv;

	cmd = atcmd_fill("ATZ", 4, &firstcmd_atcb, NULL, NULL);
	if (!cmd)
		return -ENOMEM;
	cmd->timeout_value = 5;
	cmd->flags |= ATCMD_WAKEUP_MODEM;

	return atcmd_submit(gsmd, cmd, GSMD_CMD_CHANNEL0);
}

struct bdrt {
	int bps;
	u_int32_t b;
};

static struct bdrt bdrts[] = {
	{ 0, B0 },
	{ 9600, B9600 },
	{ 19200, B19200 },
	{ 38400, B38400 },
	{ 57600, B57600 },
	{ 115200, B115200 },
	{ 230400, B230400 },
	{ 460800, B460800 },
	{ 921600, B921600 },
};

static int set_baudrate(int fd, int baudrate, int hwflow)
{
	int i;
	u_int32_t bd = 0;
	struct termios ti;

	for (i = 0; i < ARRAY_SIZE(bdrts); i++) {
		if (bdrts[i].bps == baudrate)
			bd = bdrts[i].b;
	}
	if (bd == 0)
		return -EINVAL;

	i = tcgetattr(fd, &ti);
	if (i < 0)
		return i;

	i = cfsetispeed(&ti, B0);
	if (i < 0)
		return i;

	i = cfsetospeed(&ti, bd);
	if (i < 0)
		return i;

	if (hwflow)
		ti.c_cflag |= CRTSCTS;
	else
		ti.c_cflag &= ~CRTSCTS;

	return tcsetattr(fd, 0, &ti);
}

static int gsmd_initialize(struct gsmd *g)
{
	u_int8_t channel;
	INIT_LLIST_HEAD(&g->users);

	for (channel = GSMD_CMD_CHANNEL0; channel < g->number_channels; channel++) {
		g->mlbuf[channel] = talloc_array(gsmd_tallocs, unsigned char, MLPARSE_BUF_SIZE);
		if (!g->mlbuf[channel])
			return -ENOMEM;
	}
	return 0;
}

static void gsmd_reset_timeout(struct gsmd_timer *tmr, void *data)
{
	gsmd_timer_free(g.reset_timer);
	g.reset_timer=NULL;
	if (!g.num_of_clients) {
		DEBUGP("Reset timeout, no connected clients\n");
		// For now do not timeout gsmd
		//shutdown_modem(&g,NULL);
	}
}

void gsmd_reset_timer()
{
	struct timeval tv;
	tv.tv_sec = 5;
	tv.tv_usec = 0;
	g.reset_timer=gsmd_timer_create(&tv,&gsmd_reset_timeout,NULL);
}

static struct option opts[] = {
	{ "version", 0, NULL, 'V' },
	{ "daemon", 0, NULL, 'd' },
	{ "help", 0, NULL, 'h' },
	{ "device", 1, NULL, 'p' },
	{ "speed", 1, NULL, 's' },
	{ "logfile", 1, NULL, 'l' },
	{ "hwflow", 0, NULL, 'F' },
	{ "leak-report", 0, NULL, 'L' },
	{ "vendor", 1, NULL, 'v' },
	{ "machine", 1, NULL, 'm' },
	{ "wait", 1, NULL, 'w' },
	{ "channels", 1, NULL, 'c' },
	{ "test", 0, NULL, 't' },
	{ "reset", 0, NULL, 'r' },
	{ 0, 0, 0, 0 }
};

static void print_header(void)
{
	printf("gsmd - (C) 2006-2007 by OpenMoko, Inc. and contributors\n"
		   "This program is FREE SOFTWARE under the terms of GNU GPL\n\n");
}

static void print_version(void)
{
	printf("gsmd, version %s\n",GSMD_VERSION);
}

static void print_usage(void)
{
	printf("Usage:\n"
		   "\t-V\t--version\tDisplay program version\n"
		   "\t-d\t--daemon\tDeamonize\n"
		   "\t-h\t--help\t\tDisplay this help message\n"
		   "\t-p dev\t--device dev\tSpecify serial device to be used\n"
		   "\t-s spd\t--speed spd\tSpecify speed in bps (9600,38400,115200,...)\n"
		   "\t-F\t--hwflow\tHardware Flow Control (RTS/CTS)\n"
		   "\t-L\t--leak-report\tLeak Report of talloc memory allocator\n"
		   "\t-l file\t--logfile file\tSpecify a logfile to log to\n"
		   "\t-v\t--vendor v\tSpecify GSM modem vendor plugin\n"
		   "\t-m\t--machine m\tSpecify GSM modem machine plugin\n"
		   "\t-w\t--wait m\tWait for the AT Interpreter Ready message\n"
		   "\t-c\t--channels c\tNumber of channels\n"
		   "\t-t\t--test t\tUse dummy modem\n"
		   "\t-r\t--reset t\tDie if no client connection within 5 seconds\n"
		   );
}

void gsmd_close_channel(int channel)
{
	if (g.dummym_enabled) {
		if (g.gfd_dummym[channel].fd) {
			gsmd_unregister_fd(&g.gfd_dummym[channel]);
			DEBUGP("closing %d\n",channel);
			close(g.gfd_dummym[channel].fd);
			g.gfd_dummym[channel].fd = 0;
		}
	} else {
		if (g.gfd_uart[channel].fd) {
			gsmd_unregister_fd(&g.gfd_uart[channel]);
			DEBUGP("closing %d\n",channel);
			close(g.gfd_uart[channel].fd);
			g.gfd_uart[channel].fd = 0;
		}
	}
}

void gsmd_close_channels()
{
	DEBUGP("gsmd_close_channels (%d)\n",g.running);
	if (g.running && g.number_channels) {
		struct gsmd_user *gu;
		g.running = 0;
		if (g.number_channels > 1) {
			atcmd_revert_to_single_port_mode(&g);
		}
		llist_for_each_entry(gu, &g.users, list) {
			gsmd_unregister_fd(&gu->gfd);
			close(gu->gfd.fd);
		}
	}
	gsmd_close_channel(GSMD_CMD_CHANNEL0);
}

void gsmd_eat_garbage()
{
	if (!g.dummym_enabled) {
		int rc = write(g.modem_fd, "\r", 1);
		sleep(1);
		atcmd_drain(g.modem_fd);
	}
}

static void sig_handler(int signr)
{
	switch (signr) {
	case SIGTERM:
	case SIGINT:
		talloc_report_full(gsmd_tallocs, stderr);
		gsmd_close_channels();
		exit(0);
		break;
	case SIGUSR1:
		talloc_report_full(gsmd_tallocs, stderr);
	case SIGALRM:
		gsmd_timer_check_n_run();
		break;
	}
}

static int connect_to_modem(const char* device, int bps, int hwflow)
{
	int retval = 0;
	int retry_count = 0;
	int retry_limit = 4;

	while (retry_count <  retry_limit) {
		DEBUGP("retry_count %d\n",retry_count);
		retry_count++;
		if (!g.modem_fd) {
			DEBUGP("opened %s\n",device);
			g.modem_fd = open(device, O_RDWR);
			if (g.modem_fd < 0) {
				fprintf(stderr, "can't open device `%s': %s\n",
					device, strerror(errno));
				retval = g.modem_fd;
				break;
			}
			retval = set_baudrate(g.modem_fd, bps, hwflow);
			if (retval < 0) {
				fprintf(stderr, "can't set baudrate %d\n",retval);
				break;
			}
			atcmd_drain(g.modem_fd);
			retval = write(g.modem_fd, "\r\r", 2);
		}

		retval = write(g.modem_fd, "AT\r\n", 4);
		if (retval < 0) {
			fprintf(stderr,"write failed %d\n",retval);
			break;
		}
		DEBUGP("written to port [retval %d]\n",retval);

		{
			fd_set rfds;
			struct timeval tv;
			int rdval;
			int maxfd = g.modem_fd + 1;

			retval = -1;

			FD_ZERO(&rfds);
			FD_SET(g.modem_fd, &rfds);
			tv.tv_sec = 3;
			tv.tv_usec = 0;

			rdval = select(maxfd, &rfds, NULL, NULL, &tv);
			if (rdval == 0) {
				DEBUGP("timed out\n");
			} else if (FD_ISSET(g.modem_fd, &rfds)) {
				char input_buf[50];
				int rdlen=read(g.modem_fd,input_buf,50);
				DEBUGP("recv %s %d\n",input_buf,rdlen);
				if (rdlen && (strstr(input_buf,"AT") || strstr(input_buf,"OK") || strstr(input_buf,"^SYS"))) {
					DEBUGP("modem exists %s %d\n",input_buf,rdlen);
					retval = 0;
					break;
				} else if (retry_count < retry_limit) {
					DEBUGP("possible connection\n");
					close(g.modem_fd);
					DEBUGP("closed %s\n",device);
 					retry_limit++;
					g.modem_fd = 0;
				}
			}
		}
	}

	return retval;
}


int main(int argc, char **argv)
{
	int argch;

	int bps = 115200;
	int hwflow = 0;
	char *device = NULL;
	char *logfile = "syslog";
	char *vendor_name = NULL;
	char *machine_name = NULL;
	int wait = -1;
	int set_reset_timer = 0;
	unsigned int instance_num = 0;

	signal(SIGTERM, sig_handler);
	signal(SIGINT, sig_handler);
	signal(SIGSEGV, sig_handler);
	signal(SIGUSR1, sig_handler);
	signal(SIGALRM, sig_handler);

	gsmd_tallocs = talloc_named_const(NULL, 1, "GSMD");
	g.number_channels = 1;
	g.dummym_enabled = 0;
	g.running = 1;
	g.suspended = 0;
	g.pin_sensitive_cmds_waiting = 0;
	g.modem_fd = 0;
	g.num_of_clients = 0;
	g.reset_timer = NULL;
	g.nak_timer = NULL;
	g.sim_inserted_retry_count = 0;
	g.sim_inserted_retry_timer = NULL;
	g.sim_busy_retry_count = 0;
	g.sim_busy_retry_timer = NULL;

	/*print_header();*/

	/*FIXME: parse commandline, set daemonize, device, ... */
	while ((argch = getopt_long(
		argc, argv, "FVLdhtrp:s:l:v:m:w:c:n:", opts, NULL)) != -1) {
		switch (argch) {
		case 'V':
			print_version();
			exit(0);
			break;
		case 'L':
			talloc_enable_leak_report_full();
			break;
		case 'F':
			hwflow = 1;
			break;
		case 'd':
			daemonize = 1;
			break;
		case 'h':
			/* FIXME */
			print_usage();
			exit(0);
			break;
		case 'p':
			device = optarg;
			break;
		case 's':
			bps = atoi(optarg);
			break;
		case 'l':
			if (gsmdlog_init(optarg)) {
				fprintf(stderr, "can't open logfile `%s'\n", optarg);
				exit(-2);
			}
			break;
		case 'v':
			vendor_name = optarg;
			break;
		case 'm':
			machine_name = optarg;
			break;
		case 'w':
			wait = atoi(optarg);
			break;
		case 'c':
			g.number_channels = atoi(optarg);
			if (g.number_channels < 1 || g.number_channels > GSMD_MAX_CHANNELS) {
				fprintf(stderr, "channels incorrect %d (max channels %d)\n",
					g.number_channels, GSMD_MAX_CHANNELS);
				exit(-2);
			}
			break;
		case 't':
			g.dummym_enabled = 1;
			break;
		case 'r':
			set_reset_timer = 1;
			break;
		case 'n':
			instance_num = atoi(optarg);
			break;
		}
	}

	if (g.dummym_enabled) {

		DEBUGP("Using dummy modem\n");

	} else {

		if (!device) {
			fprintf(stderr, "ERROR: you have to specify a port (-p port)\n");
			print_usage();
			exit(-2);
		}

		if (connect_to_modem(device,bps,hwflow) < 0) {
			fprintf(stderr, "** Could not connect with the modem **\n");
			if (g.modem_fd)
				close(g.modem_fd);
			exit(-1);
		}
	}

	if (gsmd_initialize(&g) < 0) {
		fprintf(stderr, "internal error\n");
		if (g.modem_fd)
			close(g.modem_fd);
		exit(-1);
	}

	gsmd_timer_init();

	if (gsmd_machine_plugin_init(&g, machine_name, vendor_name) < 0) {
		fprintf(stderr, "no machine plugins found\n");
		if (g.modem_fd)
			close(g.modem_fd);
		exit(1);
	}

	/* select a machine plugin and load possible vendor plugins */
	gsmd_machine_plugin_find(&g);

	/* initialize the machine plugin */
	if (g.machinepl->init &&
		(g.machinepl->init(&g, g.modem_fd) < 0)) {
		fprintf(stderr, "couldn't initialize machine plugin\n");
		if (g.modem_fd)
			close(g.modem_fd);
		exit(-1);
	}

	if (wait >= 0)
		g.interpreter_ready = !wait;

	if (atcmd_init(&g, g.modem_fd) < 0) {
		fprintf(stderr,"can't initialize UART device, number channels <%d>\n",
			g.number_channels);
		exit(-1);
	}

	if (usock_init(&g,instance_num) < 0) {
		fprintf(stderr, "can't open unix socket\n");
		exit(-1);
	}
	if (set_reset_timer) {
		DEBUGP("reset timeout enabled\n");
		gsmd_reset_timer();
	}

	/* select a vendor plugin */
	gsmd_vendor_plugin_find(&g);

	unsolicited_init(&g);

	gsmd_initsettings(&g);

	gsmd_opname_init(&g);

	while (g.running) {
		int ret = gsmd_select_main();
		if (ret == 0)
			continue;

		if (ret < 0) {
			if (errno == EINTR)
				continue;
			else {
				DEBUGP("select returned error (%s)\n",
					strerror(errno));
				break;
			}
		}
	}
	gsmd_close_channels();
	DEBUGP("exiting gsmd\n");
	exit(0);
}
