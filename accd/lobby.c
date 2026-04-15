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
 * lobby.c -- Kunos kson lobby client.
 *
 * Wire format reverse-engineered from accServer.exe v1.10.2,
 * captured 2026-04-15.  Endpoint 131.153.158.178:909, framed as
 * u16 LE length + body.  See lobby.h.
 *
 * After TCP connect, the Kunos server sends a 256-byte session-init
 * blob that contains mostly uninitialized stack memory under Wine
 * (visible 64-bit pointers).  The lobby accepts whatever; we send
 * 256 zero bytes.  Then the registration message (id 0x44):
 *
 *   u32 ts_low + u16 0 + u32 6 + u8 0 + u8 0x44 + u8 1 + u8 0x2b
 *   u32 tcp_port
 *   u32 udp_port
 *   u16 + N raw chars: serverName
 *   u8  + N raw chars: trackName
 *   u32 maxConnections
 *   ff fa 01 00 00 01 00 00          <-- magic, semantics unknown
 *   u8  session_count + u8 0
 *   per session (10 bytes):
 *      u8 type, u8 day_of_weekend, u8 hour, u8 duration_min,
 *      u8 0, u16 pre_race_wait_s, u16 overtime_s, u16 0x0100
 *   00 00 40 00 4b                   <-- magic trailer, unknown
 *   u8 64 + 64-byte alphanumeric token A (server fingerprint)
 *   u16 10 + 10-byte alphanumeric token B
 *
 * Periodic keepalive (every 30 s): u16 length 14 + 14-byte body
 * starting with the timestamp + session_id preamble + msg id 0x0d.
 * Server replies with `01 00 fd` (3 bytes).  Driver-count update:
 * u16 length 12 + 12-byte body, msg id 0x00.
 *
 * UNKNOWNS still:
 *   - The two magic byte runs (`ff fa 01 00 00 01 00 00` and
 *     `00 00 40 00 4b`) — semantics unclear, we copy verbatim.
 *   - The two token strings — Kunos generates them at startup
 *     (likely random), we do the same.
 *   - The 256-byte init blob — we send zeros and the lobby accepts.
 */

#define _POSIX_C_SOURCE 200809L

#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

#include "io.h"
#include "log.h"
#include "lobby.h"
#include "prim.h"
#include "state.h"

#define LOBBY_HOST_DEFAULT	"131.153.158.178"
#define LOBBY_PORT_DEFAULT	909
#define LOBBY_RETRY_MS		10000	/* matches Kunos 10s interval */
#define LOBBY_BACKOFF_MAX_MS	300000
#define LOBBY_KEEPALIVE_MS	30000
#define LOBBY_INIT_BLOB_SZ	256

/* msg ids observed on the wire (server -> lobby direction). */
#define LOBBY_MSG_REGISTER	0x44
#define LOBBY_MSG_DRIVERS	0x00
#define LOBBY_MSG_KEEPALIVE	0x0d

static uint64_t
lobby_now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000ull +
	    (uint64_t)ts.tv_nsec / 1000000ull;
}

static void
lobby_random_token(char *out, size_t n)
{
	static const char alpha[] =
	    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
	size_t i;
	unsigned int seed;

	seed = (unsigned int)(lobby_now_ms() ^ (uintptr_t)out);
	for (i = 0; i + 1 < n; i++)
		out[i] = alpha[rand_r(&seed) % (sizeof(alpha) - 1)];
	out[n - 1] = '\0';
}

static void
lobby_set_state(struct LobbyClient *l, enum lobby_state s)
{
	const char *names[] = {
		"DISABLED", "DISCONNECTED", "CONNECTING", "REGISTERING",
		"REGISTERED", "BACKOFF", "PERMANENTLY_DISABLED"
	};
	if (l->state != s)
		log_info("lobby: state %s -> %s", names[l->state], names[s]);
	l->state = s;
	l->state_entered_ms = lobby_now_ms();
}

void
lobby_init(struct LobbyClient *l)
{
	memset(l, 0, sizeof(*l));
	l->fd = -1;
	l->session_id = 6;	/* observed value; lobby may reassign */
	lobby_random_token(l->token_a, sizeof(l->token_a));
	lobby_random_token(l->token_b, sizeof(l->token_b));
	l->state = LOBBY_DISABLED;
}

void
lobby_shutdown(struct LobbyClient *l)
{
	if (l->fd >= 0) {
		close(l->fd);
		l->fd = -1;
	}
	free(l->rx_buf);
	l->rx_buf = NULL;
	l->rx_len = l->rx_cap = 0;
	l->state = LOBBY_DISABLED;
}

int
lobby_poll_fd(const struct LobbyClient *l)
{
	if (l->state == LOBBY_DISABLED ||
	    l->state == LOBBY_PERMANENTLY_DISABLED ||
	    l->state == LOBBY_BACKOFF ||
	    l->state == LOBBY_DISCONNECTED)
		return -1;
	return l->fd;
}

short
lobby_poll_events(const struct LobbyClient *l)
{
	if (l->state == LOBBY_CONNECTING)
		return POLLOUT;
	return POLLIN;
}

static int
lobby_open_socket(struct LobbyClient *l)
{
	int fd;
	struct sockaddr_in sa;
	int flags, rc, on = 1;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		log_warn("lobby: socket: %s", strerror(errno));
		return -1;
	}
	flags = fcntl(fd, F_GETFL, 0);
	if (flags >= 0)
		(void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
	(void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));

	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(LOBBY_PORT_DEFAULT);
	if (inet_pton(AF_INET, LOBBY_HOST_DEFAULT, &sa.sin_addr) != 1) {
		log_warn("lobby: bad host %s", LOBBY_HOST_DEFAULT);
		close(fd);
		return -1;
	}
	rc = connect(fd, (struct sockaddr *)&sa, sizeof(sa));
	if (rc < 0 && errno != EINPROGRESS) {
		log_warn("lobby: connect %s:%d: %s", LOBBY_HOST_DEFAULT,
		    LOBBY_PORT_DEFAULT, strerror(errno));
		close(fd);
		return -1;
	}
	log_info("lobby: connecting to %s:%d (fd=%d)",
	    LOBBY_HOST_DEFAULT, LOBBY_PORT_DEFAULT, fd);
	l->fd = fd;
	return 0;
}

static int
lobby_send_framed(struct LobbyClient *l, const void *body, size_t len)
{
	unsigned char hdr[2];
	struct iovec iov[2];
	ssize_t n;

	if (len > 0xFFFF) {
		log_warn("lobby: oversize msg %zu bytes, dropped", len);
		return -1;
	}
	hdr[0] = (unsigned char)(len & 0xff);
	hdr[1] = (unsigned char)((len >> 8) & 0xff);
	iov[0].iov_base = hdr;
	iov[0].iov_len = 2;
	iov[1].iov_base = (void *)(uintptr_t)body;
	iov[1].iov_len = len;
	n = writev(l->fd, iov, 2);
	if (n < 0) {
		log_warn("lobby: writev: %s", strerror(errno));
		return -1;
	}
	return 0;
}

static int
lobby_send_init_blob(struct LobbyClient *l)
{
	unsigned char zeros[LOBBY_INIT_BLOB_SZ];
	memset(zeros, 0, sizeof(zeros));
	/* Mirror the two visible non-zero values in Kunos's blob: tcp port
	 * and a small-int that may be a packet size hint.  Most other
	 * bytes look like uninitialized stack on Wine, so zero is fine. */
	zeros[0] = 0x10;
	zeros[1] = 0x24;
	return lobby_send_framed(l, zeros, sizeof(zeros));
}

static int
lobby_send_registration(struct LobbyClient *l, const struct Server *s)
{
	struct ByteBuf bb;
	uint64_t now;
	uint8_t i, sess_count;
	size_t name_len, track_len;
	int rc;

	bb_init(&bb);
	now = lobby_now_ms();

	/* Preamble: u32 ts_low + u16 0 + u32 session_id + u8 0 + u8 msg_id */
	if (wr_u32(&bb, (uint32_t)now) < 0) goto err;
	if (wr_u16(&bb, 0) < 0) goto err;
	if (wr_u32(&bb, l->session_id) < 0) goto err;
	if (wr_u8(&bb, 0) < 0) goto err;
	if (wr_u8(&bb, LOBBY_MSG_REGISTER) < 0) goto err;
	if (wr_u8(&bb, 1) < 0) goto err;
	if (wr_u8(&bb, 0x2b) < 0) goto err;

	if (wr_u32(&bb, (uint32_t)s->tcp_port) < 0) goto err;
	if (wr_u32(&bb, (uint32_t)s->udp_port) < 0) goto err;

	name_len = strlen(s->server_name);
	if (name_len > 0xFFFF) name_len = 0xFFFF;
	if (wr_u16(&bb, (uint16_t)name_len) < 0) goto err;
	if (bb_append(&bb, s->server_name, name_len) < 0) goto err;

	track_len = strlen(s->track);
	if (track_len > 0xFF) track_len = 0xFF;
	if (wr_u8(&bb, (uint8_t)track_len) < 0) goto err;
	if (bb_append(&bb, s->track, track_len) < 0) goto err;

	if (wr_u32(&bb, (uint32_t)s->max_connections) < 0) goto err;

	/*
	 * Magic block (post-config): verified byte-exact from a
	 * 178-byte Kunos registration (v1.10.2): `ff fa` then six
	 * bytes whose semantic is unknown but never observed to vary.
	 */
	{
		static const unsigned char magic1[] = {
			0xff, 0xfa, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00
		};
		if (bb_append(&bb, magic1, sizeof(magic1)) < 0) goto err;
	}

	/*
	 * Session block: u8 weatherRandomness + u8 sessionCount, then
	 * each 10-byte session: u8 type, u8 day_of_weekend, u8 hour,
	 * u8 dur_min, u8 pad, u16 pre_race_s, u16 overtime_s, u8 1.
	 */
	sess_count = s->session_count;
	if (wr_u8(&bb, (uint8_t)(s->weather.randomness & 0xff)) < 0)
		goto err;
	if (wr_u8(&bb, sess_count) < 0) goto err;
	for (i = 0; i < sess_count; i++) {
		const struct SessionDef *d = &s->sessions[i];
		uint16_t pre_race = d->session_type == 10 ? 80 : 3;
		if (wr_u8(&bb, d->session_type) < 0) goto err;
		if (wr_u8(&bb, d->day_of_weekend) < 0) goto err;
		if (wr_u8(&bb, d->hour_of_day) < 0) goto err;
		if (wr_u8(&bb, (uint8_t)(d->duration_min & 0xFF)) < 0)
			goto err;
		if (wr_u8(&bb, 0) < 0) goto err;
		if (wr_u16(&bb, pre_race) < 0) goto err;
		if (wr_u16(&bb, s->session_overtime_s > 0
		    ? s->session_overtime_s : 120) < 0) goto err;
		if (wr_u8(&bb, 1) < 0) goto err;
	}

	/*
	 * Token block: 2 magic zero bytes then u16-length-prefixed
	 * token_a (64 alphanumerics — server fingerprint, regenerated
	 * per launch) and u16-length-prefixed token_b (10 alphanumerics).
	 */
	if (wr_u16(&bb, 0) < 0) goto err;
	if (wr_u16(&bb, 64) < 0) goto err;
	if (bb_append(&bb, l->token_a, 64) < 0) goto err;
	if (wr_u16(&bb, 10) < 0) goto err;
	if (bb_append(&bb, l->token_b, 10) < 0) goto err;

	{
		size_t sent = bb.wpos;
		rc = lobby_send_framed(l, bb.data, bb.wpos);
		bb_free(&bb);
		if (rc == 0)
			log_info("lobby: sent registration for %s "
			    "(%zu bytes)", s->track, sent);
	}
	return rc;
err:
	bb_free(&bb);
	return -1;
}

static int
lobby_send_short_msg(struct LobbyClient *l, uint8_t msg_id, uint8_t extra)
{
	struct ByteBuf bb;
	uint64_t now;
	int rc;

	bb_init(&bb);
	now = lobby_now_ms();
	if (wr_u32(&bb, (uint32_t)now) < 0 ||
	    wr_u16(&bb, 0) < 0 ||
	    wr_u32(&bb, l->session_id) < 0 ||
	    wr_u8(&bb, 0) < 0 ||
	    wr_u8(&bb, msg_id) < 0 ||
	    wr_u8(&bb, 0) < 0 ||
	    wr_u8(&bb, extra) < 0) {
		bb_free(&bb);
		return -1;
	}
	rc = lobby_send_framed(l, bb.data, bb.wpos);
	bb_free(&bb);
	return rc;
}

static int
lobby_send_drivers_update(struct LobbyClient *l)
{
	if (lobby_send_short_msg(l, LOBBY_MSG_DRIVERS,
	    l->last_driver_count) < 0)
		return -1;
	log_info("lobby: drivers=%u", (unsigned)l->last_driver_count);
	return 0;
}

static int
lobby_send_keepalive(struct LobbyClient *l)
{
	if (lobby_send_short_msg(l, LOBBY_MSG_KEEPALIVE, 0x0d) < 0)
		return -1;
	l->last_keepalive_ms = lobby_now_ms();
	return 0;
}

static void
lobby_disconnect(struct LobbyClient *l, const char *reason)
{
	uint32_t backoff;

	log_info("lobby: disconnecting (%s)", reason);
	if (l->fd >= 0) {
		close(l->fd);
		l->fd = -1;
	}
	free(l->rx_buf);
	l->rx_buf = NULL;
	l->rx_len = l->rx_cap = 0;
	l->consecutive_fails++;
	backoff = LOBBY_RETRY_MS;
	if (l->consecutive_fails > 3)
		backoff *= (uint32_t)(1 << (l->consecutive_fails - 3));
	if (backoff > LOBBY_BACKOFF_MAX_MS)
		backoff = LOBBY_BACKOFF_MAX_MS;
	(void)backoff;	/* state_entered_ms + LOBBY_RETRY_MS used in tick */
	lobby_set_state(l, LOBBY_BACKOFF);
}

static void
lobby_handle_rx(struct LobbyClient *l, const unsigned char *p, size_t n)
{
	(void)l; (void)p; (void)n;
	/*
	 * Lobby acks are tiny: 4-byte ack after registration
	 * (`02 00 ef 00`) and 3-byte ack after each keepalive
	 * (`01 00 fd`).  We don't act on them today beyond
	 * confirming the connection is alive and bumping the
	 * REGISTERING -> REGISTERED transition on the first ack.
	 */
}

void
lobby_handle_io(struct LobbyClient *l, struct Server *s, short revents)
{
	(void)s;

	if (l->fd < 0)
		return;

	if (l->state == LOBBY_CONNECTING && (revents & POLLOUT)) {
		int err = 0;
		socklen_t slen = sizeof(err);
		if (getsockopt(l->fd, SOL_SOCKET, SO_ERROR, &err, &slen) < 0
		    || err != 0) {
			log_warn("lobby: connect failed: %s",
			    err ? strerror(err) : "unknown");
			lobby_disconnect(l, "connect failed");
			return;
		}
		log_info("lobby: TCP connected");
		if (lobby_send_init_blob(l) < 0 ||
		    lobby_send_registration(l, s) < 0) {
			lobby_disconnect(l, "send register failed");
			return;
		}
		lobby_set_state(l, LOBBY_REGISTERING);
		return;
	}

	/*
	 * Read first, ALWAYS — POLLHUP can arrive in the same poll
	 * iteration as POLLIN when the lobby acks then closes, and
	 * if we drop on HUP without reading we lose the ack and
	 * miss the REGISTERED transition.
	 */
	if (revents & POLLIN) {
		unsigned char buf[4096];
		ssize_t n = read(l->fd, buf, sizeof(buf));
		if (n > 0) {
			lobby_handle_rx(l, buf, (size_t)n);
			if (l->state == LOBBY_REGISTERING) {
				lobby_set_state(l, LOBBY_REGISTERED);
				l->consecutive_fails = 0;
				log_info("lobby: RegisterToLobby "
				    "succeeded");
				(void)lobby_send_drivers_update(l);
			}
		} else if (n == 0) {
			/* EOF — fall through to the HUP/disconnect path. */
			revents |= POLLHUP;
		} else if (errno != EAGAIN && errno != EINTR) {
			lobby_disconnect(l, strerror(errno));
			return;
		}
	}

	if (revents & (POLLERR | POLLHUP | POLLNVAL)) {
		lobby_disconnect(l, "POLLHUP/ERR from peer");
		return;
	}
}

void
lobby_tick(struct LobbyClient *l, struct Server *s)
{
	uint64_t now;

	if (l->state == LOBBY_DISABLED ||
	    l->state == LOBBY_PERMANENTLY_DISABLED)
		return;

	now = lobby_now_ms();

	switch (l->state) {
	case LOBBY_DISCONNECTED:
		if (lobby_open_socket(l) == 0)
			lobby_set_state(l, LOBBY_CONNECTING);
		else
			lobby_set_state(l, LOBBY_BACKOFF);
		break;
	case LOBBY_BACKOFF:
		if (now - l->state_entered_ms >= LOBBY_RETRY_MS)
			lobby_set_state(l, LOBBY_DISCONNECTED);
		break;
	case LOBBY_REGISTERED:
		if (l->drivers_dirty) {
			(void)lobby_send_drivers_update(l);
			l->drivers_dirty = 0;
		}
		if (now - l->last_keepalive_ms >= LOBBY_KEEPALIVE_MS)
			(void)lobby_send_keepalive(l);
		break;
	default:
		break;
	}
	(void)s;
}

void
lobby_notify_drivers_changed(struct LobbyClient *l, uint8_t count)
{
	if (l->state == LOBBY_DISABLED ||
	    l->state == LOBBY_PERMANENTLY_DISABLED)
		return;
	if (count == l->last_driver_count)
		return;
	l->last_driver_count = count;
	l->drivers_dirty = 1;
}

void
lobby_notify_session_changed(struct LobbyClient *l)
{
	if (l->state == LOBBY_DISABLED ||
	    l->state == LOBBY_PERMANENTLY_DISABLED)
		return;
	l->session_dirty = 1;
}

void
lobby_notify_lap(struct LobbyClient *l, uint16_t car_id, int32_t lap_ms)
{
	(void)l; (void)car_id; (void)lap_ms;
	/* TODO: per-lap submission requires a captured "Sent laptime
	 * to kson" payload to know the wire layout.  Stubbed for now;
	 * not blocking — Kunos lobby still lists the server without it. */
}
