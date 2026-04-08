/*
 * lan.c -- LAN discovery (UDP 8999).
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
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0) {
		log_warn("lan: SO_REUSEADDR: %s", strerror(errno));
	}
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

static void
build_response(struct ByteBuf *bb, struct Server *s)
{
	int i, used;

	(void)wr_u8(bb, ACP_LAN_RESPONSE);
	(void)wr_str_a(bb, s->server_name);
	(void)wr_u32(bb, (uint32_t)s->tcp_port);
	(void)wr_u32(bb, (uint32_t)s->udp_port);
	(void)wr_u8(bb, (uint8_t)s->max_connections);

	used = 0;
	for (i = 0; i < ACC_MAX_CARS && i < s->max_connections; i++)
		if (s->cars[i].used)
			used++;
	(void)wr_u8(bb, (uint8_t)used);

	for (i = 0; i < ACC_MAX_CARS && i < s->max_connections; i++) {
		if (!s->cars[i].used)
			continue;
		(void)wr_u16(bb, s->cars[i].car_id);
		(void)wr_u8(bb, s->cars[i].car_model);
	}
}

void
lan_handle(struct Server *s, int fd)
{
	unsigned char buf[LAN_RECV_BUF];
	struct sockaddr_in from;
	socklen_t fromlen = sizeof(from);
	ssize_t n;
	struct ByteBuf reply;

	n = recvfrom(fd, buf, sizeof(buf), 0,
	    (struct sockaddr *)&from, &fromlen);
	if (n < 0) {
		if (errno != EINTR && errno != EAGAIN)
			log_warn("lan recvfrom: %s", strerror(errno));
		return;
	}
	if (n < 1)
		return;
	if (buf[0] != ACP_LAN_DISCOVER) {
		log_warn("lan: unexpected msg 0x%02x from %s:%u",
		    buf[0], inet_ntoa(from.sin_addr),
		    ntohs(from.sin_port));
		return;
	}
	log_info("lan: discovery probe from %s:%u",
	    inet_ntoa(from.sin_addr), ntohs(from.sin_port));

	bb_init(&reply);
	build_response(&reply, s);
	if (sendto(fd, reply.data, reply.wpos, 0,
	    (struct sockaddr *)&from, fromlen) < 0) {
		log_warn("lan sendto: %s", strerror(errno));
	}
	bb_free(&reply);
}
