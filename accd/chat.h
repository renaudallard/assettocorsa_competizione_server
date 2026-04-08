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
 * Process a chat message from c with the given UTF-8 text.
 * Returns 1 if the message was a command (admin or &swap) and
 * has been fully handled (no chat broadcast needed).  Returns 0
 * if it's a regular chat message that the caller should
 * broadcast via 0x2b.
 */
int	chat_process(struct Server *s, struct Conn *c, const char *text);

#endif /* ACCD_CHAT_H */
