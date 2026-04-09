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
 * session.h -- session phase machine, leaderboard, race start.
 *
 * Drives the per-session lifecycle (P -> Q -> R -> Results),
 * tracks per-car race state, computes the standings, and emits
 * the broadcasts that fire at phase boundaries (0x3f grid
 * positions at race start, 0x3e session results at session end).
 */

#ifndef ACCD_SESSION_H
#define ACCD_SESSION_H

#include <stdint.h>

#include "state.h"

/*
 * Reset the session state to the start of session_index.
 * Clears per-car race state, sets phase to PHASE_PRE_SESSION,
 * resets the standings sequence number.
 */
void	session_reset(struct Server *s, uint8_t session_index);

/*
 * Advance the session machine one tick.  Called from tick.c
 * once per tick.  Handles phase transitions, fires the
 * one-shot broadcasts at boundaries.
 */
void	session_tick(struct Server *s);

/*
 * Advance to the next configured session immediately, used
 * by the /next admin command.
 */
void	session_advance(struct Server *s);

/*
 * Recompute standings for every used car based on the current
 * session phase (race vs P/Q ordering).  Bumps standings_seq
 * if anything changed.
 */
void	session_recompute_standings(struct Server *s);

/*
 * Returns 1 if the session phase requires lap-time-based sort
 * (P, Q), 0 if race-position-based (R).
 */
int	session_is_practice_or_qualy(const struct Server *s);

/*
 * Human-readable name for a session phase enum value.
 */
const char *
	session_phase_name(uint8_t phase);

#endif /* ACCD_SESSION_H */
