/*
 * accd -- ACC dedicated server reimplementation (work in progress).
 *
 * Phase 0 skeleton.  Reads cfg/configuration.json (UTF-16 LE), binds
 * TCP and UDP listeners on the configured ports, and logs every byte
 * received with a hex dump.  No protocol interpretation yet.
 *
 * Portable to Linux and OpenBSD.  C99, libc only (iconv is in libc on
 * both platforms).
 *
 * See ../notebook-b/NOTEBOOK_B.md for the protocol specification this
 * code is working towards.
 */

#define _POSIX_C_SOURCE 200809L

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <err.h>
#include <iconv.h>
#include <fcntl.h>
#include <time.h>
#include <ctype.h>
#include <signal.h>

#define CFG_PATH        "cfg/configuration.json"
#define RECV_BUF_LEN    4096
#define MAX_CLIENTS     85      /* matches HB default maxConnections */
#define POLL_SLOTS      (MAX_CLIENTS + 2)

struct config {
    int udpPort;
    int tcpPort;
    int maxConnections;
    int lanDiscovery;
    int configVersion;
};

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
    sa.sa_flags = 0;        /* no SA_RESTART: let poll return on signal */
    if (sigaction(SIGINT, &sa, NULL) == -1)
        err(1, "sigaction SIGINT");
    if (sigaction(SIGTERM, &sa, NULL) == -1)
        err(1, "sigaction SIGTERM");

    sa.sa_handler = SIG_IGN;
    if (sigaction(SIGPIPE, &sa, NULL) == -1)
        err(1, "sigaction SIGPIPE");
}

/* ----- logging ----------------------------------------------------- */

static void
logmsg(const char *fmt, ...)
{
    struct timespec ts;
    struct tm tm;
    char tbuf[32];
    va_list ap;

    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm);
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &tm);
    fprintf(stderr, "%s.%03ld ", tbuf, (long)(ts.tv_nsec / 1000000));

    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    fputc('\n', stderr);
}

static void
hexdump(const char *prefix, const unsigned char *buf, size_t len)
{
    size_t i, j;

    for (i = 0; i < len; i += 16) {
        fprintf(stderr, "%s %04zx  ", prefix, i);
        for (j = 0; j < 16; j++) {
            if (i + j < len)
                fprintf(stderr, "%02x ", buf[i + j]);
            else
                fprintf(stderr, "   ");
            if (j == 7)
                fputc(' ', stderr);
        }
        fputc(' ', stderr);
        for (j = 0; j < 16 && i + j < len; j++) {
            unsigned char c = buf[i + j];
            fputc(isprint(c) ? c : '.', stderr);
        }
        fputc('\n', stderr);
    }
}

/* ----- file I/O ---------------------------------------------------- */

static char *
read_file(const char *path, size_t *outlen)
{
    int fd;
    struct stat st;
    char *buf;
    ssize_t n;
    size_t off = 0;

    fd = open(path, O_RDONLY);
    if (fd < 0)
        return NULL;
    if (fstat(fd, &st) < 0) {
        close(fd);
        return NULL;
    }
    if (st.st_size <= 0 || st.st_size > (off_t)(1 << 20)) {
        close(fd);
        errno = EFBIG;
        return NULL;
    }
    buf = malloc((size_t)st.st_size);
    if (buf == NULL) {
        close(fd);
        return NULL;
    }
    while (off < (size_t)st.st_size) {
        n = read(fd, buf + off, (size_t)st.st_size - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            free(buf);
            close(fd);
            return NULL;
        }
        if (n == 0)
            break;
        off += (size_t)n;
    }
    close(fd);
    *outlen = off;
    return buf;
}

/* ----- UTF-16 LE -> UTF-8 via iconv -------------------------------- */

static char *
utf16le_to_utf8(const char *in, size_t inlen, size_t *outlen)
{
    iconv_t cd;
    char *out, *outp;
    const char *inp;
    size_t inrem, outrem, outbufsz;

    /* skip BOM if present */
    if (inlen >= 2 &&
        (unsigned char)in[0] == 0xff &&
        (unsigned char)in[1] == 0xfe) {
        in += 2;
        inlen -= 2;
    }

    cd = iconv_open("UTF-8", "UTF-16LE");
    if (cd == (iconv_t)-1)
        return NULL;

    /* UTF-8 is at most 3 bytes per BMP code point, from 2 bytes UTF-16.
     * Worst case expansion is 1.5x, so 2x plus null is generous. */
    outbufsz = inlen * 2 + 1;
    out = malloc(outbufsz);
    if (out == NULL) {
        iconv_close(cd);
        return NULL;
    }

    inp = in;
    inrem = inlen;
    outp = out;
    outrem = outbufsz - 1;

    if (iconv(cd, (char **)&inp, &inrem, &outp, &outrem) == (size_t)-1) {
        free(out);
        iconv_close(cd);
        return NULL;
    }
    *outp = '\0';
    iconv_close(cd);
    *outlen = (size_t)(outp - out);
    return out;
}

/* ----- tiny JSON int extraction ------------------------------------
 *
 * Only what configuration.json needs: find an integer value in a flat
 * top-level object given its key name.  No nesting, no strings, no
 * arrays.  Matches patterns like:
 *
 *     "key" : 1234
 *     "key":1234
 *     "key"   :   -5
 *
 * Not a general JSON parser.  Good enough for the default configs.
 */

static int
json_find_int(const char *json, const char *key, int *out)
{
    const char *p;
    char needle[64];
    int n;

    n = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (n < 0 || (size_t)n >= sizeof(needle))
        return -1;

    p = strstr(json, needle);
    if (p == NULL)
        return -1;
    p += n;
    while (*p != '\0' &&
        (*p == ' ' || *p == '\t' || *p == '\n' ||
         *p == '\r' || *p == ':'))
        p++;
    if (*p == '\0')
        return -1;
    *out = (int)strtol(p, NULL, 10);
    return 0;
}

/* ----- config reading ---------------------------------------------- */

static int
config_read(const char *path, struct config *cfg)
{
    char *raw, *utf8;
    size_t rawlen, utf8len;

    /* defaults */
    cfg->udpPort = 9231;
    cfg->tcpPort = 9232;
    cfg->maxConnections = MAX_CLIENTS;
    cfg->lanDiscovery = 1;
    cfg->configVersion = 1;

    raw = read_file(path, &rawlen);
    if (raw == NULL) {
        warn("open %s", path);
        return -1;
    }
    utf8 = utf16le_to_utf8(raw, rawlen, &utf8len);
    free(raw);
    if (utf8 == NULL) {
        warnx("iconv UTF-16LE->UTF-8 failed for %s", path);
        return -1;
    }

    (void)json_find_int(utf8, "udpPort", &cfg->udpPort);
    (void)json_find_int(utf8, "tcpPort", &cfg->tcpPort);
    (void)json_find_int(utf8, "maxConnections", &cfg->maxConnections);
    (void)json_find_int(utf8, "lanDiscovery", &cfg->lanDiscovery);
    (void)json_find_int(utf8, "configVersion", &cfg->configVersion);
    /* registerToLobby is deliberately not read: always 0 by policy. */

    free(utf8);

    if (cfg->maxConnections < 1 || cfg->maxConnections > MAX_CLIENTS)
        cfg->maxConnections = MAX_CLIENTS;
    return 0;
}

/* ----- networking -------------------------------------------------- */

static int
tcp_listen(int port)
{
    int fd, on = 1;
    struct sockaddr_in sa;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        err(1, "socket(tcp)");
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0)
        err(1, "setsockopt SO_REUSEADDR");

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0)
        err(1, "bind tcp :%d", port);
    if (listen(fd, 16) < 0)
        err(1, "listen tcp :%d", port);

    return fd;
}

static int
udp_bind(int port)
{
    int fd;
    struct sockaddr_in sa;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        err(1, "socket(udp)");

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0)
        err(1, "bind udp :%d", port);

    return fd;
}

static void
handle_udp(int fd)
{
    unsigned char buf[RECV_BUF_LEN];
    struct sockaddr_in from;
    socklen_t fromlen = sizeof(from);
    ssize_t n;

    n = recvfrom(fd, buf, sizeof(buf), 0,
        (struct sockaddr *)&from, &fromlen);
    if (n < 0) {
        if (errno != EINTR && errno != EAGAIN)
            logmsg("udp recvfrom: %s", strerror(errno));
        return;
    }
    logmsg("UDP <- %s:%u  (%zd bytes)",
        inet_ntoa(from.sin_addr), ntohs(from.sin_port), n);
    hexdump("  ", buf, (size_t)n);
}

static void
handle_tcp_accept(int lfd, struct pollfd *pfds, int *npfds, int max)
{
    int cfd;
    struct sockaddr_in from;
    socklen_t fromlen = sizeof(from);

    cfd = accept(lfd, (struct sockaddr *)&from, &fromlen);
    if (cfd < 0) {
        if (errno != EINTR && errno != EAGAIN)
            logmsg("tcp accept: %s", strerror(errno));
        return;
    }
    if (*npfds >= max) {
        logmsg("TCP !! %s:%u  rejected (server full)",
            inet_ntoa(from.sin_addr), ntohs(from.sin_port));
        close(cfd);
        return;
    }
    logmsg("TCP ++ %s:%u  (fd %d)",
        inet_ntoa(from.sin_addr), ntohs(from.sin_port), cfd);
    pfds[*npfds].fd = cfd;
    pfds[*npfds].events = POLLIN;
    pfds[*npfds].revents = 0;
    (*npfds)++;
}

static int
handle_tcp_client(struct pollfd *pfd)
{
    unsigned char buf[RECV_BUF_LEN];
    ssize_t n;

    n = recv(pfd->fd, buf, sizeof(buf), 0);
    if (n < 0) {
        if (errno == EINTR || errno == EAGAIN)
            return 0;
        logmsg("TCP -- fd %d: %s", pfd->fd, strerror(errno));
        close(pfd->fd);
        return -1;
    }
    if (n == 0) {
        logmsg("TCP -- fd %d closed by peer", pfd->fd);
        close(pfd->fd);
        return -1;
    }
    logmsg("TCP <- fd %d  (%zd bytes)", pfd->fd, n);
    hexdump("  ", buf, (size_t)n);
    return 0;
}

/* ----- main loop --------------------------------------------------- */

int
main(void)
{
    struct config cfg;
    int tcpfd, udpfd;
    struct pollfd pfds[POLL_SLOTS];
    int npfds, i, r;

    setup_signals();

    if (config_read(CFG_PATH, &cfg) < 0)
        errx(1, "cannot read %s", CFG_PATH);

    logmsg("accd phase 0 starting (pid %d)", (int)getpid());
    logmsg("config: tcpPort=%d udpPort=%d maxConnections=%d"
        " lanDiscovery=%d configVersion=%d",
        cfg.tcpPort, cfg.udpPort, cfg.maxConnections,
        cfg.lanDiscovery, cfg.configVersion);
    logmsg("policy: registerToLobby forced to 0 (private MP only)");

    tcpfd = tcp_listen(cfg.tcpPort);
    udpfd = udp_bind(cfg.udpPort);

#ifdef __OpenBSD__
    if (pledge("stdio inet", NULL) == -1)
        err(1, "pledge");
#endif

    pfds[0].fd = tcpfd;
    pfds[0].events = POLLIN;
    pfds[0].revents = 0;
    pfds[1].fd = udpfd;
    pfds[1].events = POLLIN;
    pfds[1].revents = 0;
    npfds = 2;

    logmsg("listening: tcp/%d udp/%d (Ctrl-C to stop)",
        cfg.tcpPort, cfg.udpPort);

    while (!g_stop) {
        r = poll(pfds, (nfds_t)npfds, -1);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            logmsg("poll: %s", strerror(errno));
            break;
        }
        if (pfds[0].revents & POLLIN)
            handle_tcp_accept(tcpfd, pfds, &npfds, POLL_SLOTS);
        if (pfds[1].revents & POLLIN)
            handle_udp(udpfd);
        for (i = 2; i < npfds; ) {
            if (pfds[i].revents & (POLLIN | POLLHUP | POLLERR)) {
                if (handle_tcp_client(&pfds[i]) < 0) {
                    pfds[i] = pfds[npfds - 1];
                    npfds--;
                    continue;
                }
            }
            i++;
        }
    }

    logmsg("accd shutting down");
    for (i = 2; i < npfds; i++)
        close(pfds[i].fd);
    close(tcpfd);
    close(udpfd);
    return 0;
}
