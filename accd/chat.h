/*
 * chat.h -- chat command parser.
 *
 * Receives a UTF-8 chat string from a client and dispatches:
 *   - "/<command> ..." -> admin command (must be elevated)
 *   - "&swap <driver>" -> non-admin driver swap
 *   - else              -> regular chat broadcast
 *
 * The command set follows §8 / §8.1 of NOTEBOOK_B.md.
 */

#ifndef ACCD_CHAT_H
#define ACCD_CHAT_H

#include "state.h"

/*
 * Process a chat message from c with the given UTF-8 text.  The
 * function may broadcast a 0x2b chat reply to one or more
 * clients, mutate server state, or close the connection (kick).
 */
void	chat_process(struct Server *s, struct Conn *c, const char *text);

#endif /* ACCD_CHAT_H */
