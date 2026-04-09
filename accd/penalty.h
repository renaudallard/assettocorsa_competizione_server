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
 * penalty.h -- penalty queue per car.
 *
 * Wraps the PenaltyQueue struct in state.h with helpers for
 * enqueueing, serving, clearing, and ticking.  Used by both
 * the chat command handlers (admin penalty assignment) and the
 * auto-penalty system in phase 9.
 */

#ifndef ACCD_PENALTY_H
#define ACCD_PENALTY_H

#include <stdint.h>

#include "state.h"

/* Convert a chat command suffix (e.g. "tp5", "tp5c") to enum. */
int	penalty_kind_from_string(const char *cmd);

/* Enqueue a penalty for the given car.  Returns 0 on success
 * or -1 if the queue is full / car invalid. */
int	penalty_enqueue(struct Server *s, int car_id,
		uint8_t kind, int collision);

/* Mark the front penalty as served (for stop-and-go / drive-through). */
void	penalty_serve_front(struct Server *s, int car_id);

/* Clear all pending penalties for car_id (for /clear). */
void	penalty_clear(struct Server *s, int car_id);

/* Clear all pending penalties for every car (for /clear_all). */
void	penalty_clear_all(struct Server *s);

/* Return a human-readable name for a penalty kind. */
const char *
	penalty_name(uint8_t kind);

/* Build the chat string the binary uses for a penalty issuance. */
int	penalty_format_chat(char *out, size_t outsz,
		uint8_t kind, int collision, int car_num);

#endif /* ACCD_PENALTY_H */
