/*
 * log.c -- timestamped stderr logging.
 */

#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "log.h"

static void
vlog_with_tag(const char *tag, const char *fmt, va_list ap)
{
	struct timespec ts;
	struct tm tm;
	char tbuf[32];

	clock_gettime(CLOCK_REALTIME, &ts);
	localtime_r(&ts.tv_sec, &tm);
	strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &tm);
	fprintf(stderr, "%s.%03ld %s ", tbuf,
	    (long)(ts.tv_nsec / 1000000), tag);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
}

void
log_info(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vlog_with_tag("INFO", fmt, ap);
	va_end(ap);
}

void
log_warn(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vlog_with_tag("WARN", fmt, ap);
	va_end(ap);
}

void
log_err(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vlog_with_tag("ERR ", fmt, ap);
	va_end(ap);
}

void
log_hexdump(const char *prefix, const void *buf, size_t len)
{
	const unsigned char *b = (const unsigned char *)buf;
	size_t i, j;

	for (i = 0; i < len; i += 16) {
		fprintf(stderr, "%s %04zx  ", prefix, i);
		for (j = 0; j < 16; j++) {
			if (i + j < len)
				fprintf(stderr, "%02x ", b[i + j]);
			else
				fprintf(stderr, "   ");
			if (j == 7)
				fputc(' ', stderr);
		}
		fputc(' ', stderr);
		for (j = 0; j < 16 && i + j < len; j++) {
			unsigned char c = b[i + j];
			fputc(isprint(c) ? c : '.', stderr);
		}
		fputc('\n', stderr);
	}
}
