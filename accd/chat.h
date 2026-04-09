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
 * chat.h -- chat command parser.
 *
 * Receives a UTF-8 chat string from a client and dispatches:
 *   - "/<command> ..." -> admin command (must be elevated)
 *   - "&swap <driver>" -> non-admin driver swap
 *   - else              -> regular chat broadcast
 *
 * The command set follows §8 / §8.1 of NOTEBOOK_B.md.
 *
 * The chat_do_* helpers are also called by console.c for the
 * stdin admin console, so the action + broadcast logic is
 * shared between in-game chat and the operator terminal.
 */

#ifndef ACCD_CHAT_H
#define ACCD_CHAT_H

#include <stddef.h>
#include <stdint.h>

#include "state.h"

/*
 * Process a chat message from c with the given UTF-8 text.
 * Returns 1 if the message was a command (admin or &swap) and
 * has been fully handled (no chat broadcast needed).  Returns 0
 * if it's a regular chat message that the caller should
 * broadcast via 0x2b.
 */
int	chat_process(struct Server *s, struct Conn *c, const char *text);

/* Utilities shared with console.c. */
int	chat_prefix(const char *s, const char *p);
int	chat_parse_int(const char *s, int *out);
int	chat_car_by_racenum(struct Server *s, int num);

/* Build and broadcast a 0x2b system chat notification. */
void	chat_broadcast(struct Server *s, const char *text, uint8_t type);

/*
 * Action helpers.  Each performs the mutation, broadcasts to
 * in-game clients, and writes a human-readable reply to
 * reply[replysz] if non-NULL.
 */
void	chat_do_kick(struct Server *s, const char *args, int permanent,
	    char *reply, size_t replysz);
void	chat_do_penalty(struct Server *s, const char *cmd,
	    const char *args, int collision,
	    char *reply, size_t replysz);
void	chat_do_bop(struct Server *s, const char *args, int is_ballast,
	    char *reply, size_t replysz);
void	chat_do_track(struct Server *s, const char *args,
	    char *reply, size_t replysz);

#endif /* ACCD_CHAT_H */
