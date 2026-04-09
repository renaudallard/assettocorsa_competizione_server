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
