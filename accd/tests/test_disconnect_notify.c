#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>
#include "state.h"
#include "msg.h"
volatile sig_atomic_t g_stop = 0;

static void check(int authenticated, int disconnect_state, int should_notify)
{
    struct Server *s = calloc(1, sizeof(*s));
    struct sockaddr_in peer = {0};
    struct Conn *watcher, *leaving;
    unsigned char msg[5];
    int pair[2];
    assert(s != NULL);
    server_init(s);
    s->max_connections = 4;
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    assert(fcntl(pair[1], F_SETFL, O_NONBLOCK) == 0);
    watcher = conn_new(s, pair[0], &peer);
    assert(watcher != NULL);
    watcher->state = CONN_AUTH;
    leaving = conn_new(s, -1, &peer);
    assert(leaving != NULL);
    leaving->state = disconnect_state ? CONN_DISCONNECT : CONN_AUTH;
    leaving->authenticated_once = authenticated;
    leaving->car_id = 0;
    s->cars[0].used = 1;
    s->cars[0].car_id = ACC_CAR_ID_BASE;
    conn_drop(s, leaving);
    ssize_t received = read(pair[1], msg, sizeof(msg));
    if (should_notify) {
        assert(received == 5);
        assert(msg[0] == 3 && msg[2] == SRV_CAR_DISCONNECT_NOTIFY);
    } else {
        assert(received == -1 && (errno == EAGAIN || errno == EWOULDBLOCK));
    }
    conn_drop(s, watcher);
    close(pair[1]);
    free(s);
}
int main(void)
{
    check(0, 0, 1); /* still authenticated */
    check(1, 1, 1); /* kicked or failed send */
    check(0, 1, 0); /* failed before authentication */
    puts("PASS: disconnect notices survive teardown without notifying for rejected joins");
    return 0;
}
