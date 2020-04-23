/*
 * Copyright (c) 2016-2018, Linaro Ltd.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its contributors
 * may be used to endorse or promote products derived from this software without
 * specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#include <alloca.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "cdba-server.h"
#include "circ_buf.h"
#include "device.h"
#include "device_parser.h"
#include "fastboot.h"
#include "list.h"

static bool quit_invoked;

struct device *selected_device;

static void fastboot_opened(struct fastboot *fb, void *data)
{
	const uint8_t one = 1;
	struct msg *msg;

	msg = alloca(sizeof(*msg) + 1);
	msg->type = MSG_FASTBOOT_PRESENT;
	msg->len = 1;
	memcpy(msg->data, &one, 1);

	write(STDOUT_FILENO, msg, sizeof(*msg) + 1);

	/* We've reached fastboot, release the fastboot key */
	if (selected_device)
		device_fastboot_key(selected_device, false);
}

static void fastboot_info(struct fastboot *fb, const void *buf, size_t len)
{
	fprintf(stderr, "%s\n", (char *)buf);
}

static void fastboot_disconnect(void *data)
{
	const uint8_t zero = 0;
	struct msg *msg;

	msg = alloca(sizeof(*msg) + 1);
	msg->type = MSG_FASTBOOT_PRESENT;
	msg->len = 1;
	memcpy(msg->data, &zero, 1);

	write(STDOUT_FILENO, msg, sizeof(*msg) + 1);
}

static struct fastboot_ops fastboot_ops = {
	.opened = fastboot_opened,
	.disconnect = fastboot_disconnect,
	.info = fastboot_info,
};

static void msg_select_board(const void *param)
{
	struct msg reply = { MSG_SELECT_BOARD, 0 };

	selected_device = device_open(param, &fastboot_ops);
	if (!selected_device) {
		fprintf(stderr, "failed to open %s\n", (const char *)param);
		quit_invoked = true;
	}

	write(STDOUT_FILENO, &reply, sizeof(reply));
}

static void *fastboot_payload;
static size_t fastboot_size;

static void msg_fastboot_download(const void *data, size_t len)
{
	struct msg reply = { MSG_FASTBOOT_DOWNLOAD, };
	size_t new_size = fastboot_size + len;
	void *newp;

	newp = realloc(fastboot_payload, new_size);
	if (!newp)
		err(1, "failed too expant fastboot scratch area");

	memcpy(newp + fastboot_size, data, len);

	fastboot_payload = newp;
	fastboot_size = new_size;

	if (!len) {
		device_boot(selected_device, fastboot_payload, fastboot_size);

		write(STDOUT_FILENO, &reply, sizeof(reply));
		free(fastboot_payload);
		fastboot_payload = NULL;
		fastboot_size = 0;
	}
}

static void invoke_reply(int reply)
{
	struct msg msg = { reply, };

	write(STDOUT_FILENO, &msg, sizeof(msg));
}

static int handle_stdin(int fd, void *buf)
{
	static struct circ_buf recv_buf = { 0 };
	struct msg *msg;
	struct msg hdr;
	size_t n;
	int ret;

	ret = circ_fill(STDIN_FILENO, &recv_buf);
	if (ret < 0 && errno != EAGAIN) {
		fprintf(stderr, "read %d\n", ret);
		return -1;
	}

	for (;;) {
		n = circ_peak(&recv_buf, &hdr, sizeof(hdr));
		if (n != sizeof(hdr))
			return 0;

		if (CIRC_AVAIL(&recv_buf) < sizeof(*msg) + hdr.len)
			return 0;

		msg = malloc(sizeof(*msg) + hdr.len);
		circ_read(&recv_buf, msg, sizeof(*msg) + hdr.len);

		switch (msg->type) {
		case MSG_CONSOLE:
			device_write(selected_device, msg->data, msg->len);
			break;
		case MSG_FASTBOOT_PRESENT:
			break;
		case MSG_SELECT_BOARD:
			msg_select_board(msg->data);
			break;
		case MSG_HARDRESET:
			// fprintf(stderr, "hard reset\n");
			break;
		case MSG_POWER_ON:
			device_power_on(selected_device);

			invoke_reply(MSG_POWER_ON);
			break;
		case MSG_POWER_OFF:
			device_power_off(selected_device);

			invoke_reply(MSG_POWER_OFF);
			break;
		case MSG_FASTBOOT_DOWNLOAD:
			msg_fastboot_download(msg->data, msg->len);
			break;
		case MSG_FASTBOOT_BOOT:
			// fprintf(stderr, "fastboot boot\n");
			break;
		case MSG_STATUS_UPDATE:
			device_print_status(selected_device);
			break;
		case MSG_VBUS_ON:
			device_vbus(selected_device, true);
			break;
		case MSG_VBUS_OFF:
			device_vbus(selected_device, false);
			break;
		case MSG_SEND_BREAK:
			device_send_break(selected_device);
			break;
		default:
			fprintf(stderr, "unk %d len %d\n", msg->type, msg->len);
			exit(1);
		}

		free(msg);
	}

	return 0;
}

struct watch {
	struct list_head node;

	int fd;
	int (*cb)(int, void*);
	void *data;
};

static struct list_head read_watches = LIST_INIT(read_watches);

void watch_add_readfd(int fd, int (*cb)(int, void*), void *data)
{
	struct watch *w;

	w = calloc(1, sizeof(*w));
	w->fd = fd;
	w->cb = cb;
	w->data = data;

	list_add(&read_watches, &w->node);
}

static void sigpipe_handler(int signo)
{
	quit_invoked = true;
}

void watch_quit(void)
{
	quit_invoked = true;
}

int main(int argc, char **argv)
{
	struct watch *w;
	fd_set rfds;
	int flags;
	int nfds;
	int ret;

	signal(SIGPIPE, sigpipe_handler);

	device_parser(".cdba");

	watch_add_readfd(STDIN_FILENO, handle_stdin, NULL);

	flags = fcntl(STDIN_FILENO, F_GETFL, 0);
	fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);

	while (!quit_invoked) {
		nfds = 0;

		list_for_each_entry(w, &read_watches, node) {
			nfds = MAX(nfds, w->fd);
			FD_SET(w->fd, &rfds);
		}

		if (!FD_ISSET(STDIN_FILENO, &rfds)) {
			fprintf(stderr, "rfds is trash!\n");
			goto done;
		}

		ret = select(nfds + 1, &rfds, NULL, NULL, NULL);
		if (ret < 0)
			continue;

		list_for_each_entry(w, &read_watches, node) {
			if (FD_ISSET(w->fd, &rfds)) {
				ret = w->cb(w->fd, w->data);
				if (ret < 0) {
					fprintf(stderr, "cb returned %d\n", ret);
					goto done;
				}
			}
		}
	}

done:

	device_power_off(selected_device);

	return 0;
}
