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
#include "session.h"
#include "state.h"

#define LOBBY_HOST_DEFAULT	"131.153.158.178"
#define LOBBY_PORT_DEFAULT	909
#define LOBBY_RETRY_MS		10000	/* matches Kunos 10s interval */
#define LOBBY_BACKOFF_MAX_MS	300000
#define LOBBY_KEEPALIVE_MS	30000
#define LOBBY_SESSION_UPDATE_MS	20000	/* push at least every 20 s */
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

/*
 * Time since lobby_init in milliseconds.  Kunos sends small ts
 * values in registration / drivers / keepalive (capture shows
 * ~510000 ms = 8 minutes), suggesting process-uptime not absolute
 * monotonic. We use lobby-module uptime so values stay <2^31 even
 * on long-running servers and match the magnitude Kunos sends.
 */
static uint32_t lobby_epoch_ms;

static uint32_t
lobby_uptime_ms(void)
{
	return (uint32_t)(lobby_now_ms() - lobby_epoch_ms);
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
	if (lobby_epoch_ms == 0)
		lobby_epoch_ms = (uint32_t)lobby_now_ms();
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
lobby_send_init_blob(struct LobbyClient *l, uint16_t tcp_port)
{
	/*
	 * The session-init blob is 256 raw bytes (no length prefix).
	 * Reverse-engineered from FUN_14004e400 in accServer.exe:
	 *   ushort local_port            // bytes [0..1]
	 *   char   local_port % 77       // byte 2
	 *   char   local_port % 21       // byte 3
	 *   ... 252 bytes of uninitialised stack ...
	 *   send(sock, &local_port, 256, 0)
	 *
	 * The lobby validates the two modular checksums against the
	 * port — sending stale Kunos-captured bytes for port 9232
	 * here makes the lobby reject the registration with code 4
	 * for any other port we use.  Zero out the rest; the lobby
	 * does not look at it.
	 */
	unsigned char buf[LOBBY_INIT_BLOB_SZ];
	ssize_t n;

	memset(buf, 0, sizeof(buf));
	buf[0] = (unsigned char)(tcp_port & 0xff);
	buf[1] = (unsigned char)((tcp_port >> 8) & 0xff);
	buf[2] = (unsigned char)(tcp_port % 77);
	buf[3] = (unsigned char)(tcp_port % 21);

	n = write(l->fd, buf, sizeof(buf));
	if (n < 0) {
		log_warn("lobby: init write: %s", strerror(errno));
		return -1;
	}
	return 0;
}

static int lobby_write_preamble(struct ByteBuf *bb, struct LobbyClient *l,
    uint8_t type);

static int
lobby_send_registration(struct LobbyClient *l, const struct Server *s)
{
	struct ByteBuf bb;
	uint8_t i, sess_count;
	size_t name_len, track_len;
	int rc;

	bb_init(&bb);

	/*
	 * Preamble (11 bytes, type=0xc8 register) then msg_id 0x44 +
	 * 2 sub bytes (`01 2b`).
	 */
	if (lobby_write_preamble(&bb, l, 0xc8) < 0) goto err;
	if (wr_u8(&bb, LOBBY_MSG_REGISTER) < 0) goto err;
	if (wr_u8(&bb, 0x01) < 0) goto err;
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

	/*
	 * Lobby field is maxCarSlots (rated player capacity), NOT
	 * the TCP max_connections.  Kunos sends 10 here when no
	 * rating requirements are configured.  Sending the wrong
	 * field made the lobby reject our registration with
	 * ack-status 0x04 instead of 0x00.
	 */
	if (wr_u32(&bb, (uint32_t)s->max_car_slots) < 0) goto err;

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

/*
 * Build the 11-byte preamble shared by every framed kson message:
 *   u8 0x3a   (magic)
 *   u8 type   (0xc8 register, 0xd1 drivers, 0xf2 keepalive)
 *   u32 0x7   (protocol version, constant)
 *   u32 0x6   (session id, lobby-assigned in real Kunos but constant 6
 *              works since the field is mostly opaque to the client)
 *   u8 0      (separator)
 * The msg_id byte (e.g. 0x44 register) and any payload follow.
 *
 * Reverse-engineered from FUN_1400448c0 in accServer.exe v1.10.2.
 */
static int
lobby_write_preamble(struct ByteBuf *bb, struct LobbyClient *l, uint8_t type)
{
	if (wr_u8(bb, 0x3a) < 0) return -1;
	if (wr_u8(bb, type) < 0) return -1;
	if (wr_u32(bb, 7) < 0) return -1;
	if (wr_u32(bb, l->session_id) < 0) return -1;
	if (wr_u8(bb, 0) < 0) return -1;
	return 0;
}

static int
lobby_send_drivers_update(struct LobbyClient *l, const struct Server *s)
{
	/*
	 * Drivers update body (FUN_1400473f0):
	 *   11-byte preamble (type 0xd1)
	 *   u8  count        -- cars with at least one connected driver
	 *   count × {
	 *       u32 car_id
	 *       wstring-B name         (u16 units + units × u16 UTF-16)
	 *       u8  current-driver idx
	 *   }
	 * Idle server: count=0, no per-entry block, 12-byte body.
	 */
	struct ByteBuf bb;
	uint8_t nc = 0;
	int j, rc;
	int ok;

	for (j = 0; j < ACC_MAX_CARS; j++)
		if (s->cars[j].used && s->cars[j].driver_count > 0)
			nc++;

	bb_init(&bb);
	ok = lobby_write_preamble(&bb, l, 0xd1) == 0
	    && wr_u8(&bb, nc) == 0;
	for (j = 0; ok && j < ACC_MAX_CARS; j++) {
		const struct CarEntry *c = &s->cars[j];
		const struct DriverInfo *dv;
		char name[ACC_MAX_NAME_LEN * 2 + 2];
		size_t nlen;

		if (!c->used || c->driver_count == 0)
			continue;
		dv = &c->drivers[c->current_driver_index %
		    ACC_MAX_DRIVERS_PER_CAR];
		snprintf(name, sizeof(name), "%s %s",
		    dv->first_name[0] ? dv->first_name : "Driver",
		    dv->last_name[0] ? dv->last_name : "");
		nlen = strlen(name);
		if (nlen > 0xFFFE)
			nlen = 0xFFFE;
		/*
		 * kson string = u16 utf8_byte_len + N UTF-8 bytes
		 * (FUN_14004d240 transcodes wchar→UTF-8 via
		 * FUN_14004cdd0 then writes the byte length as u16).
		 * Our internal names are already UTF-8 so we just
		 * emit length + bytes.  wr_str_b (which emits UTF-16
		 * units) is NOT the right format here — that was the
		 * first attempt and caused Kunos to drop us ~30 s
		 * after the first drivers=1 notify.
		 */
		ok = wr_u32(&bb, c->car_id) == 0
		    && wr_u16(&bb, (uint16_t)nlen) == 0
		    && bb_append(&bb, name, nlen) == 0
		    && wr_u8(&bb, c->current_driver_index) == 0;
	}
	if (!ok) {
		bb_free(&bb);
		return -1;
	}
	{
		size_t sz = bb.wpos;
		rc = lobby_send_framed(l, bb.data, bb.wpos);
		bb_free(&bb);
		if (rc == 0)
			log_info("lobby: drivers=%u (%zu B body)",
			    (unsigned)nc, sz);
	}
	return rc;
}

/*
 * Sample current session state into l->last_session_*.  Called right
 * before sending 0xcb so the wire reflects the live phase.  Kunos
 * code: FUN_1400482b0 reads sessionManager+0x268 (sessionType), calls
 * computeCurrentPhase, and takes time_remaining as a double in ms
 * then divides by DAT_14014bd20 (= 1000.0 — ms to seconds) and casts
 * to i16.  We use the same scaling from s->session.time_remaining_ms.
 */
static void
lobby_sample_session(struct LobbyClient *l, const struct Server *s)
{
	int32_t trem_ms = s->session.time_remaining_ms;
	int32_t trem_s;
	uint8_t stype = 0;

	if (s->session.session_index < s->session_count)
		stype = s->sessions[s->session.session_index].session_type;
	if (trem_ms < 0)
		trem_ms = 0;
	trem_s = trem_ms / 1000;
	if (trem_s > INT16_MAX)
		trem_s = INT16_MAX;
	l->last_session_type = stype;
	/*
	 * Lobby phase byte == Kunos computeCurrentPhase (FUN_14012e810)
	 * return value, 1..7:
	 *   1 WAITING, 2 FORMATION, 3 PRE-1, 4 PRE-2 (race formation
	 *   lap), 5 SESSION, 6 OVERTIME, 7 COMPLETED.
	 * This is NOT the SDK broadcasting SessionPhase that goes on
	 * the 0x28 client broadcast (session_phase_to_wire).  Previous
	 * attempt sent that SDK mapping to the lobby and the phase
	 * never matched reality — e.g. WAITING went out as 0, which
	 * Kunos treats as "no active session".
	 */
	{
		uint8_t p;
		switch (s->session.phase) {
		case PHASE_WAITING:     p = 1; break;
		case PHASE_FORMATION:   p = 2; break;
		case PHASE_PRE_SESSION: p = 3; break;
		case PHASE_SESSION:     p = 5; break;
		case PHASE_OVERTIME:    p = 6; break;
		case PHASE_COMPLETED:   p = 7; break;
		default:                p = 1; break;
		}
		l->last_session_phase = p;
	}
	l->last_session_time_s = (int16_t)trem_s;
}

static int
lobby_send_session_update(struct LobbyClient *l, const struct Server *s)
{
	/*
	 * Session update body: 11-byte preamble + u8 sessionType +
	 * u8 phase + i16 time_remaining_seconds + u8 0.  Total 16 bytes.
	 * Type 0xcb, reverse-engineered from FUN_1400482b0 (v1.10.2).
	 */
	struct ByteBuf bb;
	int rc;

	lobby_sample_session(l, s);
	bb_init(&bb);
	if (lobby_write_preamble(&bb, l, 0xcb) < 0 ||
	    wr_u8(&bb, l->last_session_type) < 0 ||
	    wr_u8(&bb, l->last_session_phase) < 0 ||
	    wr_u16(&bb, (uint16_t)l->last_session_time_s) < 0 ||
	    wr_u8(&bb, 0) < 0) {
		bb_free(&bb);
		return -1;
	}
	rc = lobby_send_framed(l, bb.data, bb.wpos);
	bb_free(&bb);
	if (rc == 0) {
		l->last_session_update_ms = lobby_now_ms();
		l->session_dirty = 0;
		log_info("lobby: Sent session update to lobby (type=%u "
		    "phase=%u trem=%ds)",
		    (unsigned)l->last_session_type,
		    (unsigned)l->last_session_phase,
		    (int)l->last_session_time_s);
	}
	return rc;
}

static int
lobby_send_keepalive(struct LobbyClient *l)
{
	/*
	 * Keepalive is preamble + u8 0 + u8 0x0d (msg_id at body[13]).
	 * Verified against Kunos: `0e 00 + ts + 0 + session + 0 0 0 0d`.
	 */
	struct ByteBuf bb;
	int rc;
	bb_init(&bb);
	/*
	 * Keepalive body: 11-byte preamble + u8 0 + u8 0 + u8 0x0d.
	 * Total body 14 bytes (Kunos verified).  The first two zeros
	 * are not the msg_id — they sit between preamble and the
	 * keepalive marker 0x0d.
	 */
	if (lobby_write_preamble(&bb, l, 0xf2) < 0 ||
	    wr_u8(&bb, 0) < 0 ||
	    wr_u8(&bb, 0) < 0 ||
	    wr_u8(&bb, LOBBY_MSG_KEEPALIVE) < 0) {
		bb_free(&bb);
		return -1;
	}
	rc = lobby_send_framed(l, bb.data, bb.wpos);
	bb_free(&bb);
	if (rc == 0)
		l->last_keepalive_ms = lobby_now_ms();
	return rc;
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
	if (l->state == LOBBY_PERMANENTLY_DISABLED)
		return;	/* sticky terminal state */
	l->consecutive_fails++;
	backoff = LOBBY_RETRY_MS;
	if (l->consecutive_fails > 3)
		backoff *= (uint32_t)(1 << (l->consecutive_fails - 3));
	if (backoff > LOBBY_BACKOFF_MAX_MS)
		backoff = LOBBY_BACKOFF_MAX_MS;
	(void)backoff;	/* state_entered_ms + LOBBY_RETRY_MS used in tick */
	lobby_set_state(l, LOBBY_BACKOFF);
}

static const char *
lobby_reject_reason(uint8_t code)
{
	switch (code) {
	case 0: return "accepted";
	case 1: return "outdated server";
	case 2: return "wrong version / wrong port";
	case 3: return "blocked by backend";
	case 4: return "rejected (unknown reason)";
	case 5: return "unsupported platform (Wine?)";
	case 6: return "did not respond on public IP";
	default: return "rejected (unmapped)";
	}
}

/*
 * Returns 1 if the response indicates a successful registration,
 * 0 if rejected (state machine should disable / report), -1 if
 * unrecognised (treat as transient).
 */
static int
lobby_parse_response(struct LobbyClient *l, const unsigned char *p, size_t n)
{
	(void)l;
	/*
	 * Registration response is `02 00 ef NN` (4 bytes).  NN is
	 * the case-byte read by FUN_140048660 in accServer.exe to
	 * map to a human error string. NN==0 means accepted.
	 * Keepalive ack is `01 00 fd` (3 bytes).
	 */
	if (n >= 4 && p[0] == 0x02 && p[1] == 0x00 && p[2] == 0xef) {
		uint8_t code = p[3];
		if (code == 0) {
			log_info("lobby: registration accepted");
			return 1;
		}
		log_warn("lobby: registration rejected code=%u (%s)",
		    (unsigned)code, lobby_reject_reason(code));
		return 0;
	}
	if (n >= 3 && p[0] == 0x01 && p[1] == 0x00 && p[2] == 0xfd) {
		/* keepalive ack */
		return -1;
	}
	log_debug("lobby: unrecognised response, %zu bytes", n);
	return -1;
}

void
lobby_handle_io(struct LobbyClient *l, struct Server *s, short revents)
{
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
		if (lobby_send_init_blob(l, (uint16_t)s->tcp_port) < 0 ||
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
			int rc = lobby_parse_response(l, buf, (size_t)n);
			if (l->state == LOBBY_REGISTERING) {
				if (rc == 1) {
					lobby_set_state(l, LOBBY_REGISTERED);
					l->consecutive_fails = 0;
					log_info("lobby: RegisterToLobby "
					    "succeeded");
					(void)lobby_send_drivers_update(l, s);
					/*
					 * Prime the lobby with our current
					 * phase so it doesn't show the
					 * initial "Practice" forever when we
					 * registered mid-session.
					 */
					(void)lobby_send_session_update(l, s);
				} else if (rc == 0) {
					/*
					 * Hard rejection — don't keep
					 * retrying every 10s, give up.
					 */
					log_warn("lobby: hard reject; "
					    "disabling lobby client");
					lobby_set_state(l,
					    LOBBY_PERMANENTLY_DISABLED);
				}
			}
		} else if (n == 0) {
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
			(void)lobby_send_drivers_update(l, s);
			l->drivers_dirty = 0;
		}
		/*
		 * 0xcb session update — sent only on phase transitions
		 * (session_dirty flag), NOT on a periodic timer.  Kunos
		 * itself does not push 0xcb on a timer in the sniffed
		 * idle window; emitting it every 20 s with a bad phase
		 * combo previously triggered a reset within ~110 s.
		 */
		if (l->session_dirty) {
			(void)lobby_send_session_update(l, s);
			l->session_dirty = 0;
		}
		if (now - l->last_keepalive_ms >= LOBBY_KEEPALIVE_MS)
			(void)lobby_send_keepalive(l);
		break;
	default:
		break;
	}
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
