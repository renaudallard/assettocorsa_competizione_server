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
 * weather.c -- deterministic weather simulator.
 */

#define _POSIX_C_SOURCE 200809L

#include <math.h>
#include <stdint.h>
#include <stddef.h>

#include "io.h"
#include "log.h"
#include "msg.h"
#include "prim.h"
#include "state.h"
#include "weather.h"

/* tau radians per simulated 24-hour cycle */
#define WX_CYCLE_PERIOD_S	(24.0 * 3600.0)
#define WX_RAIN_PHASE		1.7	/* rain peaks ~6h after midday */

void
weather_init(struct Server *s, float base_clouds, float base_rain,
    int randomness)
{
	(void)randomness;
	s->weather.clouds = base_clouds;
	s->weather.current_rain = base_rain;
	s->weather.target_rain = base_rain;
	s->weather.wetness = base_rain;
	s->weather.dry_line_wetness = 0;
	s->weather.puddles = 0;
	s->weather.forecast_10m = base_rain;
	s->weather.forecast_30m = base_rain;
	s->weather.last_step_ms = 0;
}

int
weather_step(struct Server *s)
{
	double t;
	float new_clouds, new_rain;
	float dc, dr;

	/* Use the session weekend time as the simulation clock. */
	t = (double)s->session.weekend_time_s;

	/*
	 * Cloud cycle: a slow sin wave between 0 and 0.6 with
	 * a 4-hour period plus a small offset from the rain phase.
	 */
	new_clouds = (float)(0.3 + 0.3 * sin(t / 3600.0 * 1.5));
	if (new_clouds < 0.0f) new_clouds = 0.0f;
	if (new_clouds > 1.0f) new_clouds = 1.0f;

	/*
	 * Rain follows clouds with a 30-minute lag, ramping
	 * smoothly toward the cloud level when it's high enough.
	 */
	new_rain = new_clouds > 0.5f
	    ? (float)(0.4 * (sin(t / 3600.0 + WX_RAIN_PHASE) + 1.0))
	    : 0.0f;
	if (new_rain < 0.0f) new_rain = 0.0f;
	if (new_rain > 1.0f) new_rain = 1.0f;

	dc = new_clouds - s->weather.clouds;
	dr = new_rain - s->weather.target_rain;

	s->weather.clouds = new_clouds;
	s->weather.target_rain = new_rain;
	/* current_rain chases target_rain at ~10% per step. */
	s->weather.current_rain += (new_rain - s->weather.current_rain) * 0.1f;
	/* wetness follows rain with a slow lag. */
	s->weather.wetness += (s->weather.current_rain -
	    s->weather.wetness) * 0.05f;
	if (s->weather.wetness > 1.0f) s->weather.wetness = 1.0f;
	if (s->weather.wetness < 0.0f) s->weather.wetness = 0.0f;

	s->weather.forecast_10m = new_rain;
	s->weather.forecast_30m = new_rain;

	/* Significant change threshold: 5% in clouds or rain. */
	return (dc * dc > 0.0025f || dr * dr > 0.0025f);
}

int
weather_build_broadcast(struct Server *s, struct ByteBuf *bb)
{
	int i;

	if (wr_u8(bb, SRV_WEATHER_STATUS) < 0)
		return -1;

	/*
	 * 7 × u32 weather scaling factors per the §5.6.4a layout.
	 * The exact constants come from the binary's
	 * tanhf-normalized rain/cloud factors.  We emit conservative
	 * placeholders that the client should accept.
	 */
	for (i = 0; i < 7; i++)
		if (wr_u32(bb, 0) < 0)
			return -1;

	/*
	 * 9 × u32 WeatherStatus fields (per the WeatherStatus::
	 * vftable[0x20] dump from Pass 2.19): 9 floats from
	 * struct offsets 0x28..0x48.
	 */
	if (wr_f32(bb, s->weather.clouds) < 0) return -1;
	if (wr_f32(bb, 22.0f) < 0) return -1;	/* ambient temp */
	if (wr_f32(bb, 26.0f) < 0) return -1;	/* track temp */
	if (wr_f32(bb, s->weather.current_rain) < 0) return -1;
	if (wr_f32(bb, s->weather.wetness) < 0) return -1;
	if (wr_f32(bb, s->weather.dry_line_wetness) < 0) return -1;
	if (wr_f32(bb, s->weather.puddles) < 0) return -1;
	if (wr_f32(bb, s->weather.forecast_10m) < 0) return -1;
	if (wr_f32(bb, s->weather.forecast_30m) < 0) return -1;

	/* Trailing f32 timestamp. */
	if (wr_f32(bb, (float)s->session.weekend_time_s) < 0)
		return -1;
	return 0;
}
