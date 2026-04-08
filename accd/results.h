/*
 * results.h -- session results JSON file writer.
 *
 * Writes results/YYMMDD_HHMMSS_<sessiontype>.json at session
 * end with the per-car standings, lap times, sector splits.
 * Schema follows §9 of NOTEBOOK_B.md.
 */

#ifndef ACCD_RESULTS_H
#define ACCD_RESULTS_H

#include "state.h"

/*
 * Write the results file for the just-finished session.  Returns
 * 0 on success, -1 on file open / write error.
 */
int	results_write(struct Server *s);

#endif /* ACCD_RESULTS_H */
