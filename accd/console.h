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
