/*
 * dispatch.h -- per-message dispatch on TCP and UDP.
 *
 * The TCP dispatcher pulls framed messages out of a connection's
 * rx buffer and dispatches by msg id.  The UDP dispatcher takes a
 * single datagram.
 */

#ifndef ACCD_DISPATCH_H
#define ACCD_DISPATCH_H

#include <stddef.h>
#include <netinet/in.h>

#include "state.h"

/*
 * Drain the rx buffer of c, dispatching every complete framed
 * message.  Returns 0 on success, -1 if the connection should be
 * dropped.
 */
int	dispatch_tcp(struct Server *s, struct Conn *c);

/*
 * Process one UDP datagram from peer.
 */
void	dispatch_udp(struct Server *s, const struct sockaddr_in *peer,
		const unsigned char *buf, size_t len);

#endif /* ACCD_DISPATCH_H */
