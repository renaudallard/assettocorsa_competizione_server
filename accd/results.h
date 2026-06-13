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

/*
 * Append one completed lap to the per-session results log.  Called at
 * lap completion for every lap (valid and invalid) in completion order.
 * is_valid is the exe's results.json isValidForBest verdict.  Grows the
 * log on demand; drops past ACC_RESULTS_LAP_MAX with a one-shot warning.
 */
void	results_laps_append(struct Server *s, uint16_t car_id,
	    uint8_t driver_index, int32_t lap_time_ms,
	    const int32_t splits_ms[3], int is_valid);

/* Clear the results log for a new session (keeps the allocation). */
void	results_laps_reset(struct Server *s);

/* Free the results log (server shutdown). */
void	results_laps_free(struct Server *s);

#endif /* ACCD_RESULTS_H */
