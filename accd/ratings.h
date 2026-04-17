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
 * ratings.h -- per-steam_id Safety / Trust rating store.
 *
 * Kunos ratings are computed server-side from racecraft signals and
 * persisted through Steam's backend.  As a clean-room reimpl we have
 * no Steam integration; instead we maintain a local EWMA on lap
 * completion signals (clean lap / cut / out-lap) and persist to a
 * JSON file keyed by steam_id so ratings survive across sessions
 * for the same driver on this server.
 *
 * Values are stored ×100 to match the 0x4e wire encoding (5000 =
 * 50.00, 9999 = 99.99).  Neutral starting value is 5000.
 */
#ifndef ACCD_RATINGS_H
#define ACCD_RATINGS_H

#include <stddef.h>
#include <stdint.h>

struct Server;

/*
 * Load the persisted ratings file if present.  Safe to call more
 * than once.  Does nothing if the file is missing.
 */
void	ratings_load(struct Server *s);

/*
 * Persist the in-memory ratings to disk.  Called at session end
 * and on server shutdown.
 */
void	ratings_save(struct Server *s);

/*
 * Look up the ratings for a steam_id.  Fills *sa and *tr (×100)
 * with the stored value, or 5000 default if the driver is new.
 */
void	ratings_get(const struct Server *s, const char *steam_id,
	    uint16_t *sa, uint16_t *tr);

/*
 * Update a driver's ratings based on the outcome of a completed lap.
 *     clean=1 / has_cut=0 / out_lap=0 : SA +5 (capped 9999)
 *     has_cut=1                        : SA -25 (floor 0)
 *     out_lap=1                        : SA unchanged (out-laps
 *                                        are pace-neutral)
 * TR is held constant in this simplified model.
 * Sets the "dirty" flag so the next periodic broadcast emits 0x4e.
 */
void	ratings_on_lap(struct Server *s, const char *steam_id,
	    int has_cut, int is_out_lap);

/*
 * Returns non-zero if any driver's rating has changed since the last
 * call to ratings_clear_dirty().  Used by the periodic 0x4e emit.
 */
int	ratings_is_dirty(const struct Server *s);
void	ratings_clear_dirty(struct Server *s);

#endif /* ACCD_RATINGS_H */
