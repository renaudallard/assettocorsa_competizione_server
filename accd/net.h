/*
 * net.h -- low-level socket helpers.
 */

#ifndef ACCD_NET_H
#define ACCD_NET_H

int	tcp_listen(int port);
int	udp_bind(int port);

#endif /* ACCD_NET_H */
