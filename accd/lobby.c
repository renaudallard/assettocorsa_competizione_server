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
 * captured 2026-04-15 and re-validated against v0.3.37 + kunos pcap
 * 2026-05-12.  Endpoint 131.153.158.178:909, framed as u16 LE length
 * + body (except the 256-byte init blob which is unframed).
 *
 * After TCP connect, kunos writes a 256-byte session-init blob:
 *   bytes [0..1]   = local_port LE
 *   byte  [2]      = local_port % 77
 *   byte  [3]      = local_port % 21
 *   bytes [4..255] = uninitialised stack (kunos leaks 78 B of 64-bit
 *                    pointers under Wine; the lobby ignores them)
 * We send zeros for [4..255] which the lobby accepts cleanly.
 *
 * Registration body (preamble type=0xc8, msg id 0x44):
 *   u8  0x44          msg id (REGISTER)
 *   u8  0x01          server-kind = dedicated
 *   u8  0x2b          protocol minor
 *   u32 tcp_port
 *   u32 udp_port
 *   u16 + N bytes     serverName  (kson_string: u16 byte-length + UTF-8)
 *   u8  + N bytes     trackName   (u8 byte-length + UTF-8)
 *   u32 maxCarSlots   (rated capacity, NOT maxConnections)
 *   ff fa 01 00 00 01 00 00          <-- capability flags, verbatim
 *   u8  weatherRandomness
 *   u8  session_count
 *   per session (10 bytes):
 *      u8 type, u8 day_of_weekend, u8 hour, u8 duration_min,
 *      u8 0, u16 pre_race_wait_s, u16 overtime_s, u8 1
 *   u16 0             token pad
 *   u16 64 + 64 alnum  token_a (server fingerprint, random per launch)
 *   u16 10 + 10 alnum  token_b
 *
 * Periodic keepalive (every 30 s): u16 length 14 + 11 B preamble +
 * u8 load + u8 0 + u8 seq.  Server replies with `01 00 fd` (3 bytes).
 * Drivers update (on driver-count change): preamble(0xd1) + u8 count
 * + count * { u32 car_id, kson_string name, u8 current_driver_idx }.
 *
 * Source bytes for `ff fa 01 00 00 01 00 00` and the magic per-session
 * trailing 0x01 are still opaque — kunos hard-codes them in the
 * builder.  Tokens token_a/token_b are random alnum strings generated
 * at server boot (kunos uses time-seeded rand; we use arc4random).
 */

#define _POSIX_C_SOURCE 200809L

/*
 * arc4random_uniform lives in libc on both platforms but the prototype
 * is feature-gated.  On Linux (glibc) it's only exposed through libbsd's
 * <bsd/stdlib.h>.  On OpenBSD it sits behind __BSD_VISIBLE, which
 * _POSIX_C_SOURCE forces to 0 via sys/cdefs.h — so we declare the
 * prototype by hand.  The symbol is always present in libc so linking
 * works either way; this just silences the implicit-declaration warning.
 */
#ifdef __linux__
#include <bsd/stdlib.h>
#endif

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
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

#ifdef __OpenBSD__
/* See header comment: __BSD_VISIBLE is forced off by _POSIX_C_SOURCE
 * so <stdlib.h> hides arc4random_uniform even though libc has it. */
uint32_t arc4random_uniform(uint32_t);
#endif
#include <time.h>

#include "bcast.h"
#include "chat.h"
#include "io.h"
#include "log.h"
#include "lobby.h"
#include "msg.h"
#include "penalty.h"
#include "prim.h"
#include "session.h"
#include "state.h"

#define LOBBY_HOST_DEFAULT	"131.153.158.178"
#define LOBBY_PORT_DEFAULT	909
/*
 * Kunos backoff (FUN_140048660 case 2): 10 s while consecutive_fails
 * < 3, 30 s after, plus a uniform random 0..7 s jitter.  accd mirrors
 * the deterministic part; the random jitter is omitted for predictable
 * test behaviour (kunos's lobby tolerates either).
 */
#define LOBBY_RETRY_MS		10000
#define LOBBY_RETRY_LONG_MS	30000
#define LOBBY_BACKOFF_MAX_MS	300000
#define LOBBY_KEEPALIVE_MS	30000
#define LOBBY_SESSION_UPDATE_MS	20000	/* push at least every 20 s */
#define LOBBY_INIT_BLOB_SZ	256

/* msg ids observed on the wire (server -> lobby direction). */
#define LOBBY_MSG_REGISTER	0x44
#define LOBBY_MSG_DRIVERS	0x00
#define LOBBY_MSG_KEEPALIVE	0x0d


static void
lobby_random_token(char *out, size_t n)
{
	/*
	 * Vowel-free alphabet from accServer.exe FUN_1400449c0 line 26:
	 *   L"123456789BCDFGHJKLMNPQRSTVWXZbcdfghjklmnpqrstvwxz" (49 chars)
	 * No '0' (collides with 'O' visually) and no vowels (AEIOU/aeiou,
	 * to avoid accidentally generating words).  The lobby never
	 * validates the alphabet but matching kunos byte-for-byte keeps
	 * the wire identical to a clean-room kunos capture.
	 */
	static const char alpha[] =
	    "123456789BCDFGHJKLMNPQRSTVWXZbcdfghjklmnpqrstvwxz";
	size_t i;

	for (i = 0; i + 1 < n; i++)
		out[i] = alpha[arc4random_uniform(sizeof(alpha) - 1)];
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
	l->state_entered_ms = mono_ms();
}

void
lobby_init(struct LobbyClient *l)
{
	size_t i;

	memset(l, 0, sizeof(*l));
	l->fd = -1;
	l->session_id = 6;	/* observed value; lobby may reassign */
	lobby_random_token(l->token_a, sizeof(l->token_a));
	lobby_random_token(l->token_b, sizeof(l->token_b));
	/*
	 * 20-digit numeric fingerprint — kunos generates this at server
	 * boot by looping `rand() / 32767 * 10` twenty times (FUN_140023700
	 * line 858).  Sent back to the lobby in 0xd7 as field[0] so kson
	 * can detect a server restart and refresh its cached entry.
	 */
	for (i = 0; i + 1 < sizeof(l->server_fpr); i++)
		l->server_fpr[i] = '0' + arc4random_uniform(10);
	l->server_fpr[sizeof(l->server_fpr) - 1] = '\0';
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
	const char *host;
	int port;
	const char *e;

	/*
	 * Test affordance: ACCD_LOBBY_HOST / ACCD_LOBBY_PORT override
	 * the hardcoded kson endpoint.  Used by tests/integration/run_
	 * lobby_* scripts that spin up a fake kson server on localhost.
	 * Production callers should never set these.
	 */
	host = LOBBY_HOST_DEFAULT;
	port = LOBBY_PORT_DEFAULT;
	if ((e = getenv("ACCD_LOBBY_HOST")) != NULL && *e != '\0')
		host = e;
	if ((e = getenv("ACCD_LOBBY_PORT")) != NULL && *e != '\0') {
		int p = atoi(e);
		if (p > 0 && p < 65536)
			port = p;
	}

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
	sa.sin_port = htons((uint16_t)port);
	if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) {
		log_warn("lobby: bad host %s", host);
		close(fd);
		return -1;
	}
	rc = connect(fd, (struct sockaddr *)&sa, sizeof(sa));
	if (rc < 0 && errno != EINPROGRESS) {
		log_warn("lobby: connect %s:%d: %s", host, port,
		    strerror(errno));
		close(fd);
		return -1;
	}
	log_info("lobby: connecting to %s:%d (fd=%d)", host, port, fd);
	l->fd = fd;
	return 0;
}

static int
lobby_send_framed(struct LobbyClient *l, const void *body, size_t len)
{
	unsigned char hdr[2];
	const unsigned char *bptr = body;
	size_t hdr_off = 0, body_off = 0;

	if (len > 0xFFFF) {
		log_warn("lobby: oversize msg %zu bytes, dropped", len);
		return -1;
	}
	hdr[0] = (unsigned char)(len & 0xff);
	hdr[1] = (unsigned char)((len >> 8) & 0xff);

	/*
	 * Loop until the full hdr+body is written.  writev on a non-
	 * blocking socket can return short under TCP backpressure;
	 * the previous code took the partial write as success and
	 * corrupted the kson framing, since the next message would
	 * land mid-header from the receiver's perspective.
	 */
	while (hdr_off < 2 || body_off < len) {
		struct iovec iov[2];
		int n_iov = 0;
		ssize_t n;

		if (hdr_off < 2) {
			iov[n_iov].iov_base = hdr + hdr_off;
			iov[n_iov].iov_len = 2 - hdr_off;
			n_iov++;
		}
		if (body_off < len) {
			iov[n_iov].iov_base = (void *)(uintptr_t)
			    (bptr + body_off);
			iov[n_iov].iov_len = len - body_off;
			n_iov++;
		}
		n = writev(l->fd, iov, n_iov);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			log_warn("lobby: writev: %s", strerror(errno));
			return -1;
		}
		if (hdr_off < 2) {
			size_t take = (size_t)n < (2 - hdr_off)
			    ? (size_t)n : (2 - hdr_off);
			hdr_off += take;
			n -= (ssize_t)take;
		}
		if (n > 0)
			body_off += (size_t)n;
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
	size_t sent = 0;

	memset(buf, 0, sizeof(buf));
	buf[0] = (unsigned char)(tcp_port & 0xff);
	buf[1] = (unsigned char)((tcp_port >> 8) & 0xff);
	buf[2] = (unsigned char)(tcp_port % 77);
	buf[3] = (unsigned char)(tcp_port % 21);

	while (sent < sizeof(buf)) {
		ssize_t n = write(l->fd, buf + sent, sizeof(buf) - sent);
		if (n > 0) {
			sent += (size_t)n;
			continue;
		}
		if (n < 0 && (errno == EINTR ||
		    errno == EAGAIN || errno == EWOULDBLOCK)) {
			/* Non-blocking lobby fd: poll briefly and retry. */
			struct pollfd pfd = { l->fd, POLLOUT, 0 };
			(void)poll(&pfd, 1, 1000);
			continue;
		}
		log_warn("lobby: init write: %s",
		    n < 0 ? strerror(errno) : "short write");
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
	 * 13 config-dependent bytes (FUN_140047af0 decomp):
	 *   u8 maxCarSlots        — rated capacity (NOT max_connections)
	 *   u8 Server+0x178 low   — usually 0 in non-CP configs
	 *   u8 trackMedalsRequirement
	 *   u8 safetyRatingRequirement
	 *   u8 racecraftRatingRequirement (0xff = unset)
	 *   u8 carGroup enum      — FUN_140116480 (FFA=0xfa, GT2=0,
	 *                            GT3=7, GT4=0xc, GTC=0xb, TCX=0xf9)
	 *   u8 Server+0x228       — 1 = block joining during race
	 *   u8 Server+0x229       — default 0 (unverified)
	 *   u8 Server+0x231       — default 0 (unverified)
	 *   u8 wineFlag           — 1 if ntdll exports wine_get_version
	 *   u8 sessionVec[0]+0x164 — default 0
	 *   u8 selector(0x203/0x202) — non-CP default 0
	 *   u8 Server+0x310 low   — default 1 (semantic unverified)
	 * accd previously wrote `u32 max + 8 magic + u8 weather.randomness`
	 * which happens to produce identical bytes on wine+defaults but
	 * diverges for any non-default rating, carGroup or randomness.
	 */
	{
		uint8_t car_group_enum = 0xfa;	/* FreeForAll fallback */
		if (strcmp(s->car_group, "GT2") == 0)
			car_group_enum = 0x00;
		else if (strcmp(s->car_group, "GT3") == 0)
			car_group_enum = 0x07;
		else if (strcmp(s->car_group, "GT4") == 0)
			car_group_enum = 0x0c;
		else if (strcmp(s->car_group, "GTC") == 0)
			car_group_enum = 0x0b;
		else if (strcmp(s->car_group, "TCX") == 0)
			car_group_enum = 0xf9;

		if (wr_u8(&bb, (uint8_t)s->max_car_slots) < 0) goto err;
		if (wr_u8(&bb, 0) < 0) goto err;
		if (wr_u8(&bb, s->track_medals_required) < 0) goto err;
		if (wr_u8(&bb, s->safety_rating_required) < 0) goto err;
		if (wr_u8(&bb, s->racecraft_rating_required) < 0) goto err;
		if (wr_u8(&bb, car_group_enum) < 0) goto err;
		if (wr_u8(&bb, s->is_race_locked) < 0) goto err;
		if (wr_u8(&bb, 0) < 0) goto err;
		if (wr_u8(&bb, 0) < 0) goto err;
		if (wr_u8(&bb, 0) < 0) goto err;
		if (wr_u8(&bb, 0) < 0) goto err;
		if (wr_u8(&bb, 0) < 0) goto err;
		if (wr_u8(&bb, 1) < 0) goto err;
	}

	/*
	 * Session block: u8 sessionCount, then each 10-byte session:
	 * u8 type, u8 day, u8 hour, i16 duration_min, u16 pre_race_s,
	 * u16 overtime_s, u8 timeMultiplier (=1 for 1.0×).
	 */
	sess_count = s->session_count;
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
	 * After the session loop the exe emits:
	 *   u8 live_current_sessionType  (sessionMgr+0x268 low byte)
	 *   u8-pfx str  alternate-identifier (SC+0xd0, empty in stock
	 *               kunos — 1 byte of 0x00)
	 * then the two tokens.  accd previously emitted a u16(0) pad
	 * here, which produces identical wire bytes ONLY when the
	 * live session is Practice (sessionType=0) and the field is
	 * empty.  For any other session_index the high byte differs.
	 */
	{
		uint8_t live_type = 0;
		if (s->session.session_index < s->session_count)
			live_type = s->sessions[s->session.session_index]
			    .session_type;
		if (wr_u8(&bb, live_type) < 0) goto err;
		if (wr_u8(&bb, 0) < 0) goto err;
	}
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
	 *       kson_string name       (u16 utf8_byte_len + N UTF-8 bytes)
	 *       u8  current-driver idx
	 *   }
	 * Idle server: count=0, no per-entry block, 12-byte body.
	 *
	 * Exe gate at FUN_1400473f0 entry: accepts state == REGISTERING
	 * (5) or REGISTERED (6).  Mirror the check so a 0xf1 refresh
	 * request arriving early (before 0xef accept) doesn't writev
	 * into a closed or wrong-state socket.
	 */
	struct ByteBuf bb;
	uint8_t nc = 0;
	int j, rc;
	int ok;

	if (l->state != LOBBY_REGISTERING && l->state != LOBBY_REGISTERED)
		return -1;

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
		/*
		 * Kunos's own accServer.exe transitions observed in
		 * server.log always look like "<session> -> <waiting
		 * for drivers>" — the exe resets the session to WAITING
		 * as soon as the last driver leaves and so never emits
		 * the <session overtime> / <session completed> phases
		 * to the lobby under typical load.  Our reimpl DOES run
		 * the full phase machine (including OVERTIME and
		 * COMPLETED) even when nobody's connected, which surfaces
		 * a kson backend filter: once the lobby sees phase=6
		 * (OVERTIME) or phase=7 (COMPLETED) it delists the
		 * server and won't re-list it even after we loop back
		 * to phase=1.  Collapse the two transient end-of-session
		 * phases to the closest stable equivalent so we stay
		 * visible across session advance:
		 *   OVERTIME  -> 5 (SESSION, since OT is an extended
		 *                   session for in-flight laps)
		 *   COMPLETED -> 1 (WAITING, since we immediately
		 *                   advance to the next session and
		 *                   that next session starts at WAITING)
		 */
		switch (s->session.phase) {
		case PHASE_WAITING:     p = 1; break;
		case PHASE_FORMATION:   p = 2; break;
		case PHASE_PRE_SESSION:
			/*
			 * Exe's computeCurrentPhase splits this into PRE-1
			 * (phase=3) and PRE-2 (phase=4).  PRE-2 is the race
			 * grid-countdown / formation-lap window, gated on
			 * `time >= ts[2]`.  Our PHASE_PRE_SESSION covers
			 * both; emit 4 once we've crossed into the race
			 * formation window so the kson backend (and any
			 * scoreboard consumers) render the grid-countdown
			 * state.  Non-race sessions stay at 3.
			 */
			if (s->session.ts_valid &&
			    s->session.session_index < s->session_count &&
			    s->sessions[s->session.session_index]
				.session_type == 10 &&
			    mono_ms() >= s->session.ts[2])
				p = 4;
			else
				p = 3;
			break;
		case PHASE_SESSION:     p = 5; break;
		case PHASE_OVERTIME:    p = 5; break;
		case PHASE_COMPLETED:   p = 1; break;
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
		l->last_session_update_ms = mono_ms();
		l->session_dirty = 0;
		log_info("lobby: Sent session update to lobby (type=%u "
		    "phase=%u trem=%ds)",
		    (unsigned)l->last_session_type,
		    (unsigned)l->last_session_phase,
		    (int)l->last_session_time_s);
	}
	return rc;
}

/*
 * Reply to a kson 0xf6 CONFIG_REQUEST with 0xd7 containing three
 * kson_strings: the per-boot server fingerprint at ServerState+0x430
 * (FUN_140044c10 dispatch case 0xf6 reads it), then SC+0x1a8 and
 * SC+0x188.  The latter two are server-side strings whose semantic
 * we have not verified — empty strings keep the message size sane
 * and the lobby has never rejected the response.  kson uses 0xd7
 * to detect a server restart so it can drop a stale entry.
 */
static int
lobby_send_config_response(struct LobbyClient *l, const struct Server *s)
{
	struct ByteBuf bb;
	int rc;
	const char *fields[3];
	int i;

	(void)s;
	fields[0] = l->server_fpr;
	fields[1] = "";
	fields[2] = "";

	bb_init(&bb);
	if (lobby_write_preamble(&bb, l, 0xd7) < 0) {
		bb_free(&bb);
		return -1;
	}
	for (i = 0; i < 3; i++) {
		const char *str = fields[i] != NULL ? fields[i] : "";
		size_t slen = strlen(str);
		if (slen > 0xFFFE)
			slen = 0xFFFE;
		if (wr_u16(&bb, (uint16_t)slen) < 0 ||
		    bb_append(&bb, str, slen) < 0) {
			bb_free(&bb);
			return -1;
		}
	}
	rc = lobby_send_framed(l, bb.data, bb.wpos);
	bb_free(&bb);
	if (rc == 0)
		log_info("lobby: Sent 0xd7 config response (fpr=%s)",
		    l->server_fpr);
	return rc;
}

/*
 * Two kson outbound message types exist in the exe (FUN_140044c10
 * dispatch / FUN_140046f30 outbox enqueue) but are deliberately not
 * implemented here:
 *
 *   0xd2 WRECKER_REPORT  — emitted from the exe's collision tracker
 *                          when a driver accumulates enough contact
 *                          incidents to be flagged.  Wire format:
 *                          preamble(0xd2) + u16 car_id + 3 kson_string
 *                          (reporter_name, target_name, context).
 *                          accd has no collision-incident accumulator
 *                          so there is no internal trigger.
 *
 *   0xd3 CP_RACE_RESULT  — emitted at race end when the server is a
 *                          CP (Championship Points) event participant.
 *                          Wire: preamble(0xd3) + u32 0x07 + u32 0x08
 *                          + u32 0x07 + per-car record blob.
 *                          accd intentionally has no CP storage and
 *                          drops the inbound 0xf3 CP_PUSH after just
 *                          logging the event_id, so the CP write-back
 *                          channel never has data to emit.
 *
 * Adding senders without internal triggers would be dead code; adding
 * the triggers would be feature work outside the clean-room dedicated-
 * server scope.  The fields are documented here so a future operator
 * who wants wrecker reporting or CP integration has the wire format
 * available without re-RE'ing the exe.
 */

static int
lobby_send_keepalive(struct LobbyClient *l, const struct Server *s)
{
	/*
	 * Keepalive body (FUN_140048660 case 6 in accServer.exe):
	 *   preamble(0xf2)                                  11 bytes
	 *   u8 load  = (char)(int)(rainLevel * DAT_140150698)
	 *   u8 0
	 *   u8 seq   = (char)(int)(FUN_14011ee30(weather)
	 *               / DAT_14014f0d8 / _DAT_140150690)
	 *
	 * Constants extracted from the PE .rdata:
	 *   DAT_140150698 = 100.0
	 *   DAT_14014f0d8 = 60.0
	 *   _DAT_140150690 ≈ 1.66666 (5/3)
	 *   FUN_14011ee30  returns (int) of
	 *       fmod(weekend_time_s, 86400) * (1/60)
	 *     = minutes-into-the-in-game-day (0..1439).
	 *
	 * So `seq = (int)(min_of_day / 60 / (5/3)) = min_of_day / 100`
	 * — cycles 0..14 across a 24h in-session day.  Matches the
	 * exe byte-for-byte (modulo float rounding).
	 */
	struct ByteBuf bb;
	uint8_t load, seq;
	uint32_t min_of_day;
	int rc;

	load = (uint8_t)(s->weather.current_rain * 100.0f);
	min_of_day = (uint32_t)((s->session.weekend_time_s % 86400) / 60);
	seq = (uint8_t)(min_of_day / 100);

	bb_init(&bb);
	if (lobby_write_preamble(&bb, l, 0xf2) < 0 ||
	    wr_u8(&bb, load) < 0 ||
	    wr_u8(&bb, 0) < 0 ||
	    wr_u8(&bb, seq) < 0) {
		bb_free(&bb);
		return -1;
	}
	rc = lobby_send_framed(l, bb.data, bb.wpos);
	bb_free(&bb);
	if (rc == 0) {
		l->last_keepalive_ms = mono_ms();
		l->keepalive_ack_pending = 1;
		log_info("lobby: Sent keepalive (load=%u seq=%u)",
		    (unsigned)load, (unsigned)seq);
	}
	return rc;
}

static void
lobby_disconnect(struct LobbyClient *l, const char *reason)
{
	log_info("lobby: disconnecting (%s)", reason);
	if (l->fd >= 0) {
		close(l->fd);
		l->fd = -1;
	}
	free(l->rx_buf);
	l->rx_buf = NULL;
	l->rx_len = l->rx_cap = 0;
	l->keepalive_ack_pending = 0;
	l->last_keepalive_ms = 0;
	if (l->state == LOBBY_PERMANENTLY_DISABLED)
		return;	/* sticky terminal state */
	l->consecutive_fails++;
	l->backoff_ms = l->consecutive_fails < 3
	    ? LOBBY_RETRY_MS : LOBBY_RETRY_LONG_MS;
	if (l->backoff_ms > LOBBY_BACKOFF_MAX_MS)
		l->backoff_ms = LOBBY_BACKOFF_MAX_MS;
	lobby_set_state(l, LOBBY_BACKOFF);
}

/*
 * Parse a kson_string (u16 LE utf8_byte_len + N UTF-8 bytes) from
 * `*pos` within `body[len]`, copy into `out[outsz]` NUL-terminated,
 * and advance `*pos`.  Returns 0 on success, -1 if the frame would
 * overflow or the string is oversized.
 */
static int
lobby_read_kson_string(const unsigned char *body, size_t len, size_t *pos,
    char *out, size_t outsz)
{
	uint16_t slen;

	if (*pos + 2 > len)
		return -1;
	slen = (uint16_t)(body[*pos] | ((uint16_t)body[*pos + 1] << 8));
	*pos += 2;
	if (*pos + slen > len)
		return -1;
	if ((size_t)slen + 1 > outsz)
		return -1;
	memcpy(out, body + *pos, slen);
	out[slen] = '\0';
	*pos += slen;
	return 0;
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
 * Dispatch one framed message from the kson backend.  `body` points
 * at the first command byte, `len` is the body length (excluding the
 * u16 LE frame header already consumed by the caller).
 *
 * Returns 1 if this was a registration accept, 0 if it was a hard
 * reject, -1 otherwise (including unknown commands).
 */
static int
lobby_dispatch_message(struct LobbyClient *l, struct Server *s,
    const unsigned char *body, size_t len)
{
	uint8_t cmd;

	(void)s;
	if (len == 0)
		return -1;
	cmd = body[0];

	switch (cmd) {
	case 0xef:
		if (len >= 2) {
			uint8_t code = body[1];
			if (code == 0) {
				log_info("lobby: registration accepted");
				return 1;
			}
			log_warn("lobby: registration rejected code=%u (%s)",
			    (unsigned)code, lobby_reject_reason(code));
			return 0;
		}
		break;
	case 0xf1:
		/* DRIVERS_REFRESH — kson wants a fresh drivers list.
		 * Per FUN_140044c10 dispatch (case 0xf1) the exe just
		 * re-invokes its 0xd1 sender. */
		(void)lobby_send_drivers_update(l, s);
		break;
	case 0xf3: {
		/*
		 * CP (Championship Points) data push from kson.  Body:
		 *   kson_string event_id        (e.g. "monza")
		 *   u8  flag_a
		 *   u8  flag_b
		 *   kson_string event_qualifier (e.g. "E_6h")
		 *   ServerEventConfig blob (consumed by exe's
		 *      FUN_14012e4f0 — stored for CP stats integration)
		 * We're not a CP-enabled server, so just log the
		 * event identifier for operator visibility and drop
		 * the rest.  Avoids the "unhandled cmd 0xf3" debug
		 * spam without pulling in CP storage.
		 */
		char event_id[128];
		size_t p = 1;

		if (lobby_read_kson_string(body, len, &p, event_id,
		    sizeof(event_id)) == 0)
			log_info("lobby: 0xf3 CP data push for event \"%s\" "
			    "(%zu B body, dropped — CP not enabled)",
			    event_id, len);
		else
			log_info("lobby: 0xf3 CP data push (%zu B body, "
			    "short parse — dropped)", len);
		break;
	}
	case 0xf4: {
		/*
		 * Lobby-initiated remote ban.  kson sends two kson_strings
		 * (s1 = target steam_id, s2 = reason) + i32 + u8.  Per
		 * FUN_1400251b0 the exe looks up the car whose driver's
		 * steam_id matches s1, sends a 0x2b chat to ONLY that
		 * connection (sender "Server", type=3), and invokes
		 * FUN_140125f50 with exe_kind=8 (DQ) bucket=6 (RaceControl).
		 * If no match — no chat is sent (kunos drops it silently).
		 */
		char s1[256], s2[256];
		size_t p = 1;
		int j, target = -1;
		struct Conn *target_conn = NULL;

		if (lobby_read_kson_string(body, len, &p, s1, sizeof(s1)) < 0
		    || lobby_read_kson_string(body, len, &p, s2, sizeof(s2))
		    < 0) {
			log_warn("lobby: 0xf4 body parse failed (%zu B)", len);
			break;
		}
		for (j = 0; j < ACC_MAX_CARS; j++) {
			int d;
			if (!s->cars[j].used)
				continue;
			for (d = 0; d < s->cars[j].driver_count &&
			    d < ACC_MAX_DRIVERS_PER_CAR; d++) {
				if (strcmp(s->cars[j].drivers[d].steam_id,
				    s1) == 0) {
					target = j;
					break;
				}
			}
			if (target >= 0)
				break;
		}
		if (target < 0) {
			log_info("lobby: 0xf4 remote DQ, no car matched "
			    "steam_id=%s: %s", s1, s2);
			break;
		}
		for (j = 0; j < ACC_MAX_CARS; j++) {
			if (s->conns[j] != NULL && s->conns[j]->car_id ==
			    target) {
				target_conn = s->conns[j];
				break;
			}
		}
		log_info("lobby: 0xf4 remote DQ for car %d (%s): %s",
		    target, s1, s2);
		(void)penalty_enqueue(s, target, EXE_DQ, 19, 3, 1, 0,
		    REASON_RACE_CONTROL);
		if (target_conn != NULL) {
			struct ByteBuf out;
			bb_init(&out);
			if (wr_u8(&out, SRV_CHAT_OR_STATE) == 0 &&
			    wr_str_a(&out, "Server") == 0 &&
			    wr_str_a(&out, s2) == 0 &&
			    wr_i32(&out, 0) == 0 &&
			    wr_u8(&out, 3) == 0)
				(void)bcast_send_one(target_conn, out.data,
				    out.wpos);
			bb_free(&out);
		}
		break;
	}
	case 0xf5: {
		/*
		 * Lobby-wide broadcast announcement.  kson sends two
		 * kson_strings + i32 + u8.  Per FUN_140025470:
		 *   - s1 acts as a target selector (steam_id).  If non-
		 *     empty, the exe sends 0x2b to ONLY the matching
		 *     connection; if empty, it broadcasts.
		 *   - sender is "Server", chat-type is 3.
		 *   - the body text is s2 only.
		 * Used for Kunos-backend-initiated messages.
		 */
		char s1[256], s2[256];
		size_t p = 1;
		struct Conn *target_conn = NULL;
		struct ByteBuf out;
		int j;

		if (lobby_read_kson_string(body, len, &p, s1, sizeof(s1)) < 0
		    || lobby_read_kson_string(body, len, &p, s2, sizeof(s2))
		    < 0) {
			log_warn("lobby: 0xf5 body parse failed (%zu B)", len);
			break;
		}
		if (s1[0]) {
			for (j = 0; j < ACC_MAX_CARS; j++) {
				struct Conn *cc = s->conns[j];
				int d;
				if (cc == NULL || cc->car_id < 0 ||
				    cc->car_id >= ACC_MAX_CARS)
					continue;
				for (d = 0; d < s->cars[cc->car_id].
				    driver_count && d <
				    ACC_MAX_DRIVERS_PER_CAR; d++) {
					if (strcmp(s->cars[cc->car_id].
					    drivers[d].steam_id, s1) == 0) {
						target_conn = cc;
						break;
					}
				}
				if (target_conn != NULL)
					break;
			}
			if (target_conn == NULL) {
				log_info("lobby: 0xf5 selector \"%s\" matched "
				    "no connection — message dropped", s1);
				break;
			}
		}
		bb_init(&out);
		if (wr_u8(&out, SRV_CHAT_OR_STATE) != 0 ||
		    wr_str_a(&out, "Server") != 0 ||
		    wr_str_a(&out, s2) != 0 ||
		    wr_i32(&out, 0) != 0 ||
		    wr_u8(&out, 3) != 0) {
			bb_free(&out);
			break;
		}
		if (target_conn != NULL) {
			log_info("lobby: 0xf5 unicast to \"%s\": \"%s\"",
			    s1, s2);
			(void)bcast_send_one(target_conn, out.data, out.wpos);
		} else {
			log_info("lobby: 0xf5 broadcast \"%s\"", s2);
			(void)bcast_all(s, out.data, out.wpos,
			    BCAST_EXCEPT_NONE);
		}
		bb_free(&out);
		break;
	}
	case 0xf6:
		/* CONFIG_REQUEST — reply with 0xd7 containing
		 * server_name, track, password. */
		(void)lobby_send_config_response(l, s);
		break;
	case 0xfd:
		/* Keepalive ack — clears the pending flag.  Kunos's
		 * FUN_140048660 case 6 disconnects with "connection to
		 * lobby timed out" if ack_pending stays set for >30 s
		 * after a keepalive send. */
		l->keepalive_ack_pending = 0;
		break;
	default:
		log_debug("lobby: unhandled cmd 0x%02x (%zu B)",
		    (unsigned)cmd, len);
		break;
	}
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
		unsigned char tmp[4096];
		ssize_t n = read(l->fd, tmp, sizeof(tmp));
		if (n > 0) {
			size_t need, pos;

			/*
			 * Accumulate into a persistent buffer so partial
			 * frames from a TCP segment boundary survive
			 * across reads.  kson framing is `u16 LE len +
			 * body`; loop until we've drained every complete
			 * frame or hit a partial at the tail.
			 */
			need = l->rx_len + (size_t)n;
			/*
			 * kson max frame is u16 length + body = 65537 B;
			 * capping at 128 KiB leaves room for a partial
			 * second frame buffered behind the first while
			 * still failing cleanly against a misbehaving
			 * peer that streams partial frames forever.
			 */
			if (need > 128u * 1024u) {
				log_warn("lobby: rx buffer would exceed "
				    "128 KiB (need=%zu)", need);
				lobby_disconnect(l, "rx overflow");
				return;
			}
			if (need > l->rx_cap) {
				size_t new_cap = l->rx_cap
				    ? l->rx_cap : 4096;
				while (new_cap < need)
					new_cap *= 2;
				unsigned char *nb = realloc(l->rx_buf,
				    new_cap);
				if (nb == NULL) {
					log_warn("lobby: oom on rx buffer");
					lobby_disconnect(l, "oom");
					return;
				}
				l->rx_buf = nb;
				l->rx_cap = new_cap;
			}
			memcpy(l->rx_buf + l->rx_len, tmp, (size_t)n);
			l->rx_len += (size_t)n;

			pos = 0;
			while (pos + 2 <= l->rx_len) {
				uint16_t mlen = (uint16_t)(
				    l->rx_buf[pos] |
				    ((uint16_t)l->rx_buf[pos + 1] << 8));
				int rc;

				if (pos + 2 + mlen > l->rx_len)
					break;	/* partial — wait for more */
				rc = lobby_dispatch_message(l, s,
				    l->rx_buf + pos + 2, mlen);
				if (l->state == LOBBY_REGISTERING) {
					if (rc == 1) {
						lobby_set_state(l,
						    LOBBY_REGISTERED);
						l->consecutive_fails = 0;
						log_info("lobby: "
						    "RegisterToLobby "
						    "succeeded");
						/*
						 * Exe order on 0xef accept
						 * (FUN_140044c10): 0xcb
						 * session update first,
						 * then 0xd1 drivers.
						 */
						(void)lobby_send_session_update(
						    l, s);
						(void)lobby_send_drivers_update(
						    l, s);
					} else if (rc == 0) {
						log_warn("lobby: hard reject;"
						    " disabling lobby client");
						lobby_set_state(l,
						    LOBBY_PERMANENTLY_DISABLED);
					}
				}
				pos += 2 + mlen;
			}
			if (pos > 0) {
				memmove(l->rx_buf, l->rx_buf + pos,
				    l->rx_len - pos);
				l->rx_len -= pos;
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

	now = mono_ms();

	switch (l->state) {
	case LOBBY_DISCONNECTED:
		if (lobby_open_socket(l) == 0)
			lobby_set_state(l, LOBBY_CONNECTING);
		else
			lobby_set_state(l, LOBBY_BACKOFF);
		break;
	case LOBBY_BACKOFF:
		if (now - l->state_entered_ms >= l->backoff_ms)
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
		/* Mirror kunos's keepalive cycle (FUN_140048660 case 6):
		 *   - Send 0xf2 every 30 s if no ack is pending.
		 *   - If ack stays pending for >30 s after send, the
		 *     lobby has gone silent — drop and reconnect. */
		if (l->keepalive_ack_pending) {
			if (now - l->last_keepalive_ms > LOBBY_KEEPALIVE_MS) {
				lobby_disconnect(l, "keepalive ack timeout");
				return;
			}
		} else if (now - l->last_keepalive_ms >= LOBBY_KEEPALIVE_MS) {
			(void)lobby_send_keepalive(l, s);
		}
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
lobby_notify_lap(struct LobbyClient *l, uint16_t car_id,
    uint16_t race_number, int32_t lap_ms, int32_t race_time_ms)
{
	/*
	 * 0xd0 laptime-to-kson (FUN_1400477a0, called from
	 * FUN_1400142f0 case 0x21):
	 *   preamble(0xd0)
	 *   u16 car_id        (carId / grid index)
	 *   u16 race_number   (exe: `(short)uVar14` where uVar14 =
	 *                       FUN_140020630(carId, ...) — the
	 *                       visible race-number lookup)
	 *   u32 lap_time_ms
	 *   u32 race_time_ms  (iVar17 = FUN_140042000 = total race
	 *                       time from the lap record, normalized)
	 *
	 * The public lobby stats page credits the lap to the
	 * race_number and shows race_time_ms on its per-session
	 * view.  We used to send both as 0 — now both are correct.
	 */
	struct ByteBuf bb;
	int rc;

	if (l->state != LOBBY_REGISTERED)
		return;
	bb_init(&bb);
	if (lobby_write_preamble(&bb, l, 0xd0) < 0 ||
	    wr_u16(&bb, car_id) < 0 ||
	    wr_u16(&bb, race_number) < 0 ||
	    wr_u32(&bb, (uint32_t)lap_ms) < 0 ||
	    wr_u32(&bb, (uint32_t)race_time_ms) < 0) {
		bb_free(&bb);
		return;
	}
	rc = lobby_send_framed(l, bb.data, bb.wpos);
	bb_free(&bb);
	if (rc == 0)
		log_info("lobby: Sent laptime to kson: car %u #%u "
		    "lap %dms race_time %dms",
		    (unsigned)car_id, (unsigned)race_number,
		    (int)lap_ms, (int)race_time_ms);
}
