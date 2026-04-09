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
 * results.h -- session results JSON file writer.
 *
 * Writes results/YYMMDD_HHMMSS_<sessiontype>.json at session
 * end with the per-car standings, lap times, sector splits.
 * Schema follows §9 of NOTEBOOK_B.md.
 */

#ifndef ACCD_RESULTS_H
#define ACCD_RESULTS_H

#include "state.h"

/*
 * Write the results file for the just-finished session.  Returns
 * 0 on success, -1 on file open / write error.
 */
int	results_write(struct Server *s);

#endif /* ACCD_RESULTS_H */
