/*
 * bans.c -- persistent kick / ban list.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "bans.h"
#include "log.h"
#include "state.h"

void
bans_init(struct BanList *bl)
{
	memset(bl, 0, sizeof(*bl));
}

void
bans_load(struct BanList *bl, const char *cfg_dir)
{
	char path[512];
	FILE *f;
	char line[64];

	snprintf(path, sizeof(path), "%s/banlist.txt", cfg_dir);
	f = fopen(path, "r");
	if (f == NULL) {
		if (errno != ENOENT)
			log_warn("bans: open %s: %s", path, strerror(errno));
		return;
	}
	while (fgets(line, sizeof(line), f) != NULL) {
		size_t len;

		/* strip whitespace */
		while (line[0] == ' ' || line[0] == '\t')
			memmove(line, line + 1, strlen(line));
		if (line[0] == '#' || line[0] == '\n' || line[0] == '\0')
			continue;
		len = strlen(line);
		while (len > 0 && (line[len - 1] == '\n' ||
		    line[len - 1] == '\r' || line[len - 1] == ' '))
			line[--len] = '\0';
		if (len == 0)
			continue;
		if (bl->count < ACC_MAX_BANS) {
			snprintf(bl->entries[bl->count],
			    sizeof(bl->entries[bl->count]), "%s", line);
			bl->count++;
		}
	}
	fclose(f);
	log_info("bans: loaded %d entries from %s", bl->count, path);
}

void
bans_save(const struct BanList *bl, const char *cfg_dir)
{
	char path[512];
	FILE *f;
	int i;

	snprintf(path, sizeof(path), "%s/banlist.txt", cfg_dir);
	f = fopen(path, "w");
	if (f == NULL) {
		log_warn("bans: write %s: %s", path, strerror(errno));
		return;
	}
	fprintf(f, "# accd ban list -- one Steam64 ID per line\n");
	for (i = 0; i < bl->count; i++)
		fprintf(f, "%s\n", bl->entries[i]);
	fclose(f);
}

int
bans_contains(const struct BanList *bl, const char *steam_id)
{
	int i;

	if (steam_id == NULL || steam_id[0] == '\0')
		return 0;
	for (i = 0; i < bl->count; i++)
		if (strcmp(bl->entries[i], steam_id) == 0)
			return 1;
	return 0;
}

int
bans_add(struct BanList *bl, const char *steam_id)
{
	if (steam_id == NULL || steam_id[0] == '\0')
		return -1;
	if (bans_contains(bl, steam_id))
		return 0;
	if (bl->count >= ACC_MAX_BANS)
		return -1;
	snprintf(bl->entries[bl->count],
	    sizeof(bl->entries[bl->count]), "%s", steam_id);
	bl->count++;
	return 0;
}

int
bans_remove(struct BanList *bl, const char *steam_id)
{
	int i;

	for (i = 0; i < bl->count; i++) {
		if (strcmp(bl->entries[i], steam_id) == 0) {
			memmove(&bl->entries[i], &bl->entries[i + 1],
			    sizeof(bl->entries[0]) * (bl->count - i - 1));
			bl->count--;
			return 0;
		}
	}
	return -1;
}
