/*
 * bcast.c -- broadcast helpers.
 */

#define _POSIX_C_SOURCE 200809L

#include <stddef.h>

#include "bcast.h"
#include "io.h"
#include "log.h"
#include "msg.h"
#include "state.h"

int
bcast_send_one(struct Conn *c, const void *body, size_t len)
{
	if (c == NULL || c->fd < 0)
		return -1;
	return tcp_send_framed(c->fd, body, len);
}

int
bcast_all(struct Server *s, const void *body, size_t len,
    uint16_t except_conn_id)
{
	int i, sent;

	sent = 0;
	for (i = 0; i < ACC_MAX_CARS; i++) {
		struct Conn *c = s->conns[i];

		if (c == NULL)
			continue;
		if (c->state != CONN_AUTH)
			continue;
		if (c->conn_id == except_conn_id)
			continue;
		if (bcast_send_one(c, body, len) == 0)
			sent++;
	}
	return sent;
}
