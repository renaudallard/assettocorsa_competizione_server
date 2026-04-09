/*
 * Copyright (c) 2025-2026 Renaud Allard
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
/*
 * probe.c -- ACC protocol test client.
 *
 * Connects to an ACC server (real Kunos or accd), performs the 0x09
 * handshake, and dumps all received TCP frames.  Sends UDP keepalives
 * to maintain the session.  Useful for protocol capture and server
 * validation.
 *
 * Compile: make probe
 * Usage:   ./probe -h 172.20.0.74 -p 9232
 */

#define _POSIX_C_SOURCE 200809L

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define ACC_VERSION		0x0100
#define KEEPALIVE_MS		2000
#define DEFAULT_TCP_PORT	9232
#define RXBUF_SIZE		65536

static volatile sig_atomic_t quit;

static void
sighandler(int sig)
{
	(void)sig;
	quit = 1;
}

/* ----- little-endian helpers ------------------------------------- */

static void
put_u8(unsigned char **p, uint8_t v)
{
	*(*p)++ = v;
}

static void
put_u16(unsigned char **p, uint16_t v)
{
	(*p)[0] = (unsigned char)(v & 0xff);
	(*p)[1] = (unsigned char)((v >> 8) & 0xff);
	*p += 2;
}

static void
put_u32(unsigned char **p, uint32_t v)
{
	(*p)[0] = (unsigned char)(v & 0xff);
	(*p)[1] = (unsigned char)((v >> 8) & 0xff);
	(*p)[2] = (unsigned char)((v >> 16) & 0xff);
	(*p)[3] = (unsigned char)((v >> 24) & 0xff);
	*p += 4;
}

static uint16_t
get_u16(const unsigned char *p)
{
	return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t
get_u32(const unsigned char *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	    ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/*
 * Write a Format-A string (ASCII only; each byte becomes a u32
 * code point, prefixed by a u8 count).
 */
static void
put_str_a(unsigned char **p, const char *s)
{
	size_t len = s ? strlen(s) : 0;
	size_t i;

	if (len > 255)
		len = 255;
	put_u8(p, (uint8_t)len);
	for (i = 0; i < len; i++)
		put_u32(p, (uint32_t)(unsigned char)s[i]);
}

/* ----- hex dump -------------------------------------------------- */

static void
hexdump(const char *label, const unsigned char *data, size_t len)
{
	size_t i, j, start;

	printf("%s (%zu bytes):\n", label, len);
	for (i = 0; i < len; i++) {
		if (i % 16 == 0)
			printf("  %04zx: ", i);
		printf("%02x ", data[i]);
		if (i % 16 == 15 || i == len - 1) {
			for (j = i % 16; j < 15; j++)
				printf("   ");
			printf(" ");
			start = i - (i % 16);
			for (j = start; j <= i; j++)
				putchar(data[j] >= 0x20 && data[j] < 0x7f
				    ? (int)data[j] : '.');
			putchar('\n');
		}
	}
}

/* ----- message names --------------------------------------------- */

static const char *
msg_name(uint8_t id)
{
	switch (id) {
	case 0x0b: return "HANDSHAKE_RESPONSE";
	case 0x0c: return "REJECT";
	case 0x14: return "KEEPALIVE";
	case 0x1b: return "LAP_BROADCAST";
	case 0x1e: return "PERCAR_FAST";
	case 0x23: return "CAR_INFO_RESPONSE";
	case 0x24: return "CAR_DISCONNECT";
	case 0x28: return "LARGE_STATE";
	case 0x2b: return "CHAT_OR_STATE";
	case 0x2e: return "CAR_SYSTEM_RELAY";
	case 0x2f: return "TYRE_COMPOUND_RELAY";
	case 0x36: return "LEADERBOARD";
	case 0x37: return "WEATHER";
	case 0x39: return "PERCAR_SLOW";
	case 0x3e: return "SESSION_RESULTS";
	case 0x3f: return "GRID_POSITIONS";
	case 0x40: return "WEEKEND_RESET";
	case 0x4e: return "RATING_SUMMARY";
	case 0x4f: return "DRIVER_STINT_RELAY";
	case 0x53: return "BOP_UPDATE";
	case 0x5b: return "CTRL_INFO_REQUEST";
	case 0x5d: return "CONNECTIONS_LIST_ROW";
	default:   return "???";
	}
}

/* ----- TCP framing ----------------------------------------------- */

static int
tcp_send_framed(int fd, const unsigned char *body, size_t len)
{
	unsigned char hdr[6];
	struct iovec iov[2];
	size_t hdr_sz;

	if (len < 0xFFFF) {
		hdr[0] = (unsigned char)(len & 0xff);
		hdr[1] = (unsigned char)((len >> 8) & 0xff);
		hdr_sz = 2;
	} else {
		hdr[0] = 0xff;
		hdr[1] = 0xff;
		hdr[2] = (unsigned char)(len & 0xff);
		hdr[3] = (unsigned char)((len >> 8) & 0xff);
		hdr[4] = (unsigned char)((len >> 16) & 0xff);
		hdr[5] = (unsigned char)((len >> 24) & 0xff);
		hdr_sz = 6;
	}
	iov[0].iov_base = hdr;
	iov[0].iov_len = hdr_sz;
	iov[1].iov_base = (void *)(uintptr_t)body;
	iov[1].iov_len = len;
	return writev(fd, iov, 2) < 0 ? -1 : 0;
}

/* ----- build 0x09 handshake -------------------------------------- */

static size_t
build_handshake(unsigned char *buf, const char *password,
    const char *steam_id)
{
	unsigned char *p = buf;

	put_u8(&p, 0x09);		/* msg_id */
	put_u16(&p, ACC_VERSION);	/* protocol version */
	put_str_a(&p, password);	/* password */
	put_str_a(&p, "Lifecycle");	/* first_name */
	put_str_a(&p, "LFT");		/* last_name */
	put_str_a(&p, "LFT");		/* short_name */
	put_u8(&p, 0);			/* driver_category */
	put_u16(&p, 0);		/* nationality */
	put_str_a(&p, steam_id);	/* steam_id */
	put_u32(&p, 42);		/* race_number (i32) */
	put_u8(&p, 0);			/* car_model */
	put_u8(&p, 0);			/* cup_category */
	put_str_a(&p, "");		/* team_name */

	return (size_t)(p - buf);
}

/* ----- frame save ------------------------------------------------ */

static void
save_frame(const char *dir, uint8_t msg_id, int seq,
    const unsigned char *data, size_t len)
{
	char path[512];
	FILE *f;

	if (dir == NULL)
		return;
	snprintf(path, sizeof(path), "%s/0x%02x_%03d.bin",
	    dir, msg_id, seq);
	f = fopen(path, "wb");
	if (f == NULL) {
		fprintf(stderr, "save: %s: %s\n", path, strerror(errno));
		return;
	}
	fwrite(data, 1, len, f);
	fclose(f);
	printf("  -> %s\n", path);
}

/* ----- main ------------------------------------------------------ */

static void
usage(void)
{
	fprintf(stderr,
	    "usage: probe [-h host] [-p port] [-P password] [-s steamid]\n"
	    "             [-d savedir] [-t timeout] [-v]\n");
	exit(1);
}

int
main(int argc, char *argv[])
{
	const char *host = "127.0.0.1";
	int tcp_port = DEFAULT_TCP_PORT;
	const char *password = "";
	const char *steam_id = "S76561198000000001";
	const char *save_dir = NULL;
	int timeout_sec = 30;
	int verbose = 0;
	int ch;

	struct sigaction sa;
	struct sockaddr_in addr, udp_addr;
	unsigned char hs_buf[4096], rxbuf[RXBUF_SIZE];
	size_t hs_len, rxlen;
	int tcp_fd, udp_fd, udp_port, frame_seq;
	uint16_t conn_id;
	uint64_t last_ka;
	time_t start;

	while ((ch = getopt(argc, argv, "h:p:P:s:d:t:v")) != -1) {
		switch (ch) {
		case 'h': host = optarg; break;
		case 'p': tcp_port = atoi(optarg); break;
		case 'P': password = optarg; break;
		case 's': steam_id = optarg; break;
		case 'd': save_dir = optarg; break;
		case 't': timeout_sec = atoi(optarg); break;
		case 'v': verbose = 1; break;
		default:  usage();
		}
	}

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sighandler;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	/* TCP connect. */
	printf("probe: connecting to %s:%d\n", host, tcp_port);
	tcp_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (tcp_fd < 0) {
		perror("socket");
		return 1;
	}
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons((uint16_t)tcp_port);
	if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
		fprintf(stderr, "bad address: %s\n", host);
		return 1;
	}
	if (connect(tcp_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("connect");
		return 1;
	}
	printf("probe: connected\n");

	/* Send handshake. */
	hs_len = build_handshake(hs_buf, password, steam_id);
	printf("probe: tx 0x09 handshake (%zu bytes)\n", hs_len);
	if (verbose)
		hexdump("tx 0x09", hs_buf, hs_len);
	if (tcp_send_framed(tcp_fd, hs_buf, hs_len) < 0) {
		perror("send");
		return 1;
	}

	/* Read loop. */
	udp_fd = -1;
	udp_port = 0;
	conn_id = 0;
	rxlen = 0;
	frame_seq = 0;
	last_ka = 0;
	start = time(NULL);

	while (!quit) {
		struct pollfd pfd;
		struct timespec ts;
		uint64_t now_ms;
		int poll_ms, n;
		ssize_t r;

		clock_gettime(CLOCK_MONOTONIC, &ts);
		now_ms = (uint64_t)ts.tv_sec * 1000 +
		    (uint64_t)ts.tv_nsec / 1000000;

		if (timeout_sec > 0 &&
		    time(NULL) - start >= timeout_sec) {
			printf("probe: timeout (%ds)\n", timeout_sec);
			break;
		}

		poll_ms = (udp_fd >= 0)
		    ? (int)(KEEPALIVE_MS - (now_ms - last_ka))
		    : 1000;
		if (poll_ms < 0)
			poll_ms = 0;
		if (poll_ms > 1000)
			poll_ms = 1000;

		pfd.fd = tcp_fd;
		pfd.events = POLLIN;
		pfd.revents = 0;

		n = poll(&pfd, 1, poll_ms);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			perror("poll");
			break;
		}

		/* UDP keepalive. */
		clock_gettime(CLOCK_MONOTONIC, &ts);
		now_ms = (uint64_t)ts.tv_sec * 1000 +
		    (uint64_t)ts.tv_nsec / 1000000;
		if (udp_fd >= 0 && now_ms - last_ka >= KEEPALIVE_MS) {
			unsigned char ka = 0x13;

			sendto(udp_fd, &ka, 1, 0,
			    (struct sockaddr *)&udp_addr,
			    sizeof(udp_addr));
			last_ka = now_ms;
		}

		if (n == 0)
			continue;

		if (!(pfd.revents & (POLLIN | POLLHUP)))
			continue;

		r = recv(tcp_fd, rxbuf + rxlen,
		    sizeof(rxbuf) - rxlen, 0);
		if (r <= 0) {
			if (r == 0)
				printf("probe: server closed connection\n");
			else
				perror("recv");
			break;
		}
		rxlen += (size_t)r;

		/* Extract framed messages. */
		while (rxlen >= 2) {
			size_t flen, hdr_sz, consumed;
			uint16_t len16;
			const unsigned char *body;
			uint8_t msg_id;

			len16 = get_u16(rxbuf);
			if (len16 == 0xFFFF) {
				if (rxlen < 6)
					break;
				flen = (size_t)get_u32(rxbuf + 2);
				hdr_sz = 6;
			} else {
				flen = len16;
				hdr_sz = 2;
			}
			if (rxlen < hdr_sz + flen)
				break;

			body = rxbuf + hdr_sz;
			msg_id = body[0];

			printf("[%03d] rx 0x%02x %-24s %zu bytes\n",
			    frame_seq, msg_id, msg_name(msg_id), flen);

			if (verbose)
				hexdump("  body", body, flen);

			save_frame(save_dir, msg_id, frame_seq,
			    body, flen);

			/* Parse 0x0b accept. */
			if (msg_id == 0x0b && flen >= 8) {
				udp_port = get_u16(body + 1);
				conn_id = get_u16(body + 6);
				printf("  accept: udp=%d conn_id=%u "
				    "nconns=%u flags=0x%02x\n",
				    udp_port, (unsigned)conn_id,
				    (unsigned)get_u16(body + 4),
				    body[3]);

				udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
				if (udp_fd >= 0) {
					memset(&udp_addr, 0,
					    sizeof(udp_addr));
					udp_addr.sin_family = AF_INET;
					udp_addr.sin_port =
					    htons((uint16_t)udp_port);
					udp_addr.sin_addr = addr.sin_addr;
					printf("  keepalive -> %s:%d\n",
					    host, udp_port);
					last_ka = now_ms;
				}
			}

			/* Parse 0x0c reject. */
			if (msg_id == 0x0c) {
				printf("  ** REJECTED **\n");
				if (flen >= 12)
					printf("  server_ver=0x%04x\n",
					    get_u16(body + 10));
				quit = 1;
			}

			frame_seq++;
			consumed = hdr_sz + flen;
			memmove(rxbuf, rxbuf + consumed,
			    rxlen - consumed);
			rxlen -= consumed;
		}
	}

	/* Clean disconnect. */
	if (tcp_fd >= 0) {
		unsigned char disc = 0x10;

		tcp_send_framed(tcp_fd, &disc, 1);
		printf("probe: sent 0x10 disconnect\n");
		close(tcp_fd);
	}
	if (udp_fd >= 0)
		close(udp_fd);

	printf("probe: %d frames received\n", frame_seq);
	return 0;
}
