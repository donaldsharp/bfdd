/*********************************************************************
 * Copyright 2017 Network Device Education Foundation, Inc. ("NetDEF")
 *
 * This file is licensed to You under the Eclipse Public License (EPL);
 * You may not use this file except in compliance with the License. You
 * may obtain a copy of the License at
 * http://www.opensource.org/licenses/eclipse-1.0.php
 *
 * control.c implements the BFD daemon control socket. It will be used to talk
 * with clients daemon/scripts/consumers.
 *
 * Authors
 * -------
 * Rafael Zalamena <rzalamena@opensourcerouting.org>
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "bfd.h"

/*
 * Prototypes
 */
void control_accept(evutil_socket_t sd, short ev, void *arg);

struct bfd_control_socket *control_new(int sd);
void control_free(struct bfd_control_socket *bcs);
void control_reset_buf(struct bfd_control_buffer *bcb);
void control_read(evutil_socket_t sd, short ev, void *arg);
void control_write(evutil_socket_t sd, short ev, void *arg);

void control_handle_request_add(struct bfd_control_socket *bcs,
				struct bfd_control_msg *bcm);
void control_handle_request_del(struct bfd_control_socket *bcs,
				struct bfd_control_msg *bcm);
void control_handle_notify(struct bfd_control_socket *bcs,
			   struct bfd_control_msg *bcm);
void control_response(struct bfd_control_socket *bcs, uint16_t id,
		      const char *status, const char *error);


/*
 * Functions
 */
int control_init(void)
{
	int sd;
	struct sockaddr_un sun = {
		.sun_family = AF_UNIX, .sun_path = BFD_CONTROL_SOCK_PATH,
	};

	/* Remove previously created sockets. */
	unlink(sun.sun_path);

	sd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK,
		    PF_UNSPEC);
	if (sd == -1) {
		log_error("%s: socket: %s\n", __FUNCTION__, strerror(errno));
		return -1;
	}

	if (bind(sd, (struct sockaddr *)&sun, sizeof(sun)) == -1) {
		log_error("%s: bind: %s\n", __FUNCTION__, strerror(errno));
		close(sd);
		return -1;
	}

	if (listen(sd, SOMAXCONN) == -1) {
		log_error("%s: listen: %s\n", __FUNCTION__, strerror(errno));
		close(sd);
		return -1;
	}

	bglobal.bg_csock = sd;
	event_assign(&bglobal.bg_csockev, bglobal.bg_eb, sd,
		     EV_READ | EV_PERSIST, control_accept, NULL);
	event_add(&bglobal.bg_csockev, NULL);

	return 0;
}

void control_accept(evutil_socket_t sd, short ev __attribute__((unused)),
		    void *arg __attribute__((unused)))
{
	int csock;

	csock = accept(sd, NULL, 0);
	if (csock == -1) {
		log_warning("%s: accept: %s\n", __FUNCTION__, strerror(errno));
		return;
	}

	if (control_new(csock) == NULL)
		close(csock);
}


/*
 * Client handling
 */
struct bfd_control_socket *control_new(int sd)
{
	struct bfd_control_socket *bcs;

	bcs = calloc(1, sizeof(*bcs));
	if (bcs == NULL)
		return NULL;

	/* Disable notifications by default. */
	bcs->bcs_notify = 0;

	bcs->bcs_sd = sd;
	event_assign(&bcs->bcs_ev, bglobal.bg_eb, sd, EV_READ | EV_PERSIST,
		     control_read, bcs);
	event_assign(&bcs->bcs_outev, bglobal.bg_eb, sd, EV_WRITE | EV_PERSIST,
		     control_write, bcs);
	event_add(&bcs->bcs_ev, NULL);

	TAILQ_INSERT_TAIL(&bglobal.bg_bcslist, bcs, bcs_entry);

	return bcs;
}

void control_free(struct bfd_control_socket *bcs)
{
	event_del(&bcs->bcs_outev);
	event_del(&bcs->bcs_ev);
	close(bcs->bcs_sd);

	TAILQ_REMOVE(&bglobal.bg_bcslist, bcs, bcs_entry);

	control_reset_buf(&bcs->bcs_bin);
	control_reset_buf(&bcs->bcs_bout);
	free(bcs);
}

void control_reset_buf(struct bfd_control_buffer *bcb)
{
	/* Get ride of old data. */
	free(bcb->bcb_buf);
	bcb->bcb_buf = NULL;
	bcb->bcb_pos = 0;
	bcb->bcb_left = 0;
}

void control_read(evutil_socket_t sd, short ev __attribute__((unused)),
		  void *arg)
{
	struct bfd_control_socket *bcs = arg;
	struct bfd_control_buffer *bcb = &bcs->bcs_bin;
	struct bfd_control_msg bcm;
	ssize_t bread;
	size_t plen;

	/*
	 * Check if we have already downloaded message content, if so then skip
	 * to
	 * download the rest of it and process.
	 *
	 * Otherwise download a new message header and allocate the necessary
	 * memory.
	 */
	if (bcb->bcb_buf != NULL)
		goto skip_header;

	bread = read(sd, &bcm, sizeof(bcm));
	if (bread == 0) {
		control_free(bcs);
		return;
	}
	if (bread < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
			return;

		log_warning("%s: read: %s\n", __FUNCTION__, strerror(errno));
		control_free(bcs);
		return;
	}

	/* Validate header fields. */
	plen = ntohl(bcm.bcm_length);
	if (plen < 2) {
		log_debug("%s: client closed due small message length: %d\n",
			  __FUNCTION__, bcm.bcm_length);
		control_free(bcs);
		return;
	}

	if (bcm.bcm_ver != BMV_VERSION_1) {
		log_debug("%s: client closed due bad version: %d\n",
			  __FUNCTION__, bcm.bcm_ver);
		control_free(bcs);
		return;
	}

	/* Prepare the buffer to load the message. */
	bcs->bcs_version = bcm.bcm_ver;
	bcs->bcs_type = bcm.bcm_type;

	bcb->bcb_pos = sizeof(bcm);
	bcb->bcb_left = plen;
	bcb->bcb_buf = malloc(sizeof(bcm) + bcb->bcb_left + 1);
	if (bcb->bcb_buf == NULL) {
		log_warning("%s: not enough memory for message size: %u\n",
			  __FUNCTION__, bcb->bcb_left);
		control_free(bcs);
		return;
	}

	memcpy(bcb->bcb_buf, &bcm, sizeof(bcm));

	/* Terminate data string with NULL for later processing. */
	bcb->bcb_buf[sizeof(bcm) + bcb->bcb_left] = 0;

skip_header:
	/* Download the remaining data of the message and process it. */
	bread = read(sd, &bcb->bcb_buf[bcb->bcb_pos], bcb->bcb_left);
	if (bread == 0) {
		control_free(bcs);
		return;
	}
	if (bread < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
			return;

		log_warning("%s: read: %s\n", __FUNCTION__, strerror(errno));
		control_free(bcs);
		return;
	}

	bcb->bcb_pos += bread;
	bcb->bcb_left -= bread;
	/* We need more data, return to wait more. */
	if (bcb->bcb_left > 0)
		return;

	switch (bcm.bcm_type) {
	case BMT_REQUEST_ADD:
		control_handle_request_add(bcs, bcb->bcb_bcm);
		break;
	case BMT_REQUEST_DEL:
		control_handle_request_del(bcs, bcb->bcb_bcm);
		break;
	case BMT_NOTIFY:
		control_handle_notify(bcs, bcb->bcb_bcm);
		break;

	default:
		log_debug("%s: unhandled message type: %d\n", __FUNCTION__,
			  bcm.bcm_type);
		break;
	}

	bcs->bcs_version = 0;
	bcs->bcs_type = 0;
	control_reset_buf(bcb);
}

void control_write(evutil_socket_t sd, short ev __attribute__((unused)),
		   void *arg)
{
	struct bfd_control_socket *bcs = arg;
	struct bfd_control_buffer *bcb = &bcs->bcs_bout;
	ssize_t bwrite;

	bwrite = write(sd, &bcb->bcb_buf[bcb->bcb_pos], bcb->bcb_left);
	if (bwrite == 0) {
		control_free(bcs);
		return;
	}
	if (bwrite < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
			return;

		log_warning("%s: write: %s\n", __FUNCTION__, strerror(errno));
		control_free(bcs);
		return;
	}

	bcb->bcb_pos += bwrite;
	bcb->bcb_left -= bwrite;
	if (bcb->bcb_left > 0)
		return;

	control_reset_buf(bcb);

	event_add(&bcs->bcs_ev, NULL);
	event_del(&bcs->bcs_outev);
}


/*
 * Message processing
 */
void control_handle_request_add(struct bfd_control_socket *bcs,
				struct bfd_control_msg *bcm)
{
	const char *json = (const char *)bcm->bcm_data;

	if (config_request_add(json) == 0)
		control_response(bcs, bcm->bcm_id, BCM_RESPONSE_OK, NULL);
	else
		control_response(bcs, bcm->bcm_id, BCM_RESPONSE_ERROR,
				 "request add failed");
}

void control_handle_request_del(struct bfd_control_socket *bcs,
				struct bfd_control_msg *bcm)
{
	const char *json = (const char *)bcm->bcm_data;

	if (config_request_del(json) == 0)
		control_response(bcs, bcm->bcm_id, BCM_RESPONSE_OK, NULL);
	else
		control_response(bcs, bcm->bcm_id, BCM_RESPONSE_ERROR,
				 "request del failed");
}

void control_handle_notify(struct bfd_control_socket *bcs,
			   struct bfd_control_msg *bcm)
{
	bcs->bcs_notify = *(uint64_t *)bcm->bcm_data;

	control_response(bcs, bcm->bcm_id, BCM_RESPONSE_OK, NULL);
}

void control_response(struct bfd_control_socket *bcs, uint16_t id,
		      const char *status, const char *error)
{
	struct bfd_control_buffer *bcb = &bcs->bcs_bout;
	char *jsonstr;
	size_t jsonstrlen;

	/* Generate JSON response. */
	jsonstr = config_response(status, error);
	if (jsonstr == NULL) {
		log_warning("%s: config_response: failed to get JSON str\n",
				__FUNCTION__);
		return;
	}

	/* Allocate data and answer. */
	jsonstrlen = strlen(jsonstr);
	bcb->bcb_buf = malloc(sizeof(struct bfd_control_msg) + jsonstrlen);
	if (bcb->bcb_buf == NULL) {
		log_warning("%s: malloc: %s\n", __FUNCTION__, strerror(errno));
		free(jsonstr);
		return;
	}

	bcb->bcb_bcm->bcm_length = htonl(jsonstrlen);
	bcb->bcb_bcm->bcm_ver = BMV_VERSION_1;
	bcb->bcb_bcm->bcm_type = BMT_RESPONSE;
	bcb->bcb_bcm->bcm_id = id;
	memcpy(bcb->bcb_bcm->bcm_data, jsonstr, jsonstrlen);
	free(jsonstr);

	bcb->bcb_left = sizeof(struct bfd_control_msg) + jsonstrlen;

	event_add(&bcs->bcs_outev, NULL);
	event_del(&bcs->bcs_ev);
}