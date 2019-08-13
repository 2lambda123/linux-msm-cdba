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
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <alloca.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "cdba.h"
#include "circ_buf.h"
#include "list.h"

static bool quit;

static const char *fastboot_file;

static struct termios *tty_unbuffer(void)
{
	static struct termios orig_tios;
	struct termios tios;
	int ret;

	ret = tcgetattr(STDIN_FILENO, &orig_tios);
	if (ret < 0) {
		/* stdin is not a tty */
		if (errno == ENOTTY)
			return NULL;
		err(1, "unable to retrieve tty tios");
	}

	memcpy(&tios, &orig_tios, sizeof(struct termios));
	tios.c_lflag &= ~(ICANON | ECHO | ISIG);
	tios.c_iflag &= ~(ISTRIP | IGNCR | ICRNL | INLCR | IXOFF | IXON);
	tios.c_cc[VTIME] = 0;
	tios.c_cc[VMIN] = 1;
	ret = tcsetattr(STDIN_FILENO, TCSANOW, &tios);
	if (ret)
		err(1, "unable to update tty tios");

	return &orig_tios;
}

static void tty_reset(struct termios *orig_tios)
{
	int ret;

	if (!orig_tios)
		return;

	tcflush(STDIN_FILENO, TCIFLUSH);
	ret = tcsetattr(STDIN_FILENO, TCSANOW, orig_tios);
	if (ret < 0)
		warn("unable to reset tty tios");
}

static int fork_ssh(const char *host, const char *cmd, int *pipes)
{
	int piped_stdin[2];
	int piped_stdout[2];
	int piped_stderr[2];
	pid_t pid;
	int flags;
	int i;

	pipe(piped_stdin);
	pipe(piped_stdout);
	pipe(piped_stderr);

	pid = fork();
	switch(pid) {
	case -1:
		err(1, "failed to fork");
	case 0:
		dup2(piped_stdin[0], STDIN_FILENO);
		dup2(piped_stdout[1], STDOUT_FILENO);
		dup2(piped_stderr[1], STDERR_FILENO);

		close(piped_stdin[0]);
		close(piped_stdin[1]);

		close(piped_stdout[0]);
		close(piped_stdout[1]);

		close(piped_stderr[0]);
		close(piped_stderr[1]);

		execl("/usr/bin/ssh", "ssh", host, cmd, NULL);
		err(1, "launching ssh failed");
	default:
		close(piped_stdin[0]);
		close(piped_stdout[1]);
		close(piped_stderr[1]);
	}

	pipes[0] = piped_stdin[1];
	pipes[1] = piped_stdout[0];
	pipes[2] = piped_stderr[0];

	for (i = 0; i < 3; i++) {
		flags = fcntl(pipes[i], F_GETFL, 0);
		fcntl(pipes[i], F_SETFL, flags | O_NONBLOCK);
	}

	return 0;
}

static int tty_callback(int *ssh_fds)
{
	static bool special;
	struct msg hdr;
	char buf[32];
	ssize_t k;
	ssize_t n;

	n = read(STDIN_FILENO, buf, sizeof(buf));
	if (n < 0)
		return n;

	for (k = 0; k < n; k++) {
		if (buf[k] == 0x1) {
			special = true;
		} else if (special) {
			switch (buf[k]) {
			case 'q':
				quit = true;
				break;
			case 'P':
				hdr.type = MSG_POWER_ON;
				hdr.len = 0;
				write(ssh_fds[0], &hdr, sizeof(hdr));
				break;
			case 'p':
				hdr.type = MSG_POWER_OFF;
				hdr.len = 0;
				write(ssh_fds[0], &hdr, sizeof(hdr));
				break;
			case 's':
				hdr.type = MSG_STATUS_UPDATE;
				hdr.len = 0;
				write(ssh_fds[0], &hdr, sizeof(hdr));
				break;
			case 'V':
				hdr.type = MSG_VBUS_ON;
				hdr.len = 0;
				write(ssh_fds[0], &hdr, sizeof(hdr));
				break;
			case 'v':
				hdr.type = MSG_VBUS_OFF;
				hdr.len = 0;
				write(ssh_fds[0], &hdr, sizeof(hdr));
				break;
			case 'a':
				hdr.type = MSG_CONSOLE;
				hdr.len = 1;

				write(ssh_fds[0], &hdr, sizeof(hdr));
				write(ssh_fds[0], "\001", 1);
				break;
#if 0
			case 'b':
				device_break(device);
				break;
#endif
			}

			special = false;
		} else {
			hdr.type = MSG_CONSOLE;
			hdr.len = 1;

			write(ssh_fds[0], &hdr, sizeof(hdr));
			write(ssh_fds[0], buf + k, 1);
		}
	}

	return 0;
}

struct work {
	void (*fn)(struct work *work, int ssh_stdin);

	struct list_head node;
};

static struct list_head work_items = LIST_INIT(work_items);

struct select_board {
	struct work work;

	const char *board;
};

static void select_board_fn(struct work *work, int ssh_stdin)
{
	struct select_board *board = container_of(work, struct select_board, work);
	size_t blen = strlen(board->board) + 1;
	struct msg *msg;
	ssize_t n;

	msg = alloca(sizeof(*msg) + blen);
	msg->type = MSG_SELECT_BOARD;
	msg->len = blen;
	memcpy(msg->data, board->board, blen);

	n = write(ssh_stdin, msg, sizeof(*msg) + blen);
	if (n < 0)
		err(1, "failed to send power on request");

	free(work);
}

static void request_select_board(const char *board)
{
	struct select_board *work;

	work = malloc(sizeof(*work));
	work->work.fn = select_board_fn;
	work->board = board;

	list_add(&work_items, &work->work.node);
}

static void request_power_on_fn(struct work *work, int ssh_stdin)
{
	struct msg msg = { MSG_POWER_ON, };
	ssize_t n;

	n = write(ssh_stdin, &msg, sizeof(msg));
	if (n < 0)
		err(1, "failed to send power on request");
}

static void request_power_off_fn(struct work *work, int ssh_stdin)
{
	struct msg msg = { MSG_POWER_OFF, };
	ssize_t n;

	n = write(ssh_stdin, &msg, sizeof(msg));
	if (n < 0)
		err(1, "failed to send power off request");
}

static void request_power_on(void)
{
	static struct work work = { request_power_on_fn };

	list_add(&work_items, &work.node);
}

static void request_power_off(void)
{
	static struct work work = { request_power_off_fn };

	list_add(&work_items, &work.node);
}

struct fastboot_download_work {
	struct work work;

	void *data;
	size_t offset;
	size_t size;
};

static void fastboot_work_fn(struct work *_work, int ssh_stdin)
{
	struct fastboot_download_work *work = container_of(_work, struct fastboot_download_work, work);
	struct msg *msg;
	size_t left;
	ssize_t n;

	left = MIN(2048, work->size - work->offset);

	msg = alloca(sizeof(*msg) + left);
	msg->type = MSG_FASTBOOT_DOWNLOAD;
	msg->len = left;
	memcpy(msg->data, work->data + work->offset, left);

	n = write(ssh_stdin, msg, sizeof(*msg) + msg->len);
	if (n < 0 && errno == EAGAIN) {
		list_add(&work_items, &_work->node);
		return;
	} else if (n < 0) {
		err(1, "failed to write fastboot message");
	}

	work->offset += msg->len;

	/* We've sent the entire image, and a zero length packet */
	if (!msg->len)
		free(work);
	else
		list_add(&work_items, &_work->node);
}

static void request_fastboot_files(void)
{
	struct fastboot_download_work *work;
	struct stat sb;
	int fd;

	work = calloc(1, sizeof(*work));
	work->work.fn = fastboot_work_fn;

	fd = open(fastboot_file, O_RDONLY);
	if (fd < 0)
		err(1, "failed to open \"%s\"", fastboot_file);

	fstat(fd, &sb);

	work->size = sb.st_size;
	work->data = malloc(work->size);
	read(fd, work->data, work->size);
	close(fd);

	list_add(&work_items, &work->work.node);
}

static void handle_status_update(const void *data, size_t len)
{
	char *str = alloca(len + 1);

	memcpy(str, data, len);
	str[len] = '\0';

	printf("%s\n", str);
}

static bool received_power_off;
static bool reached_timeout;

static void handle_console(const void *data, size_t len)
{
	static int power_off_chars = 0;
	const char *p = data;
	int i;

	for (i = 0; i < len; i++) {
		if (*p++ == '~') {
			if (power_off_chars++ == 19) {
				received_power_off = true;
				power_off_chars = 0;
			}
		} else {
			power_off_chars = 0;
		}
	}

	write(STDOUT_FILENO, data, len);
}

static bool auto_power_on;

static int handle_message(struct circ_buf *buf)
{
	struct msg *msg;
	struct msg hdr;
	size_t n;

	for (;;) {
		n = circ_peak(buf, &hdr, sizeof(hdr));
		if (n != sizeof(hdr))
			return 0;

		if (CIRC_AVAIL(buf) < sizeof(*msg) + hdr.len)
			return 0;

		// fprintf(stderr, "avail: %zd hdr.len: %d\n", CIRC_AVAIL(buf), hdr.len);

		msg = malloc(sizeof(*msg) + hdr.len);
		circ_read(buf, msg, sizeof(*msg) + hdr.len);

		switch (msg->type) {
		case MSG_SELECT_BOARD:
			// printf("======================================== MSG_SELECT_BOARD\n");
			request_power_on();
			break;
		case MSG_CONSOLE:
			handle_console(msg->data, msg->len);
			break;
		case MSG_HARDRESET:
			break;
		case MSG_POWER_ON:
			// printf("======================================== MSG_POWER_ON\n");
			break;
		case MSG_POWER_OFF:
			// printf("======================================== MSG_POWER_OFF\n");
			if (auto_power_on) {
				sleep(2);
				request_power_on();
			}
			break;
		case MSG_FASTBOOT_PRESENT:
			if (*(uint8_t*)msg->data) {
				// printf("======================================== MSG_FASTBOOT_PRESENT(on)\n");
				request_fastboot_files();
			} else {
				// printf("======================================== MSG_FASTBOOT_PRESENT(off)\n");
			}
			break;
		case MSG_FASTBOOT_DOWNLOAD:
			// printf("======================================== MSG_FASTBOOT_DOWNLOAD\n");
			break;
		case MSG_FASTBOOT_BOOT:
			// printf("======================================== MSG_FASTBOOT_BOOT\n");
			break;
		case MSG_STATUS_UPDATE:
			handle_status_update(msg->data, msg->len);
			break;
		default:
			fprintf(stderr, "unk %d len %d\n", msg->type, msg->len);
			exit(1);
		}

		free(msg);
	}

	return 0;
}

static void usage(void)
{
	extern const char *__progname;

	fprintf(stderr, "usage: %s -b <board> -h <host> [-t <timeout>] "
			"[-T <inactivity-timeout>] boot.img\n",
			__progname);
	exit(1);
}

int main(int argc, char **argv)
{
	bool power_cycle_on_timeout = true;
	bool timeout_on_inactivity = true;
	struct termios *orig_tios;
	struct work *next;
	struct work *work;
	struct circ_buf recv_buf = { 0 };
	const char *board = NULL;
	const char *host = NULL;
	struct timeval tv;
	int power_cycles = 0;
	struct stat sb;
	int ssh_fds[3];
	char buf[128];
	fd_set rfds;
	fd_set wfds;
	int timeout = 600;
	ssize_t n;
	int nfds;
	int opt;
	int ret;

	while ((opt = getopt(argc, argv, "b:c:C:h:t:T:")) != -1) {
		switch (opt) {
		case 'b':
			board = optarg;
			break;
		case 'C':
			power_cycle_on_timeout = false;
			/* FALLTHROUGH */
		case 'c':
			power_cycles = atoi(optarg);
			break;
		case 'h':
			host = optarg;
			break;
		case 't':
			timeout = atoi(optarg);
			timeout_on_inactivity = false;
			break;
		case 'T':
			timeout = atoi(optarg);
			break;
		default:
			usage();
		}
	}

	if (optind >= argc || !board || !host)
		usage();

	fastboot_file = argv[optind];
	if (lstat(fastboot_file, &sb))
		err(1, "unable to read \"%s\"", fastboot_file);
	if (!S_ISREG(sb.st_mode))
		errx(1, "\"%s\" is not a regular file", fastboot_file);

	request_select_board(board);

	ret = fork_ssh(host, "sandbox/cdba/bad", ssh_fds);
	if (ret)
		err(1, "failed to connect to \"%s\"", host);

	//sleep(5);

	tv.tv_sec = timeout;
	tv.tv_usec = 0;

	orig_tios = tty_unbuffer();

	while (!quit) {
		if (received_power_off || reached_timeout) {
			if (!power_cycles)
				break;

			if (reached_timeout && !power_cycle_on_timeout)
				break;

			printf("power cycle (%d left)\n", power_cycles);
			fflush(stdout);

			auto_power_on = true;
			power_cycles--;
			received_power_off = false;
			reached_timeout = false;

			request_power_off();

			tv.tv_sec = timeout;
			tv.tv_usec = 0;
		}

		FD_ZERO(&rfds);
		FD_SET(ssh_fds[1], &rfds);
		FD_SET(ssh_fds[2], &rfds);
		nfds = MAX(ssh_fds[1], ssh_fds[2]);

		if (orig_tios) {
			FD_SET(STDIN_FILENO, &rfds);

			nfds = MAX(nfds, STDIN_FILENO);
		}

		FD_ZERO(&wfds);
		if (!list_empty(&work_items))
			FD_SET(ssh_fds[0], &wfds);

		ret = select(nfds + 1, &rfds, &wfds, NULL, timeout ? &tv : NULL);
#if 0
		printf("select: %d (%c%c%c)\n", ret, FD_ISSET(STDIN_FILENO, &rfds) ? 'X' : '-',
						     FD_ISSET(ssh_fds[1], &rfds) ? 'X' : '-',
						     FD_ISSET(ssh_fds[2], &rfds) ? 'X' : '-');
#endif
		if (ret < 0) {
			err(1, "select");
		} else if (ret == 0) {
			if (timeout_on_inactivity)
				warnx("timeout due to inactivity");
			else
				warnx("timeout reached");

			reached_timeout = true;
		}

		if (FD_ISSET(STDIN_FILENO, &rfds))
			tty_callback(ssh_fds);

		if (FD_ISSET(ssh_fds[2], &rfds)) {
			n = read(ssh_fds[2], buf, sizeof(buf));
			if (!n) {
				warnx("EOF on stderr");
				break;
			} else if (n < 0 && errno == EAGAIN) {
			       continue;
			} else if (n < 0) {
				warn("received %zd on stderr", n);
				break;
			}

			const char blue[] = "\033[94m";
			const char reset[] = "\033[0m";

			write(2, blue, sizeof(blue) - 1);
			write(2, buf, n);
			write(2, reset, sizeof(reset) - 1);
		}

		if (FD_ISSET(ssh_fds[1], &rfds)) {
			ret = circ_fill(ssh_fds[1], &recv_buf);
			if (ret < 0 && errno != EAGAIN) {
				warn("received %d on stdout", ret);
				break;
			}

			n = handle_message(&recv_buf);
			if (n < 0)
				break;

			/* Reset inactivity timeout on activity */
			if (timeout_on_inactivity) {
				tv.tv_sec = timeout;
				tv.tv_usec = 0;
			}
		}

		if (FD_ISSET(ssh_fds[0], &wfds)) {
			list_for_each_entry_safe(work, next, &work_items, node) {
				list_del(&work->node);

				work->fn(work, ssh_fds[0]);
			}
		}
	}

	close(ssh_fds[0]);
	close(ssh_fds[1]);
	close(ssh_fds[2]);

	printf("Waiting for ssh to finish\n");

	wait(NULL);

	tty_reset(orig_tios);

	if (reached_timeout)
		return 110;

	return (quit || received_power_off) ? 0 : 1;
}
