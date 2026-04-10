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
 * weather.h -- deterministic weather simulator.
 *
 * Drives the WeatherStatus snapshot in the Server struct using
 * a sinusoidal day cycle for clouds + rain + temperature.  The
 * algorithm is a direct port of the rain/cloud cycle in the
 * binary's FUN_140116830 weather simulator (the only function
 * in the dedicated server that calls sinf/cosf).
 *
 * Called from tick.c at the configured cadence.  Emits the
 * 0x37 SRV_WEATHER_STATUS broadcast to all connected clients
 * when significant fields change.
 */

#ifndef ACCD_WEATHER_H
#define ACCD_WEATHER_H

#include "state.h"

/* Initialize the weather state from event.json values.
 * start_time_s is hourOfDay * 3600 so initial cloud/rain
 * values match what weather_step will compute on first tick. */
void	weather_init(struct Server *s, float base_clouds,
		float base_rain, int randomness,
		uint32_t start_time_s);

/*
 * Step the weather simulator forward.  Called every CADENCE_
 * WEATHER ticks (~5 s).  Returns 1 if a significant change
 * occurred and the caller should broadcast 0x37, 0 otherwise.
 */
int	weather_step(struct Server *s);

/* Build a 0x37 weather status body in bb.  Wire format:
 * u8 = 0x37 + 7 × u32 weather factors + WeatherStatus inline
 * (9 × u32) + f32 timestamp.  See §5.6.4a row for 0x37. */
int	weather_build_broadcast(struct Server *s, struct ByteBuf *bb);

#endif /* ACCD_WEATHER_H */
