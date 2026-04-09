/*
 * Copyright (c) 2025-2026 Renaud Allard
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
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

int g_debug;

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
log_debug(const char *fmt, ...)
{
	va_list ap;

	if (!g_debug)
		return;
	va_start(ap, fmt);
	vlog_with_tag("DBG ", fmt, ap);
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
