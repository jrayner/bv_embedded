/* libgsmd core
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

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <gsmd/usock.h>
#include <libgsmd/libgsmd.h>
#include <libgsmd/event.h>

#include "lgsm_internals.h"

#ifdef CONFIG_DEBUG_ON
#define _DLOG1(A)  printf(A)
#define _DLOG2(A,B)  printf(A,B)
#define _DLOG3(A,B,C)  printf(A,B,C)
#define _DLOG4(A,B,C,D)  printf(A,B,C,D)
#else
#define _DLOG1(A)
#define _DLOG2(A,B)
#define _DLOG3(A,B,C)
#define _DLOG4(A,B,C,D)
#endif

static int lgsm_get_packet(struct lgsm_handle *lh)
{
	static char buf[GSMD_MSGSIZE_MAX];
	struct gsmd_msg_hdr *hdr = (struct gsmd_msg_hdr *) buf;
	int rc = read(lh->fd, buf, sizeof(buf));
	if (rc <= 0)
		return rc;

	if (hdr->version != GSMD_PROTO_VERSION)
		return -EINVAL;

	switch (hdr->msg_type) {
	case GSMD_MSG_PASSTHROUGH:

		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int lgsm_open_backend(struct lgsm_handle *lh, const char *device,
	unsigned int instance_num)
{
	int rc;

	if (!strcmp(device, LGSMD_DEVICE_GSMD)) {
		struct sockaddr_un sun;

		// socket has initial \0 so extra work setting up base socket name
		const char* base_name_ptr = GSMD_UNIX_SOCKET;
		char socket_name[20];
		size_t socket_name_len;
		memset((void*)socket_name,0,20);
		if (instance_num) {
			snprintf(socket_name+1,19,"%s%d",base_name_ptr+1,instance_num);
		} else {
			snprintf(socket_name+1,19,"%s",base_name_ptr+1);
		}
		socket_name_len = strlen(socket_name+1);
		socket_name_len++;

		/* use unix domain socket to gsm daemon */
		lh->fd = socket(PF_UNIX, GSMD_UNIX_SOCKET_TYPE, 0);
		if (lh->fd < 0)
			return lh->fd;

		memset(&sun, 0, sizeof(sun));
		sun.sun_family = AF_UNIX;
		memcpy(sun.sun_path, socket_name, socket_name_len);

		rc = connect(lh->fd, (struct sockaddr *)&sun, sizeof(sun));
		if (rc < 0) {
			close(lh->fd);
			lh->fd = -1;
			return rc;
		}
	} else
		return -EINVAL;

	return 0;
}

/* handle a packet that was received on the gsmd socket */
int lgsm_handle_packet(struct lgsm_handle *lh, char *buf, int len)
{
	struct gsmd_msg_hdr *gmh;
	lgsm_msg_handler *handler;
	int rc = 0;

	while (len) {
		if (len < sizeof(*gmh))
			return -EINVAL;
		gmh = (struct gsmd_msg_hdr *) buf;

		if (len - sizeof(*gmh) < gmh->len)
			return -EINVAL;
		len -= sizeof(*gmh) + gmh->len;
		buf += sizeof(*gmh) + gmh->len;

		if (gmh->msg_type >= __NUM_GSMD_MSGS)
			return -EINVAL;

		handler = lh->handler[gmh->msg_type];

		if (handler)
			rc |= handler(lh, gmh);
		else
			fprintf(stderr, "unable to handle packet type=%u\n",
					gmh->msg_type);
	}
	return rc;
}

int lgsm_register_handler(struct lgsm_handle *lh, int type, lgsm_msg_handler *handler)
{
	if (type >= __NUM_GSMD_MSGS)
		return -EINVAL;

	lh->handler[type] = handler;

	return 0;
}

void lgsm_unregister_handler(struct lgsm_handle *lh, int type)
{
	if (type < __NUM_GSMD_MSGS)
		lh->handler[type] = NULL;
}

/* blocking read and processing of packets until packet matching 'id' is found */
int lgsm_blocking_wait_packet(struct lgsm_handle *lh, u_int16_t id,
				  struct gsmd_msg_hdr *gmh, int rlen)
{
	int rc;
	fd_set readset;

	FD_ZERO(&readset);

	while (1) {
		FD_SET(lh->fd, &readset);
		rc = select(lh->fd+1, &readset, NULL, NULL, NULL);
		if (rc <= 0)
			return rc;

		rc = read(lh->fd, (char *)gmh, rlen);
		if (rc <= 0)
			return rc;

		if (gmh->id == id) {
			/* we've found the matching packet, return to calling function */
			return rc;
		} else
			rc = lgsm_handle_packet(lh, (char *)gmh, rc);
	}
}

int lgsm_fd(struct lgsm_handle *lh)
{
	return lh->fd;
}

static pthread_mutex_t write_buffer_mutex = PTHREAD_MUTEX_INITIALIZER;

struct lgsm_handle *lgsm_init(const char *device)
{
	_DLOG2(">>lgsm_init %s\n",device);
	struct lgsm_handle *lh = malloc(sizeof(struct lgsm_handle));
	if (lh) {
		memset(lh, 0, sizeof(*lh));

		lh->fd = -1;
		lh->use_wait_list = 0;
		lh->start = NULL;
		lh->end = NULL;
		lh->num_waiting = 0;

		if (lgsm_open_backend(lh, device, 0) < 0) {
			free(lh);
			return NULL;
		}

		pthread_mutex_init(&write_buffer_mutex,NULL);
		lgsm_evt_init(lh);
	}
	_DLOG2("<<lgsm_init lh 0x%X\n",lh);
	return lh;
}

struct lgsm_handle *lgsm_unit_init(const char *device,
	unsigned int instance_num)
{
	_DLOG3(">>lgsm_unit_init %s %d\n",device,instance_num);
	struct lgsm_handle *lh = malloc(sizeof(struct lgsm_handle));
	if (lh) {
		memset(lh, 0, sizeof(*lh));

		lh->fd = -1;
		lh->use_wait_list = 0;
		lh->start = NULL;
		lh->end = NULL;
		lh->num_waiting = 0;

		if (lgsm_open_backend(lh, device, instance_num) < 0) {
			free(lh);
			return NULL;
		}

		pthread_mutex_init(&write_buffer_mutex,NULL);
		lgsm_evt_init(lh);
	}
	_DLOG2("<<lgsm_unit_init lh 0x%X\n",lh);
	return lh;
}

int lgsm_exit(struct lgsm_handle *lh)
{
	_DLOG1("lgsm_exit\n");
	if (lh) {
		close(lh->fd);
		if (lh->use_wait_list) {
			int ret = pthread_mutex_lock(&write_buffer_mutex);
			if (lh->start) {
				_DLOG2("%d items still on send queue?\n",lh->num_waiting);
				struct lgsm_send_data* tmp = lh->start;
				while (tmp) {
					struct lgsm_send_data* t = tmp;
					tmp = tmp->next;
					free(t);
				}
			}
			ret = pthread_mutex_unlock(&write_buffer_mutex);
		}
		free(lh);
		pthread_mutex_destroy(&write_buffer_mutex);
	}
	return 0;
}

int lgsm_send_ready(struct lgsm_handle *lh) { return lh->num_waiting; }

int lgsm_send(struct lgsm_handle *lh, struct gsmd_msg_hdr *gmh)
{
	int retval = 0;
	_DLOG2("lgsm_send lh 0x%X\n",lh);
	if (!lh) {
		fprintf(stderr,"** handle is null? **\n");
		return -EINVAL;
	}
	if (lh->use_wait_list) {
		int size = sizeof(struct lgsm_send_data) + gmh->len;
		int size_gmh = sizeof(struct gsmd_msg_hdr) + gmh->len;
		struct lgsm_send_data* data = (struct lgsm_send_data*) malloc(size);
		if (data) {
			data->next = NULL;
			memcpy((void*) &data->gmh, (void*) gmh, size_gmh);
			{
				unsigned char* p = (unsigned char*) &data->gmh;
				int i;
				_DLOG1("queued <");
				for (i = 0; i < size_gmh; i ++) {
					_DLOG2("%02X", *(p++));
				}
				_DLOG1(">\n");
			}
			int ret = pthread_mutex_lock(&write_buffer_mutex);
			if (lh->end) {
				lh->end->next = data;
			} else {
				lh->start = data;
			}
			lh->end = data;
			lh->num_waiting++;
			ret = pthread_mutex_unlock(&write_buffer_mutex);
			retval = size_gmh;
		} else {
			_DLOG1("malloc failed");
			retval = -ENOMEM;
		}
	} else {
		retval = send(lh->fd, (char *) gmh, sizeof(*gmh) + gmh->len, 0);
		//retval = write(lh->fd, gmh, sizeof(*gmh) + gmh->len);

#ifdef CONFIG_DEBUG_ON
		{
			unsigned char* p = (unsigned char*) gmh;
			int i;
			_DLOG1("sent <");
			for (i = 0; i < sizeof(struct gsmd_msg_hdr) + gmh->len; i ++) {
				_DLOG2("%02X", *(p++));
			}
			_DLOG1(">\n");
		}
#endif
	}
	_DLOG2("lgsm_send retval <size %d>\n",retval);

	/* Note: returns -ve error code or size of data transfered */
	return retval;
}

int lgsm_send_then_free_gmh(struct lgsm_handle *lh, struct gsmd_msg_hdr *gmh)
{
	int retval = 0;
	int size_sent = lgsm_send(lh,gmh);

	if (size_sent < 0) {
		retval = size_sent;
	} else {
		if (size_sent < gmh->len + sizeof(struct gsmd_msg_hdr)) {
			_DLOG3("result/size_sent %d < expected %d\n",
				size_sent,(gmh->len + sizeof(struct gsmd_msg_hdr)));
			retval = -EIO;
		}
	}

	lgsm_gmh_free(gmh);

	return retval;
}

void lgsm_set_checking(struct lgsm_handle *lh, int val) { lh->use_wait_list = val; }

int lgsm_check(struct lgsm_handle *lh)
{
	int retval = 0;
	if (lh->use_wait_list) {
		int ret = pthread_mutex_lock(&write_buffer_mutex);
		if (lh->num_waiting) {
			_DLOG2("lh->num_waiting 0x%X\n",lh->num_waiting);
			struct lgsm_send_data *pos;
			pos = lh->start;
			if (lh->start == lh->end) {
				lh->start = NULL;
				lh->end = NULL;
			} else {
			   struct lgsm_send_data* tmp = lh->start->next;
				lh->start = tmp;
			}
			lh->num_waiting--;
			ret = pthread_mutex_unlock(&write_buffer_mutex);
			{
					unsigned char* p = (unsigned char*) &pos->gmh;
					int i;
					_DLOG1("sending <");
					for (i = 0; i < sizeof(struct gsmd_msg_hdr) + pos->gmh.len; i ++) {
						_DLOG2("%02X", *(p++));
					}
					_DLOG2("> (%d)\n",i);
			}
			if (pos->gmh.version != GSMD_PROTO_VERSION) {
				fprintf(stderr,"*** BAD DATA ***??\n");
			} else {
				retval = write(lh->fd, &pos->gmh, sizeof(struct gsmd_msg_hdr) + pos->gmh.len);
			}
			free(pos);
			_DLOG2("lgsm_check retval %d\n",retval);
		} else {
			ret = pthread_mutex_unlock(&write_buffer_mutex);
		}
	}
	return retval;
}

struct gsmd_msg_hdr *lgsm_gmh_fill(int type, int subtype, int payload_len)
{
	struct gsmd_msg_hdr *gmh = malloc(sizeof(*gmh)+payload_len);
	if (!gmh)
		return NULL;

	memset(gmh, 0, sizeof(*gmh)+payload_len);

	gmh->version = GSMD_PROTO_VERSION;
	gmh->msg_type = type;
	gmh->msg_subtype = subtype;
	gmh->len = payload_len;

	return gmh;
}

int lgsm_send_simple(struct lgsm_handle *lh, int type, int sub_type)
{
	struct gsmd_msg_hdr *gmh = lgsm_gmh_fill(type, sub_type, 0);
	if (!gmh)
		return -ENOMEM;

	return lgsm_send_then_free_gmh(lh, gmh);
}


