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
 * entrylist.h -- entrylist.json reader.
 *
 * Populates Server.cars[] with CarEntry records and DriverInfo
 * substructures from entrylist.json (UTF-16 LE per Kunos
 * convention).  Schema follows HB §III.4.
 */

#ifndef ACCD_ENTRYLIST_H
#define ACCD_ENTRYLIST_H

#include "state.h"

/*
 * Read cfg_dir/entrylist.json and populate s->cars[].  Sets
 * the `used` flag on each car that has an entry.  Returns the
 * number of cars loaded, or -1 on error.
 */
int	entrylist_load(struct Server *s, const char *cfg_dir);

/*
 * Write the current Server.cars[] to cfg_dir/entrylist.json as
 * UTF-8 JSON.  Intended for the /manual entrylist admin command
 * — captures the live entry list so the next server restart
 * reproduces the current car/driver lineup.  Returns 0 on success,
 * -1 on I/O error.
 */
int	entrylist_save(const struct Server *s, const char *cfg_dir);

/*
 * Read cfg_dir/bop.json (handbook §VI.3) into s->bop[].  File is
 * optional; missing or unparseable yields bop_count = 0 and the
 * apply step becomes a no-op.  Returns the number of entries
 * loaded, or -1 on I/O error.
 */
int	bop_load(struct Server *s, const char *cfg_dir);

/*
 * Apply the loaded BoP table to a single car.  Looks up the
 * (s->track, car->car_model) pair and adds the matching
 * ballast / restrictor on top of whatever the car already
 * carries from entrylist + admin overrides.  Idempotent only if
 * the car was reset first; otherwise consecutive calls would
 * stack the additive.  Call exactly once per car-join.
 */
void	bop_apply(const struct Server *s, struct CarEntry *car);

#endif /* ACCD_ENTRYLIST_H */
