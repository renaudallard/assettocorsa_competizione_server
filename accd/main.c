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
 * accd -- ACC dedicated server reimplementation.
 *
 * Phase 1: TCP framing layer + handshake parser + handshake
 * response builder + module-level dispatch skeleton.  All other
 * cases are stubs that log and skip — see dispatch.c, chat.c,
 * tick.c.
 *
 * The protocol specification this code is working towards lives
 * in ../notebook-b/NOTEBOOK_B.md.
 *
 * Build: see Makefile (BSD or GNU make, cc with c99).
 * Portable to Linux and OpenBSD.
 */

#define _POSIX_C_SOURCE 200809L

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <poll.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include "bans.h"
#include "ratings.h"
#include "config.h"
#include "console.h"
#include "dispatch.h"
#include "bcast.h"
#include "io.h"
#include "lan.h"
#include "log.h"
#include "msg.h"
#include "net.h"
#include "sandbox.h"
#include "state.h"
#include "tick.h"

#define POLL_RECV_BUF	8192
#define POLL_SLOTS	(ACC_MAX_CARS + 5)	/* tcp + udp + lan + stdin + conns */
#define TICK_INTERVAL_US	3000	/* 333 Hz, matching exe CreateTimerQueueTimer(3) */
#define CONN_UNAUTH_TIMEOUT_MS	30000	/* reap silent TCP scanners */

volatile sig_atomic_t g_stop;

static void
on_signal(int sig)
{
	(void)sig;
	g_stop = 1;
}

static void
setup_signals(void)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	if (sigaction(SIGINT, &sa, NULL) < 0)
		log_err("sigaction SIGINT: %s", strerror(errno));
	if (sigaction(SIGTERM, &sa, NULL) < 0)
		log_err("sigaction SIGTERM: %s", strerror(errno));
	sa.sa_handler = SIG_IGN;
	if (sigaction(SIGPIPE, &sa, NULL) < 0)
		log_err("sigaction SIGPIPE: %s", strerror(errno));
#ifdef SIGTTIN
	if (sigaction(SIGTTIN, &sa, NULL) < 0)
		log_err("sigaction SIGTTIN: %s", strerror(errno));
#endif
}

/* ----- per-fd handlers ------------------------------------------- */

static void
handle_tcp_accept(struct Server *s)
{
	int cfd;
	struct sockaddr_in from;
	socklen_t fromlen = sizeof(from);
	struct Conn *c;

	cfd = accept(s->tcp_fd, (struct sockaddr *)&from, &fromlen);
	if (cfd < 0) {
		if (errno != EINTR && errno != EAGAIN)
			log_warn("accept: %s", strerror(errno));
		return;
	}
	{
		struct timeval tv;
		int yes = 1;
		int flags;
		tv.tv_sec = 5;
		tv.tv_usec = 0;
		(void)setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO,
		    &tv, sizeof(tv));
		/*
		 * Kunos sets TCP_NODELAY on every accepted fd (exe
		 * FUN_14004e360).  Without it our per-tick small writes
		 * (0x14 keepalives, 0x1e per-peer broadcasts) get coalesced
		 * into the 40 ms Nagle window, adding perceptible input
		 * latency in-car.
		 */
		(void)setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY,
		    &yes, sizeof(yes));
		/*
		 * Non-blocking so a slow / stuck client never stalls the
		 * main loop during a fan-out.  Partial writes and EAGAIN
		 * results are captured in c->tx and drained on POLLOUT.
		 */
		flags = fcntl(cfd, F_GETFL, 0);
		if (flags >= 0)
			(void)fcntl(cfd, F_SETFL, flags | O_NONBLOCK);
	}
	c = conn_new(s, cfd, &from);
	if (c == NULL) {
		/*
		 * Server full.  Mirror kunos's wire behaviour: send a
		 * 0x0c REJECT_FULL with (sub=0, a=current_count, b=max)
		 * so the client renders the "server full" dialog instead
		 * of "connection lost".  Body 14 B + 2 B length prefix.
		 */
		unsigned char reject[16];
		size_t i;
		uint32_t cur = (uint32_t)s->nconns;
		uint32_t cap = (uint32_t)s->max_connections;
		reject[0] = 14;
		reject[1] = 0;
		reject[2] = SRV_STATE_RECORD_0C;
		reject[3] = REJECT_FULL;
		reject[4] = reject[5] = reject[6] = reject[7] = 0;	/* sub */
		for (i = 0; i < 4; i++)
			reject[8 + i] = (unsigned char)(cur >> (i * 8));
		for (i = 0; i < 4; i++)
			reject[12 + i] = (unsigned char)(cap >> (i * 8));
		(void)send(cfd, reject, sizeof(reject), MSG_NOSIGNAL);
		log_warn("server full: dropping %s:%u (sent REJECT_FULL "
		    "current=%u max=%u)",
		    inet_ntoa(from.sin_addr), ntohs(from.sin_port),
		    (unsigned)cur, (unsigned)cap);
		close(cfd);
		return;
	}
	log_info("tcp accept: %s:%u -> conn=%u fd=%d",
	    inet_ntoa(from.sin_addr), ntohs(from.sin_port),
	    (unsigned)c->conn_id, cfd);
}

static int
handle_tcp_client(struct Server *s, struct Conn *c)
{
	unsigned char buf[POLL_RECV_BUF];
	ssize_t n;

	n = recv(c->fd, buf, sizeof(buf), 0);
	if (n < 0) {
		if (errno == EINTR || errno == EAGAIN)
			return 0;
		log_warn("recv conn=%u: %s",
		    (unsigned)c->conn_id, strerror(errno));
		return -1;
	}
	if (n == 0) {
		log_info("conn=%u closed by peer", (unsigned)c->conn_id);
		return -1;
	}
	if (bb_append(&c->rx, buf, (size_t)n) < 0) {
		log_warn("rx grow failed for conn=%u", (unsigned)c->conn_id);
		return -1;
	}
	return dispatch_tcp(s, c);
}

static void
handle_udp(struct Server *s)
{
	unsigned char buf[POLL_RECV_BUF];
	struct sockaddr_in from;
	ssize_t n;
	int drained = 0;

	/*
	 * Drain every queued datagram in this poll iteration, not
	 * just one.  Under real-race load every client sends 0x1e
	 * car updates at ~18 Hz plus 0x13/0x16 keepalives, so the
	 * UDP socket can accumulate dozens of packets between two
	 * polls; reading one and looping back adds a poll roundtrip
	 * per packet and pushes the fan-out latency up.  Cap at 256
	 * to avoid starving TCP and the tick during a UDP flood.
	 */
	for (;;) {
		socklen_t fromlen = sizeof(from);
		n = recvfrom(s->udp_fd, buf, sizeof(buf), 0,
		    (struct sockaddr *)&from, &fromlen);
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			if (errno == EINTR)
				continue;
			log_warn("udp recvfrom: %s", strerror(errno));
			break;
		}
		dispatch_udp(s, &from, buf, (size_t)n);
		if (++drained >= 256)
			break;
	}
}

/* ----- main loop ------------------------------------------------- */

int
main(int argc, char **argv)
{
	struct Server srv;
	struct pollfd pfds[POLL_SLOTS];
	/*
	 * Parallel owner array: pfd_owner[i] is the Conn* that owns
	 * pfds[i].fd when pfds[i] is a client socket, NULL otherwise
	 * (tcp_fd / udp_fd / lan / console / lobby slots).  Built in
	 * the same pass that populates pfds[] so the event-dispatch
	 * loop can resolve fd -> Conn* in O(1) instead of walking
	 * conns[] linearly per TCP event.
	 */
	struct Conn *pfd_owner[POLL_SLOTS];
	int npfds, i, r, ch;
	const char *cfg_dir = "cfg";
	uint64_t last_tick_us;

	while ((ch = getopt(argc, argv, "dc:V")) != -1) {
		switch (ch) {
		case 'd':
			g_debug = 1;
			break;
		case 'c':
			cfg_dir = optarg;
			break;
		case 'V':
#ifdef ACCD_VERSION_STR
			printf("accd %s\n", ACCD_VERSION_STR);
#else
			printf("accd (dev)\n");
#endif
			return 0;
		default:
			fprintf(stderr,
			    "usage: accd [-d] [-c cfgdir] | -V\n");
			return 1;
		}
	}
	if (optind < argc)
		cfg_dir = argv[optind];

	setup_signals();
	server_init(&srv);

	if (config_load(&srv, cfg_dir) < 0) {
		log_err("config_load failed for %s", cfg_dir);
		return 1;
	}
	snprintf(srv.cfg_dir, sizeof(srv.cfg_dir), "%s", cfg_dir);
	bans_init(&srv.bans);
	bans_load(&srv.bans, cfg_dir);
	bans_init(&srv.kicks);
	kicks_load(&srv.kicks, cfg_dir);
	ratings_load(&srv);

	log_info("accd %s starting (pid %d)",
#ifdef ACCD_VERSION_STR
	    ACCD_VERSION_STR,
#else
	    "(dev)",
#endif
	    (int)getpid());
	/*
	 * Kunos-format stdout banner.  accweb's logparser regex is
	 * `^Server starting with version (\d+)$`; version 256 (=0x100)
	 * is the wire protocol version, matching the value
	 * accServer.exe prints.
	 */
	log_kunos("Server starting with version 256");
	log_info("config: tcp=%d udp=%d max=%d lan=%d track=\"%s\"",
	    srv.tcp_port, srv.udp_port, srv.max_connections,
	    srv.lan_discovery, srv.track);
	log_kunos("Track %s was set and updated", srv.track);
	if (srv.stats_udp_port > 0)
		log_info("policy: statsUdpPort=%d (0xbe telemetry to "
		    "127.0.0.1)", srv.stats_udp_port);
	if (srv.register_to_lobby) {
		log_info("policy: registerToLobby=1 (will register with "
		    "Kunos kson backend)");
		srv.lobby.state = LOBBY_DISCONNECTED;
	} else {
		log_info("policy: registerToLobby=0 (private MP only)");
	}

	srv.tcp_fd = tcp_listen(srv.tcp_port);
	srv.udp_fd = udp_bind(srv.udp_port);
	if (srv.tcp_fd < 0 || srv.udp_fd < 0)
		return 1;

	/*
	 * Open UDP 8999 for discovery when configuration.json lanDiscovery
	 * is set (the default).  The ACC client probes this port before
	 * connecting via TCP, even for remote servers in serverList.json.
	 * The exe gates its responder the same way (ServerConfiguration
	 * cfg+0x23); with lanDiscovery=0, lan_fd stays -1 and every poll
	 * and close guard below skips the socket.
	 */
	if (srv.lan_discovery)
		(void)lan_open(&srv.lan_fd);

	sandbox_apply(srv.cfg_dir, "results");

	console_init();

	log_info("listening: tcp/%d udp/%d (Ctrl-C to stop)",
	    srv.tcp_port, srv.udp_port);

	last_tick_us = mono_us();
	while (!g_stop) {
		int slot;
		int idle;

		npfds = 0;
		pfds[npfds].fd = srv.tcp_fd;
		pfds[npfds].events = POLLIN;
		pfds[npfds].revents = 0;
		pfd_owner[npfds] = NULL;
		npfds++;
		pfds[npfds].fd = srv.udp_fd;
		pfds[npfds].events = POLLIN;
		pfds[npfds].revents = 0;
		pfd_owner[npfds] = NULL;
		npfds++;
		if (srv.lan_fd >= 0) {
			pfds[npfds].fd = srv.lan_fd;
			pfds[npfds].events = POLLIN;
			pfds[npfds].revents = 0;
			pfd_owner[npfds] = NULL;
			npfds++;
		}
		if (console_fd() >= 0) {
			pfds[npfds].fd = console_fd();
			pfds[npfds].events = POLLIN;
			pfds[npfds].revents = 0;
			pfd_owner[npfds] = NULL;
			npfds++;
		}
		for (slot = 0; slot < ACC_MAX_CARS && npfds < POLL_SLOTS;
		    slot++) {
			struct Conn *cn = srv.conns[slot];
			if (cn == NULL)
				continue;
			pfds[npfds].fd = cn->fd;
			pfds[npfds].events = POLLIN;
			/* Subscribe to POLLOUT only while we have bytes
			 * queued — otherwise poll would spin on write-
			 * ready every iteration. */
			if (cn->tx.wpos > cn->tx.rpos)
				pfds[npfds].events |= POLLOUT;
			pfds[npfds].revents = 0;
			pfd_owner[npfds] = cn;
			npfds++;
		}
		{
			int lfd = lobby_poll_fd(&srv.lobby);
			if (lfd >= 0 && npfds < POLL_SLOTS) {
				pfds[npfds].fd = lfd;
				pfds[npfds].events =
				    lobby_poll_events(&srv.lobby);
				pfds[npfds].revents = 0;
				pfd_owner[npfds] = NULL;
				npfds++;
			}
		}

		/*
		 * Non-blocking poll when a client is connected.  On
		 * OpenBSD poll() with timeout < 20 ms still waits ~20
		 * ms (HZ=100 kernel tick + rounding), which would pin
		 * our tick loop at 50 Hz instead of the exe's 333 Hz.
		 * We pick up whatever's ready now and spin on
		 * sched_yield() until the tick deadline — matches the
		 * exe's dedicated tick thread.
		 *
		 * With zero clients we have no 0x1e / 0x28 fan-out
		 * deadline to honour, so block in poll() for up to 100
		 * ms.  All tick work is already gated on wall-clock
		 * cadences (keepalive 1 s, leaderboard 75 s, etc.),
		 * lobby I/O runs through the same poll fd, and new
		 * TCP / LAN / console traffic still wakes us within
		 * the budget.  This drops idle CPU from ~100 % of one
		 * core to near zero without changing any emission
		 * cadence.
		 */
		idle = (srv.nconns == 0);
		r = poll(pfds, (nfds_t)npfds, idle ? 100 : 0);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			log_err("poll: %s", strerror(errno));
			break;
		}

		i = 0;
		if (pfds[i].revents & POLLIN)
			handle_tcp_accept(&srv);
		i++;
		if (pfds[i].revents & POLLIN)
			handle_udp(&srv);
		i++;
		if (srv.lan_fd >= 0) {
			if (pfds[i].revents & POLLIN)
				lan_handle(&srv, srv.lan_fd);
			i++;
		}
		if (console_fd() >= 0) {
			if (pfds[i].revents & (POLLIN | POLLHUP))
				console_handle(&srv);
			i++;
		}
		for (; i < npfds; i++) {
			struct Conn *c = pfd_owner[i];

			if (c == NULL) {
				/*
				 * Non-client slot.  The only non-client
				 * entry past the fixed tcp/udp/lan/console
				 * prefix is the lobby socket; dispatch it
				 * by fd match.  Events on any other slot
				 * here indicate a build-order bug — skip.
				 */
				if (pfds[i].fd == lobby_poll_fd(&srv.lobby) &&
				    (pfds[i].revents & (POLLIN | POLLOUT |
				    POLLHUP | POLLERR | POLLNVAL)))
					lobby_handle_io(&srv.lobby, &srv,
					    pfds[i].revents);
				continue;
			}
			if (!(pfds[i].revents & (POLLIN | POLLOUT |
			    POLLHUP | POLLERR)))
				continue;
			if (pfds[i].revents & POLLOUT) {
				int rc = conn_drain_tx(c);
				if (rc < 0) {
					conn_drop(&srv, c);
					continue;
				}
			}
			if ((pfds[i].revents &
			    (POLLIN | POLLHUP | POLLERR)) == 0)
				continue;
			if (c->state == CONN_DISCONNECT ||
			    handle_tcp_client(&srv, c) < 0)
				conn_drop(&srv, c);
		}
		lobby_tick(&srv.lobby, &srv);

		/* Sweep connections marked for disconnect (kick/ban
		 * or failed sends) that had no poll events this
		 * iteration, plus any TCP that accepted but never
		 * authenticated within CONN_UNAUTH_TIMEOUT_MS — port
		 * scanners occasionally hold the socket open forever,
		 * and the session-start gate treats any TCP conn as a
		 * driver present, pinning the phase machine. */
		for (slot = 0; slot < ACC_MAX_CARS; slot++) {
			struct Conn *cn = srv.conns[slot];

			if (cn == NULL)
				continue;
			if (cn->state == CONN_DISCONNECT) {
				conn_drop(&srv, cn);
				continue;
			}
			if (cn->state == CONN_UNAUTH &&
			    mono_ms() - cn->accepted_mono_ms >=
			    CONN_UNAUTH_TIMEOUT_MS) {
				log_info("conn=%u unauth idle timeout, "
				    "closing %s:%u",
				    (unsigned)cn->conn_id,
				    inet_ntoa(cn->peer.sin_addr),
				    ntohs(cn->peer.sin_port));
				conn_drop(&srv, cn);
				continue;
			}
			/*
			 * Force-drop an authenticated driver who has gone
			 * UDP-silent for more than 5s, mirroring the exe's
			 * ignorePrematureDisconnects=0 timer (FUN_14002f180):
			 * a live client streams pongs and car updates at
			 * >=1 Hz, so a 5s gap means it is gone.  When the
			 * setting is 1 the exe tolerates the silence and so
			 * do we.  Gated on last_udp_server_ms != 0 so a conn
			 * that has not opened its UDP channel yet, and an
			 * SMPR TCP-only monitor, are never reaped here.
			 */
			if (cn->state == CONN_AUTH && !cn->is_smpr &&
			    !srv.ignore_premature_disconnects &&
			    cn->last_udp_server_ms != 0 &&
			    (uint32_t)mono_ms() - cn->last_udp_server_ms >
			    5000) {
				log_kunos("Disconnecting connId %u after 5s "
				    "due to setting ignorePrematureDisconnects"
				    "=false", (unsigned)cn->conn_id);
				conn_drop(&srv, cn);
				continue;
			}
		}

		{
			uint64_t now_us = mono_us();
			if (now_us - last_tick_us >= TICK_INTERVAL_US) {
				uint64_t work_us;
				float frac;

				tick_run(&srv);
				/*
				 * Sample CPU load = tick_run work / tick interval
				 * for the 0x14 keepalive bytes 11/12 (avg/max CPU
				 * percent).  Measure only the tick body, not the
				 * poll() idle above, mirroring the exe's dedicated
				 * tick thread (FUN_14002e8d0 sample = (tickEnd -
				 * tickStart)/targetStep).
				 */
				work_us = mono_us() - now_us;
				frac = (float)work_us / (float)TICK_INTERVAL_US;
				srv.cpu_ring[srv.cpu_ring_head] = frac;
				srv.cpu_ring_head =
				    (uint8_t)((srv.cpu_ring_head + 1) % 41);
				if (srv.cpu_ring_count < 41)
					srv.cpu_ring_count++;
				last_tick_us = now_us;
			}
		}
		/*
		 * Yield briefly so other runnable processes can
		 * schedule.  sched_yield() returns in < 1 µs on
		 * OpenBSD so it doesn't break the 333 Hz deadline
		 * the way nanosleep/usleep/poll(timeout>0) would.
		 * Skipped when idle — we already blocked in poll()
		 * for up to 100 ms and burning CPU here would defeat
		 * the whole point.
		 */
		if (!idle)
			sched_yield();
	}

	log_info("accd shutting down");
	ratings_save(&srv);
	console_close();
	if (srv.lan_fd >= 0)
		close(srv.lan_fd);
	close(srv.tcp_fd);
	close(srv.udp_fd);
	server_free(&srv);
	return 0;
}
