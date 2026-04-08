/*
 * config.h -- configuration file readers.
 *
 * Reads cfg/configuration.json + cfg/settings.json + cfg/event.json
 * (UTF-16 LE) into the Server struct.  See HB §III for the field
 * meanings.
 */

#ifndef ACCD_CONFIG_H
#define ACCD_CONFIG_H

#include "state.h"

/*
 * Read every JSON file under cfg_dir and populate s.
 * Returns 0 on success, -1 if a required file is missing.
 */
int	config_load(struct Server *s, const char *cfg_dir);

#endif /* ACCD_CONFIG_H */
