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

#endif /* ACCD_CHAT_H */
