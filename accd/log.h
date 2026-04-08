/*
 * log.h -- timestamped stderr logging.
 */

#ifndef ACCD_LOG_H
#define ACCD_LOG_H

#include <stddef.h>

void	log_info(const char *fmt, ...)
		__attribute__((format(printf, 1, 2)));
void	log_warn(const char *fmt, ...)
		__attribute__((format(printf, 1, 2)));
void	log_err(const char *fmt, ...)
		__attribute__((format(printf, 1, 2)));

void	log_hexdump(const char *prefix, const void *buf, size_t len);

#endif /* ACCD_LOG_H */
