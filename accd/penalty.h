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
