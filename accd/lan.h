/*
 * lan.h -- LAN discovery on the fixed UDP port 8999.
 *
 * The protocol:
 *   client -> server   ACP_LAN_DISCOVER (0x48)
 *   server -> client   ACP_LAN_RESPONSE (0xc0) carrying the
 *                       server name, capacity, current car count,
 *                       and a per-car summary.
 */

#ifndef ACCD_LAN_H
#define ACCD_LAN_H

#include <netinet/in.h>

#include "state.h"

#define ACC_LAN_DISCOVERY_PORT	8999

int	lan_open(int *out_fd);
void	lan_handle(struct Server *s, int fd);

#endif /* ACCD_LAN_H */
