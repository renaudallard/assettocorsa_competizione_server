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
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#ifdef __OpenBSD__
#include <unistd.h>
#endif

#include "config.h"
#include "dispatch.h"
#include "io.h"
#include "lan.h"
#include "log.h"
#include "msg.h"
#include "net.h"
#include "state.h"
#include "tick.h"

#define POLL_RECV_BUF	8192
#define POLL_SLOTS	(ACC_MAX_CARS + 4)
#define TICK_INTERVAL_MS	100

static volatile sig_atomic_t g_stop;

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
	socklen_t fromlen = sizeof(from);
	ssize_t n;

	n = recvfrom(s->udp_fd, buf, sizeof(buf), 0,
	    (struct sockaddr *)&from, &fromlen);
	if (n < 0) {
		if (errno != EINTR && errno != EAGAIN)
			log_warn("udp recvfrom: %s", strerror(errno));
		return;
	}
	dispatch_udp(s, &from, buf, (size_t)n);
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
	int npfds, i, r;
	const char *cfg_dir = "cfg";
	uint64_t last_tick_ms;

	if (argc > 1)
		cfg_dir = argv[1];

	setup_signals();
	server_init(&srv);

	if (config_load(&srv, cfg_dir) < 0) {
		log_err("config_load failed for %s", cfg_dir);
		return 1;
	}

	log_info("accd phase 1 starting (pid %d)", (int)getpid());
	log_info("config: tcp=%d udp=%d max=%d lan=%d track=\"%s\"",
	    srv.tcp_port, srv.udp_port, srv.max_connections,
	    srv.lan_discovery, srv.track);
	log_info("policy: registerToLobby forced 0 (private MP only)");

	srv.tcp_fd = tcp_listen(srv.tcp_port);
	srv.udp_fd = udp_bind(srv.udp_port);
	if (srv.tcp_fd < 0 || srv.udp_fd < 0)
		return 1;

	if (srv.lan_discovery)
		(void)lan_open(&srv.lan_fd);

#ifdef __OpenBSD__
	if (pledge("stdio inet", NULL) < 0)
		log_warn("pledge: %s", strerror(errno));
#endif

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
		npfds++;
		pfds[npfds].fd = srv.udp_fd;
		pfds[npfds].events = POLLIN;
		pfds[npfds].revents = 0;
		npfds++;
		if (srv.lan_fd >= 0) {
			pfds[npfds].fd = srv.lan_fd;
			pfds[npfds].events = POLLIN;
			pfds[npfds].revents = 0;
			npfds++;
		}
		for (slot = 0; slot < ACC_MAX_CARS && npfds < POLL_SLOTS;
		    slot++) {
			if (srv.conns[slot] == NULL)
				continue;
			pfds[npfds].fd = srv.conns[slot]->fd;
			pfds[npfds].events = POLLIN;
			pfds[npfds].revents = 0;
			npfds++;
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
		for (; i < npfds; i++) {
			struct Conn *c;
			int fd;
			int slot2;

			if (!(pfds[i].revents & (POLLIN | POLLHUP | POLLERR)))
				continue;
			fd = pfds[i].fd;
			c = NULL;
			for (slot2 = 0; slot2 < ACC_MAX_CARS; slot2++) {
				if (srv.conns[slot2] != NULL &&
				    srv.conns[slot2]->fd == fd) {
					c = srv.conns[slot2];
					break;
				}
			}
			if (c == NULL)
				continue;
			if (handle_tcp_client(&srv, c) < 0)
				conn_drop(&srv, c);
		}

		if (mono_ms() - last_tick_ms >= TICK_INTERVAL_MS) {
			tick_run(&srv);
			last_tick_ms = mono_ms();
		}
	}

	log_info("accd shutting down");
	if (srv.lan_fd >= 0)
		close(srv.lan_fd);
	close(srv.tcp_fd);
	close(srv.udp_fd);
	server_free(&srv);
	return 0;
}
