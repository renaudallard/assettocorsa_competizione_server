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

#endif /* ACCD_ENTRYLIST_H */
