/*
 * console.h -- stdin admin console.
 *
 * When stdin is a TTY, the operator can type admin commands
 * directly (no game client or /admin password required).
 * Commands are the same as the in-game chat set, with the
 * leading / optional.  Replies go to stdout; log output goes
 * to stderr (see log.c).
 *
 * When stdin is not a TTY (e.g. systemd, ./accd < /dev/null),
 * the console disables itself silently.
 */

#ifndef ACCD_CONSOLE_H
#define ACCD_CONSOLE_H

#include "state.h"

/*
 * Initialise the console.  Call once after pledge().  If stdin
 * is not a TTY, the console is permanently disabled and
 * console_fd() returns -1.
 */
void	console_init(void);

/*
 * Return the file descriptor to poll for console input, or -1
 * if the console is disabled.
 */
int	console_fd(void);

/*
 * Called when poll(2) reports POLLIN on the console fd.  Reads
 * available input, dispatches complete lines as commands.
 */
void	console_handle(struct Server *s);

/*
 * Shut down the console (called during server teardown).
 */
void	console_close(void);

#endif /* ACCD_CONSOLE_H */
