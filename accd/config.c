/*
 * config.c -- configuration file readers.
 *
 * The default ACC server ships its configs as UTF-16 LE JSON.
 * We read each file, convert to UTF-8 with iconv, then run a
 * minimal extractor that pulls flat top-level int and string
 * values out of the result.  No nested objects or arrays in
 * phase 1 — the entry list / event rules are read by separate
 * helpers when we get to phase 3.
 */

#define _POSIX_C_SOURCE 200809L

#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>
#include <iconv.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "log.h"
#include "state.h"

#define CFG_MAX_SIZE	(1u << 20)

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
	if (st.st_size <= 0 || st.st_size > (off_t)CFG_MAX_SIZE) {
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

static char *
utf16le_to_utf8(const char *in, size_t inlen)
{
	iconv_t cd;
	char *out, *outp;
	const char *inp;
	size_t outbufsz, inrem, outrem;

	if (inlen >= 2 && (unsigned char)in[0] == 0xff &&
	    (unsigned char)in[1] == 0xfe) {
		in += 2;
		inlen -= 2;
	}

	cd = iconv_open("UTF-8", "UTF-16LE");
	if (cd == (iconv_t)-1)
		return NULL;
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
	return out;
}

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

static int
json_find_string(const char *json, const char *key, char *out, size_t outsz)
{
	const char *p, *q;
	char needle[64];
	int n;
	size_t copy;

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
	if (*p != '"')
		return -1;
	p++;
	q = strchr(p, '"');
	if (q == NULL)
		return -1;
	copy = (size_t)(q - p);
	if (copy >= outsz)
		copy = outsz - 1;
	memcpy(out, p, copy);
	out[copy] = '\0';
	return 0;
}

static char *
load_one(const char *cfg_dir, const char *name)
{
	char path[512];
	char *raw, *utf8;
	size_t rawlen;

	snprintf(path, sizeof(path), "%s/%s", cfg_dir, name);
	raw = read_file(path, &rawlen);
	if (raw == NULL) {
		log_warn("config: cannot read %s: %s", path, strerror(errno));
		return NULL;
	}
	utf8 = utf16le_to_utf8(raw, rawlen);
	free(raw);
	if (utf8 == NULL)
		log_warn("config: iconv failed for %s", path);
	return utf8;
}

int
config_load(struct Server *s, const char *cfg_dir)
{
	char *configuration, *settings, *event;

	/* defaults */
	s->tcp_port = 9232;
	s->udp_port = 9231;
	s->max_connections = ACC_MAX_CARS;
	s->lan_discovery = 1;
	s->password[0] = '\0';
	s->admin_password[0] = '\0';
	s->spectator_password[0] = '\0';
	snprintf(s->server_name, sizeof(s->server_name), "accd");
	snprintf(s->track, sizeof(s->track), "monza");

	configuration = load_one(cfg_dir, "configuration.json");
	if (configuration != NULL) {
		(void)json_find_int(configuration, "tcpPort", &s->tcp_port);
		(void)json_find_int(configuration, "udpPort", &s->udp_port);
		(void)json_find_int(configuration, "maxConnections",
		    &s->max_connections);
		(void)json_find_int(configuration, "lanDiscovery",
		    &s->lan_discovery);
		free(configuration);
	} else {
		return -1;
	}

	settings = load_one(cfg_dir, "settings.json");
	if (settings != NULL) {
		(void)json_find_string(settings, "serverName",
		    s->server_name, sizeof(s->server_name));
		(void)json_find_string(settings, "password",
		    s->password, sizeof(s->password));
		(void)json_find_string(settings, "adminPassword",
		    s->admin_password, sizeof(s->admin_password));
		(void)json_find_string(settings, "spectatorPassword",
		    s->spectator_password,
		    sizeof(s->spectator_password));
		(void)json_find_int(settings, "ignorePrematureDisconnects",
		    &s->ignore_premature_disconnects);
		(void)json_find_int(settings, "dumpLeaderboards",
		    &s->dump_leaderboards);
		free(settings);
	}

	event = load_one(cfg_dir, "event.json");
	if (event != NULL) {
		(void)json_find_string(event, "track",
		    s->track, sizeof(s->track));
		free(event);
	}

	if (s->max_connections < 1 || s->max_connections > ACC_MAX_CARS)
		s->max_connections = ACC_MAX_CARS;
	return 0;
}
