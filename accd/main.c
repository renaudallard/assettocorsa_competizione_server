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
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <poll.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#ifdef __OpenBSD__
/*
 * pledge(2) is an OpenBSD extension declared in <unistd.h>, but
 * only when _POSIX_C_SOURCE is not defined.  We want the strict
 * POSIX feature set on Linux so we forward-declare pledge here
 * to keep both platforms compiling cleanly.
 */
extern int pledge(const char *promises, const char *execpromises);
#endif

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
#include "state.h"
#include "tick.h"

#define POLL_RECV_BUF	8192
#define POLL_SLOTS	(ACC_MAX_CARS + 5)	/* tcp + udp + lan + stdin + conns */
#define TICK_INTERVAL_MS	3

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
		log_warn("server full: dropping %s:%u",
		    inet_ntoa(from.sin_addr), ntohs(from.sin_port));
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

static int
ms_until_next_tick(uint64_t last_tick_ms, uint64_t now_ms)
{
	uint64_t target = last_tick_ms + TICK_INTERVAL_MS;

	if (now_ms >= target)
		return 0;
	return (int)(target - now_ms);
}

static uint64_t
mono_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000ull +
	    (uint64_t)ts.tv_nsec / 1000000ull;
}

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
	uint64_t last_tick_ms;

	while ((ch = getopt(argc, argv, "dc:")) != -1) {
		switch (ch) {
		case 'd':
			g_debug = 1;
			break;
		case 'c':
			cfg_dir = optarg;
			break;
		default:
			fprintf(stderr,
			    "usage: accd [-d] [-c cfgdir]\n");
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
	ratings_load(&srv);

	log_info("accd phase 1 starting (pid %d)", (int)getpid());
	log_info("config: tcp=%d udp=%d max=%d lan=%d track=\"%s\"",
	    srv.tcp_port, srv.udp_port, srv.max_connections,
	    srv.lan_discovery, srv.track);
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
	 * Always open UDP 8999 for discovery.  The ACC client sends a
	 * discovery probe to this port before connecting via TCP, even
	 * for remote servers listed in serverList.json.
	 */
	(void)lan_open(&srv.lan_fd);

#ifdef __OpenBSD__
	if (pledge("stdio rpath wpath cpath inet", NULL) < 0)
		log_warn("pledge: %s", strerror(errno));
#endif

	console_init();

	log_info("listening: tcp/%d udp/%d (Ctrl-C to stop)",
	    srv.tcp_port, srv.udp_port);

	last_tick_ms = mono_ms();
	while (!g_stop) {
		int timeout_ms;
		int slot;

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

		timeout_ms = ms_until_next_tick(last_tick_ms, mono_ms());
		r = poll(pfds, (nfds_t)npfds, timeout_ms);
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
		 * iteration. */
		for (slot = 0; slot < ACC_MAX_CARS; slot++) {
			if (srv.conns[slot] != NULL &&
			    srv.conns[slot]->state == CONN_DISCONNECT)
				conn_drop(&srv, srv.conns[slot]);
		}

		if (mono_ms() - last_tick_ms >= TICK_INTERVAL_MS) {
			tick_run(&srv);
			last_tick_ms = mono_ms();
		}
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
