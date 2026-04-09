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
 * entrylist.h -- entrylist.json reader.
 *
 * Populates Server.cars[] with CarEntry records and DriverInfo
 * substructures from entrylist.json (UTF-16 LE per Kunos
 * convention).  Schema follows HB §III.4.
 */

#ifndef ACCD_ENTRYLIST_H
#define ACCD_ENTRYLIST_H

#include "state.h"

/*
 * Read cfg_dir/entrylist.json and populate s->cars[].  Sets
 * the `used` flag on each car that has an entry.  Returns the
 * number of cars loaded, or -1 on error.
 */
int	entrylist_load(struct Server *s, const char *cfg_dir);

#endif /* ACCD_ENTRYLIST_H */
