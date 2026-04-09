/*
 * Copyright (c) 2025-2026 Renaud Allard
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * config.h -- configuration file readers.
 *
 * Reads cfg/configuration.json + cfg/settings.json + cfg/event.json
 * (UTF-16 LE) into the Server struct.  See HB §III for the field
 * meanings.
 */

#ifndef ACCD_CONFIG_H
#define ACCD_CONFIG_H

#include "state.h"

/*
 * Read every JSON file under cfg_dir and populate s.
 * Returns 0 on success, -1 if a required file is missing.
 */
int	config_load(struct Server *s, const char *cfg_dir);

#endif /* ACCD_CONFIG_H */
