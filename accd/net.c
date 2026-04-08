/*
 * net.c -- TCP listen + UDP bind helpers.
 */

#define _POSIX_C_SOURCE 200809L

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

#include "log.h"
#include "net.h"

int
tcp_listen(int port)
{
	int fd, on = 1;
	struct sockaddr_in sa;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		log_err("socket(tcp): %s", strerror(errno));
		return -1;
	}
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0)
		log_warn("setsockopt SO_REUSEADDR: %s", strerror(errno));

	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_ANY);
	sa.sin_port = htons((uint16_t)port);
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		log_err("bind tcp :%d: %s", port, strerror(errno));
		close(fd);
		return -1;
	}
	if (listen(fd, 16) < 0) {
		log_err("listen tcp :%d: %s", port, strerror(errno));
		close(fd);
		return -1;
	}
	return fd;
}

int
udp_bind(int port)
{
	int fd;
	struct sockaddr_in sa;

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		log_err("socket(udp): %s", strerror(errno));
		return -1;
	}
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_ANY);
	sa.sin_port = htons((uint16_t)port);
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		log_err("bind udp :%d: %s", port, strerror(errno));
		close(fd);
		return -1;
	}
	return fd;
}
