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
