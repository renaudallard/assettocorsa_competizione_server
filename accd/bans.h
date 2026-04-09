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
 * bans.h -- persistent kick / ban list.
 *
 * Two distinct lists:
 *   - bans: persistent across server restarts.  Steam ID match
 *           refuses handshake with REJECT_BANNED.
 *   - kicks: cleared on race weekend restart.  Steam ID match
 *            refuses handshake with REJECT_KICKED.
 *
 * Storage: cfg/banlist.json (one Steam ID per line, plain text;
 * comments with #).  Kicks are in-memory only and lost on
 * server restart.
 */

#ifndef ACCD_BANS_H
#define ACCD_BANS_H

#include "state.h"

void	bans_init(struct BanList *bl);
void	bans_load(struct BanList *bl, const char *cfg_dir);
void	bans_save(const struct BanList *bl, const char *cfg_dir);
int	bans_contains(const struct BanList *bl, const char *steam_id);
int	bans_add(struct BanList *bl, const char *steam_id);
int	bans_remove(struct BanList *bl, const char *steam_id);

#endif /* ACCD_BANS_H */
