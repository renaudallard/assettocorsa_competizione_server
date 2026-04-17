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

/*
 * Exe penalty kind values used by FUN_140125f50 (param_5), 1..6:
 * 1=DriveThrough, 2=StopAndGo10, 3=StopAndGo20, 4=StopAndGo30,
 * 5=PostRaceTime, 6=Disqualified.  Kind 0 = unused slot.
 */
enum penalty_exe_kind {
	EXE_NONE = 0,
	EXE_DT   = 1,
	EXE_SG10 = 2,
	EXE_SG20 = 3,
	EXE_SG30 = 4,
	EXE_TP   = 5,
	EXE_DQ   = 6
};

/* Map our internal PEN_* enum to the exe penalty kind (1..6). */
uint8_t	penalty_exe_kind_of(uint8_t pen_kind);

/*
 * Issue a penalty event matching FUN_140125f50 semantics.
 *   exe_kind  — 1..6 (see enum penalty_exe_kind)
 *   category  — exe local_res20, typically 8 for admin/auto events
 *   value     — counter increment (accumulates to threshold 0x100)
 *   force     — 0 = normal, 1 = admin-forced (skips certain gates)
 *   collision — "c" variant of admin command
 *   reason    — enum penalty_reason for wire translation
 * Fresh entries initialize counter to `value` and return without
 * materializing a Penalty.  Existing entries accumulate; when the
 * counter crosses 0x100 the function appends a Penalty to the car's
 * PenaltyQueue and steps the severity ladder (DT → SG30/DQ etc.).
 * Returns 0 on success, -1 on invalid car / queue full.
 */
int	penalty_enqueue(struct Server *s, int car_id,
		uint8_t exe_kind, uint8_t category,
		int32_t value, int force,
		int collision, uint8_t reason);

/* Translate internal (kind, reason) to the 0..35
 * ServerMonitorPenaltyShortcut wire value. */
uint16_t
	penalty_wire_value(uint8_t kind, uint8_t reason);

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
