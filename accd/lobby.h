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
 * lobby.h -- Kunos kson lobby client.
 *
 * Connects to the Kunos backend at 131.153.158.178:909 and
 * registers our server so that the public lobby lists it.
 * Active only when settings.json has "registerToLobby": 1.
 *
 * Wire format reverse-engineered from a captured Kunos
 * accServer.exe v1.10.2 session.  Each TCP message is u16 LE
 * length + body; the first message after TCP connect is a
 * 256-byte session-init blob, followed by the registration
 * message (id 0x44).  See lobby.c for details and unknowns.
 *
 * Threading: single-thread, integrated into the main poll loop.
 * lobby_poll_fd / lobby_handle_io / lobby_tick are called from
 * the main loop; no blocking syscalls.
 */

#ifndef ACCD_LOBBY_H
#define ACCD_LOBBY_H

#include <stdint.h>

struct Server;

enum lobby_state {
	LOBBY_DISABLED = 0,	/* registerToLobby == 0 */
	LOBBY_DISCONNECTED,	/* not connected, will retry */
	LOBBY_CONNECTING,	/* TCP connect() in progress */
	LOBBY_REGISTERING,	/* sent init+register, awaiting ack */
	LOBBY_REGISTERED,	/* steady state */
	LOBBY_BACKOFF,		/* failed, waiting for retry timer */
	LOBBY_PERMANENTLY_DISABLED, /* "outdated" / "blocked" / "rejected" */
};

struct LobbyClient {
	enum lobby_state state;
	int		fd;			/* TCP socket, -1 when none */
	uint64_t	state_entered_ms;	/* monotonic ms */
	uint64_t	last_keepalive_ms;
	uint32_t	session_id;		/* assigned by lobby (or 6) */
	uint32_t	seq;			/* monotonically increasing */
	int		consecutive_fails;
	int		drivers_dirty;		/* need to push driver count */
	int		session_dirty;		/* need to push session phase */
	uint8_t		last_driver_count;
	uint8_t		last_session_type;
	uint8_t		last_session_phase;
	int16_t		last_session_time_s;
	uint64_t	last_session_update_ms;
	char		token_a[65];	/* 64 alphanum + NUL */
	char		token_b[11];	/* 10 alphanum + NUL */
	unsigned char	*rx_buf;
	size_t		rx_len;
	size_t		rx_cap;
};

void	lobby_init(struct LobbyClient *l);
void	lobby_shutdown(struct LobbyClient *l);

/* Returns the fd to add to the poll set (or -1 if none). */
int	lobby_poll_fd(const struct LobbyClient *l);
/* Returns events of interest (POLLIN | maybe POLLOUT). */
short	lobby_poll_events(const struct LobbyClient *l);

/* Called on POLLIN/POLLOUT/POLLHUP from the main loop. */
void	lobby_handle_io(struct LobbyClient *l, struct Server *s, short revents);

/* Called once per main loop tick — drives the state machine. */
void	lobby_tick(struct LobbyClient *l, struct Server *s);

/* Notifications from the server core. */
void	lobby_notify_drivers_changed(struct LobbyClient *l, uint8_t count);
void	lobby_notify_session_changed(struct LobbyClient *l);
void	lobby_notify_lap(struct LobbyClient *l, uint16_t car_id,
		int32_t lap_ms);

#endif /* ACCD_LOBBY_H */
