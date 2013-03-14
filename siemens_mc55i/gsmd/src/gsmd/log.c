/* gsmd logging functions 
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

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>
#include <syslog.h>
#include <time.h>

#include <sys/types.h>

#include "gsmd.h"

#include <gsmd/gsmd.h>
#include <gsmd/usock.h>

static FILE *logfile;
static FILE syslog_dummy;
static int loglevel = GSMD_DEBUG; //GSMD_ERROR; /* Reduce the logging on podpoint */

static int gsmd2syslog[] = {
	[GSMD_DEBUG]	= LOG_DEBUG,
	[GSMD_INFO]	= LOG_INFO,
	[GSMD_NOTICE]	= LOG_NOTICE,
	[GSMD_ERROR]	= LOG_ERR,
	[GSMD_FATAL]	= LOG_CRIT,
};

static inline int gsmd2syslog_level(int level)
{
	if (level >= ARRAY_SIZE(gsmd2syslog))
		return LOG_ERR;

	return gsmd2syslog[level];
}

void __gsmd_log(int level, const char *file, int line, const char *function,
		const char *format, ...)
{
	char *timestr;
	va_list ap;
	time_t tm;
	FILE *outfd;

	if (level < loglevel)
		return;
	
	if (logfile == &syslog_dummy) {
		va_start(ap, format);
		vsyslog(gsmd2syslog_level(level), format, ap);
		va_end(ap);
	} else {
		if (logfile)
			outfd = logfile;
		else
			outfd = stderr;

		tm = time(NULL);
		timestr = ctime(&tm);
		timestr[strlen(timestr)-1] = '\0';
		fprintf(outfd, "%s <%1.1d> %s:%d:%s() ", timestr, level, file, 
			line, function);

		va_start(ap, format);
		vfprintf(outfd, format, ap);
		va_end(ap);

		fflush(outfd);
	}
}

int gsmdlog_init(const char *path)
{
	
	if (!strcmp(path, "syslog")) {
		logfile = &syslog_dummy;
		openlog("gsmd", 0, LOG_DAEMON);
	} else {
		logfile = fopen(path, "a+");
	}

	if (logfile == NULL)
		return -1;
	
	gsmd_log(GSMD_INFO, "\n\n\nlogfile successfully opened\n");

	return 0;
}

static void __rerun_basic_log(const char *format, ...)
{
	va_list ap;
	FILE *outfd;

	if (logfile == &syslog_dummy) {
		va_start(ap, format);
		vsyslog(LOG_INFO, format, ap);
		va_end(ap);
	} else {
		if (logfile)
			outfd = logfile;
		else
			outfd = stderr;

		va_start(ap, format);
		vfprintf(outfd, format, ap);
		va_end(ap);

		fflush(outfd);
	}
}

static void __rerun_log(char* token, unsigned char* c, int clen, unsigned char* at, int atlen)
{
	int i;
	unsigned char* p;
	
	__rerun_basic_log("<%s",token);
	if (c) {
		p = c;
		__rerun_basic_log(" c=\"",token);
		for (i = 0; i < clen; i ++) {
			__rerun_basic_log("%02X", *(p++));
		}
		__rerun_basic_log("\"");
	}
	if (at) {
		p = at;
		__rerun_basic_log(" at=\"");
		for (i = 0; i < atlen; i ++) {
			__rerun_basic_log("%02X", *(p++));
		}
		__rerun_basic_log("\"");
	}
	__rerun_basic_log("/>\n");
}

void rerun_log(char* token, struct gsmd_msg_hdr* hdr, unsigned char* at, int atlen)
{
	if (hdr) {
		int hlen = sizeof(struct gsmd_msg_hdr) + hdr->len;
		__rerun_log(token, (unsigned char*) hdr, hlen, at, atlen);
	} else {
		__rerun_log(token, NULL, 0, at, atlen);
	}
}


