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
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY,
 * OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
/*
 * lan.c -- LAN / remote discovery (UDP 8999).
 *
 * The ACC client sends a discovery probe to port 8999, either as a
 * LAN broadcast or directed to an IP from serverList.json.  The
 * probe is framed as: u8(0xbf) + u8(0x48) + u32(nonce).  The
 * server replies with: u8(0xc0) + str_a(server_name) + u8(clients)
 * + u8(has_password) + u16(tcp_port) + u32(echo_nonce) +
 * u8(session_type).
 */

#define _POSIX_C_SOURCE 200809L

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

#include "io.h"
#include "lan.h"
#include "log.h"
#include "msg.h"
#include "prim.h"
#include "state.h"

#define LAN_RECV_BUF	2048

int
lan_open(int *out_fd)
{
	int fd, on = 1;
	struct sockaddr_in sa;

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		log_err("lan: socket: %s", strerror(errno));
		return -1;
	}
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0)
		log_warn("lan: SO_REUSEADDR: %s", strerror(errno));
	if (setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on)) < 0)
		log_warn("lan: SO_BROADCAST: %s", strerror(errno));
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_ANY);
	sa.sin_port = htons(ACC_LAN_DISCOVERY_PORT);
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		log_warn("lan: bind 0.0.0.0:%d: %s",
		    ACC_LAN_DISCOVERY_PORT, strerror(errno));
		close(fd);
		return -1;
	}
	*out_fd = fd;
	log_info("lan discovery listening on udp/%d", ACC_LAN_DISCOVERY_PORT);
	return 0;
}

void
lan_handle(struct Server *s, int fd)
{
	unsigned char buf[LAN_RECV_BUF];
	struct sockaddr_in from;
	socklen_t fromlen = sizeof(from);
	ssize_t n;
	struct ByteBuf reply;
	struct Reader r;
	uint8_t envelope, sub;
	uint32_t nonce;
	int i, clients;

	n = recvfrom(fd, buf, sizeof(buf), 0,
	    (struct sockaddr *)&from, &fromlen);
	if (n < 0) {
		if (errno != EINTR && errno != EAGAIN)
			log_warn("lan recvfrom: %s", strerror(errno));
		return;
	}

	/*
	 * Probe format: u8(0xbf) + u8(0x48) + u32(nonce).
	 * Minimum 6 bytes.
	 */
	rd_init(&r, buf, (size_t)n);
	if (rd_u8(&r, &envelope) < 0 || envelope != 0xbf) {
		log_warn("lan: unexpected envelope 0x%02x from %s:%u",
		    (unsigned)buf[0], inet_ntoa(from.sin_addr),
		    ntohs(from.sin_port));
		return;
	}
	if (rd_u8(&r, &sub) < 0 || sub != ACP_LAN_DISCOVER) {
		log_warn("lan: unexpected sub-opcode 0x%02x from %s:%u",
		    (unsigned)sub, inet_ntoa(from.sin_addr),
		    ntohs(from.sin_port));
		return;
	}
	if (rd_u32(&r, &nonce) < 0) {
		log_warn("lan: short probe from %s:%u",
		    inet_ntoa(from.sin_addr), ntohs(from.sin_port));
		return;
	}

	log_info("lan: discovery probe from %s:%u nonce=0x%08x",
	    inet_ntoa(from.sin_addr), ntohs(from.sin_port),
	    (unsigned)nonce);

	clients = 0;
	for (i = 0; i < ACC_MAX_CARS && i < s->max_connections; i++)
		if (s->cars[i].used)
			clients++;

	/*
	 * Response: u8(0xc0) + str_a(server_name) + u8(clients) +
	 * u8(has_password) + u16(tcp_port) + u32(echo_nonce) +
	 * u8(session_type).
	 */
	bb_init(&reply);
	if (wr_u8(&reply, ACP_LAN_RESPONSE) == 0 &&
	    wr_str_a(&reply, s->server_name) == 0 &&
	    wr_u8(&reply, (uint8_t)clients) == 0 &&
	    wr_u8(&reply, s->password[0] != '\0' ? 1 : 0) == 0 &&
	    wr_u16(&reply, (uint16_t)s->tcp_port) == 0 &&
	    wr_u32(&reply, nonce) == 0 &&
	    wr_u8(&reply, s->session_count > 0
		? s->sessions[s->session.session_index].session_type
		: 0) == 0) {
		if (sendto(fd, reply.data, reply.wpos, 0,
		    (struct sockaddr *)&from, fromlen) < 0)
			log_warn("lan sendto: %s", strerror(errno));
	}
	bb_free(&reply);
}
