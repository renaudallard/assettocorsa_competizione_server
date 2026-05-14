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
 * Clears per-car race state, sets phase to PHASE_WAITING,
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
 * Populate the 7 schedule timestamps and mark ts_valid.  Called
 * by the tick on the first driver connect, and by handshake.c
 * right before the welcome-time 0x28 so its per-session records
 * are already valid — matches Kunos.
 */
void	session_start(struct Server *s);

/*
 * Advance to the next configured session immediately, used
 * by the /next admin command.
 */
void	session_advance(struct Server *s);

/*
 * Recompute standings for every used car based on the current
 * session phase (race vs P/Q ordering).  Writes the new ordering
 * to car.race.position; the leaderboard broadcast picks the change
 * up via deep-compare on the next tick.
 */
void	session_recompute_standings(struct Server *s);

/*
 * Driver-stint tracker (FUN_14012ae10 equivalent).
 *   start_tracking   = called when the car moves from non-track to on-track;
 *                      records the timestamp to accumulate against.
 *   stop_tracking    = called on pit entry or driver swap; flushes the
 *                      elapsed delta into driver_stint_ms[current_driver].
 *   check_violations = called at session end; enqueues DQ with
 *                      REASON_EXCEEDED_DRIVER_STINT_LIMIT on any driver
 *                      whose accumulated stint exceeds driver_stint_time_s.
 */
void	stint_start_tracking(struct Server *s, int car_id);
void	stint_stop_tracking(struct Server *s, int car_id);
void	stint_check_violations(struct Server *s);

/*
 * Snapshot the current session's race state into each used car's
 * race_archive[session_index] slot.  Called once when the session
 * transitions to PHASE_COMPLETED so the 0x56 garage reply can
 * serve laps from a past session after we've moved on.
 */
void	session_archive_snapshot(struct Server *s);

/*
 * Free every car's race_archive[] slots — used on a full weekend
 * reset (session_reset with index 0) and on server teardown.
 */
void	session_archive_clear(struct Server *s);

/*
 * Returns 1 if the session phase requires lap-time-based sort
 * (P, Q), 0 if race-position-based (R).
 */
int	session_is_practice_or_qualy(const struct Server *s);

/*
 * Map internal phase enum to the Broadcasting SDK SessionPhase
 * value expected by the client on the wire.
 */
uint8_t	session_phase_to_wire(uint8_t p);

/*
 * Called from the lap completion handler during overtime.
 * Decrements the cars-still-racing counter; releases the
 * overtime hold when all cars have finished.
 */
void	session_overtime_car_finished(struct Server *s);

/*
 * Quali "Right to Finish" / "Instant Drop": clear a car's
 * eligibility-to-finish flag and decrement the hold counter.
 * No-op outside Quali OVERTIME.  Call from the lap-invalidation
 * path (h_out_of_track) and from the lap-completion path when
 * the closing car was eligible.
 */
void	session_quali_drop_eligibility(struct Server *s, int car_id);

/*
 * Race formation-/green-flag position gate (FUN_14012f4a0 equivalent).
 * Called every tick during race PHASE_PRE_SESSION with the leader's
 * normalized track position (0..1) and current monotonic ms.  Flips
 * formation_ended/green_fired as the leader crosses the configured
 * trigger ranges, and populates ts[3]/ts[4] when green fires so the
 * time-driven phase machine advances to PHASE_SESSION on the next
 * tick.  Returns 1 exactly once, when green fires — the caller should
 * broadcast the "Race start initialized" 0x2b system chat.
 */
int	session_advance_race_triggers(struct Server *s,
	    float leader_norm_pos);

/*
 * Human-readable name for a session phase enum value.
 */
const char *
	session_phase_name(uint8_t phase);

/* Kunos-format phase/type names for stdout log_kunos lines. */
const char *
	session_phase_kname(uint8_t phase);
const char *
	session_type_kname(uint8_t session_type);

/*
 * Current session_type with bound-checked index.  Returns 0xff
 * when session_index is out of range (PHASE_RESULTS post-weekend
 * window, or before any session is configured).
 *
 * The is_race / is_qualy wrappers replace the
 * `session_index < session_count && sessions[].session_type == N`
 * pattern that recurred in 12+ sites and shipped two real bugs in
 * v0.2.97 (Quali overtime double-count and welcome session_index
 * OOB) when individual sites forgot one half of the guard.
 */
uint8_t	session_cur_type(const struct Server *s);
int	session_is_race(const struct Server *s);
int	session_is_qualy(const struct Server *s);

#endif /* ACCD_SESSION_H */
