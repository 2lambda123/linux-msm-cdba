/*
 * Copyright (c) 2016-2018, Linaro Ltd.
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <sys/time.h>
#include <alloca.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <syslog.h>

#include "cdba-server.h"
#include "circ_buf.h"
#include "device.h"
#include "device_parser.h"
#include "fastboot.h"
#include "list.h"

static bool quit_invoked;
static const char *username;

struct device *selected_device;

int tty_open(const char *tty, struct termios *old)
{
	struct termios tios;
	int ret;
	int fd;

	fd = open(tty, O_RDWR | O_NOCTTY | O_EXCL);
	if (fd < 0)
		err(1, "unable to open \"%s\"", tty);

	ret = tcgetattr(fd, old);
	if (ret < 0)
		err(1, "unable to retrieve \"%s\" tios", tty);

	memset(&tios, 0, sizeof(tios));
	tios.c_cflag = B115200 | CS8 | CLOCAL | CREAD;
	tios.c_iflag = IGNPAR;
	tios.c_oflag = 0;

	tcflush(fd, TCIFLUSH);

	ret = tcsetattr(fd, TCSANOW, &tios);
	if (ret < 0)
		err(1, "unable to update \"%s\" tios", tty);

	return fd;
}

static void fastboot_opened(struct fastboot *fb, void *data)
{
	const uint8_t one = 1;

	warnx("fastboot connection opened");

	cdba_send_buf(MSG_FASTBOOT_PRESENT, 1, &one);
}

static void fastboot_info(struct fastboot *fb, const void *buf, size_t len)
{
	fprintf(stderr, "%s\n", (const char *)buf);
}

static void fastboot_disconnect(void *data)
{
	const uint8_t zero = 0;

	cdba_send_buf(MSG_FASTBOOT_PRESENT, 1, &zero);
}

static struct fastboot_ops fastboot_ops = {
	.opened = fastboot_opened,
	.disconnect = fastboot_disconnect,
	.info = fastboot_info,
};

static void msg_select_board(const void *param)
{
	selected_device = device_open(param, username, &fastboot_ops);
	if (!selected_device) {
		fprintf(stderr, "failed to open %s\n", (const char *)param);
		quit_invoked = true;
	}

	cdba_send(MSG_SELECT_BOARD);
}

static void *fastboot_payload;
static size_t fastboot_size;

static void msg_fastboot_download(const void *data, size_t len)
{
	size_t new_size = fastboot_size + len;
	char *newp;

	newp = realloc(fastboot_payload, new_size);
	if (!newp)
		err(1, "failed too expant fastboot scratch area");

	memcpy(newp + fastboot_size, data, len);

	fastboot_payload = newp;
	fastboot_size = new_size;

	if (!len) {
		device_boot(selected_device, fastboot_payload, fastboot_size);

		cdba_send(MSG_FASTBOOT_DOWNLOAD);
		free(fastboot_payload);
		fastboot_payload = NULL;
		fastboot_size = 0;
	}
}

static void msg_fastboot_continue(void)
{
	device_fastboot_continue(selected_device);
	cdba_send(MSG_FASTBOOT_CONTINUE);
}

void cdba_send_buf(int type, size_t len, const void *buf)
{
	struct msg msg = {
		.type = type,
		.len = len
	};

	write(STDOUT_FILENO, &msg, sizeof(msg));
	if (len)
		write(STDOUT_FILENO, buf, len);
}

static int handle_stdin(int fd, void *buf)
{
	static struct circ_buf recv_buf = { };
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
			device_power(selected_device, true);

			cdba_send(MSG_POWER_ON);
			break;
		case MSG_POWER_OFF:
			device_power(selected_device, false);

			cdba_send(MSG_POWER_OFF);
			break;
		case MSG_FASTBOOT_DOWNLOAD:
			msg_fastboot_download(msg->data, msg->len);
			break;
		case MSG_FASTBOOT_BOOT:
			// fprintf(stderr, "fastboot boot\n");
			break;
		case MSG_STATUS_UPDATE:
			device_status_enable(selected_device);
			break;
		case MSG_VBUS_ON:
			device_usb(selected_device, true);
			break;
		case MSG_VBUS_OFF:
			device_usb(selected_device, false);
			break;
		case MSG_SEND_BREAK:
			device_send_break(selected_device);
			break;
		case MSG_LIST_DEVICES:
			device_list_devices(username);
			break;
		case MSG_BOARD_INFO:
			device_info(username, msg->data, msg->len);
			break;
		case MSG_FASTBOOT_CONTINUE:
			msg_fastboot_continue();
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

struct timer {
	struct list_head node;
	struct timeval tv;

	void (*cb)(void *);
	void *data;
};

static struct list_head read_watches = LIST_INIT(read_watches);
static struct list_head timer_watches = LIST_INIT(timer_watches);

void watch_add_readfd(int fd, int (*cb)(int, void*), void *data)
{
	struct watch *w;

	w = calloc(1, sizeof(*w));
	w->fd = fd;
	w->cb = cb;
	w->data = data;

	list_add(&read_watches, &w->node);
}

void watch_timer_add(int timeout_ms, void (*cb)(void *), void *data)
{
	struct timeval tv_timeout;
	struct timeval now;
	struct timer *t;

	t = calloc(1, sizeof(*t));

	gettimeofday(&now, NULL);

	tv_timeout.tv_sec = timeout_ms / 1000;
	tv_timeout.tv_usec = (timeout_ms % 1000) * 1000;

	t->cb = cb;
	t->data = data;
	timeradd(&now, &tv_timeout, &t->tv);

	list_add(&timer_watches, &t->node);
}

static struct timeval *watch_timer_next(void)
{
	static struct timeval timeout;
	struct timeval now;
	struct timer *next;
	struct timer *t;

	if (list_empty(&timer_watches))
		return NULL;

	next = list_entry_first(&timer_watches, struct timer, node);

	list_for_each_entry(t, &timer_watches, node) {
		if (timercmp(&t->tv, &next->tv, <))
			next = t;
	}

	gettimeofday(&now, NULL);
	timersub(&next->tv, &now, &timeout);
	if (timeout.tv_sec < 0) {
		timeout.tv_sec = 0;
		timeout.tv_usec = 0;
	}

	return &timeout;
}

static void watch_timer_invoke(void)
{
	struct timeval now;
	struct timer *tmp;
	struct timer *t;

	gettimeofday(&now, NULL);

	list_for_each_entry_safe(t, tmp, &timer_watches, node) {
		if (timercmp(&t->tv, &now, <)) {
			t->cb(t->data);

			list_del(&t->node);
			free(t);
		}
	}
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
	struct timeval *timeoutp;
	struct watch *w;
	fd_set rfds;
	int flags;
	int nfds;
	int ret;

	signal(SIGPIPE, sigpipe_handler);

	fprintf(stderr, "Starting cdba server\n");

	username = getenv("CDBA_USER");
	if (!username)
		username = getenv("USER");
	if (!username)
		username = "nobody";

	openlog("cdba-server", 0, LOG_DAEMON);

	ret = device_parser(".cdba");
	if (ret) {
		ret = device_parser("/etc/cdba");
		if (ret) {
			fprintf(stderr, "device parser: unable to open config file\n");
			exit(1);
		}
	}

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

		timeoutp = watch_timer_next();
		ret = select(nfds + 1, &rfds, NULL, NULL, timeoutp);
		if (ret < 0 && errno == EINTR)
			continue;
		else if (ret < 0)
			break;

		watch_timer_invoke();

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

	/* if we got here, stdin/out/err might be not accessible anymore */
	ret = open("/dev/null", O_RDWR);
	if (ret >= 0) {
		close(STDIN_FILENO);
		dup2(ret, STDIN_FILENO);
		close(STDOUT_FILENO);
		dup2(ret, STDOUT_FILENO);
		close(STDERR_FILENO);
		dup2(ret, STDERR_FILENO);
	}

	if (selected_device)
		device_close(selected_device);

	return 0;
}
