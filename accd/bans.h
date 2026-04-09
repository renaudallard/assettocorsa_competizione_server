/*
 * Copyright (c) 2025-2026 Renaud Allard
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
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

#define ACC_MAX_BANS	256

struct BanList {
	char	entries[ACC_MAX_BANS][32];
	int	count;
};

void	bans_init(struct BanList *bl);
void	bans_load(struct BanList *bl, const char *cfg_dir);
void	bans_save(const struct BanList *bl, const char *cfg_dir);
int	bans_contains(const struct BanList *bl, const char *steam_id);
int	bans_add(struct BanList *bl, const char *steam_id);
int	bans_remove(struct BanList *bl, const char *steam_id);

#endif /* ACCD_BANS_H */
