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
    int randomness, uint32_t start_time_s)
{
	double t = (double)start_time_s;
	float clouds, rain;

	(void)randomness;
	(void)base_clouds;
	(void)base_rain;

	/*
	 * Compute initial cloud/rain from the same sine wave that
	 * weather_step uses, evaluated at the session start time.
	 * This ensures the first weather_step sees zero delta and
	 * won't fire a spurious broadcast during client loading.
	 */
	clouds = (float)(0.3 + 0.3 * sin(t / 3600.0 * 1.5));
	if (clouds < 0.0f) clouds = 0.0f;
	if (clouds > 1.0f) clouds = 1.0f;

	rain = clouds > 0.5f
	    ? (float)(0.4 * (sin(t / 3600.0 + WX_RAIN_PHASE) + 1.0))
	    : 0.0f;
	if (rain < 0.0f) rain = 0.0f;
	if (rain > 1.0f) rain = 1.0f;

	s->weather.wind_speed = 0.1f;
	s->weather.wind_direction = 0.0f;
	s->weather.clouds = clouds;
	s->weather.current_rain = rain;
	s->weather.target_rain = rain;
	s->weather.track_wetness = rain > 0.0f ? 0.7f : 0.0f;
	s->weather.dry_line_wetness = 0;
	s->weather.puddles = 0;
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
	/* track_wetness follows rain with a slow lag. */
	s->weather.track_wetness += (s->weather.current_rain -
	    s->weather.track_wetness) * 0.05f;
	if (s->weather.track_wetness > 1.0f) s->weather.track_wetness = 1.0f;
	if (s->weather.track_wetness < 0.0f) s->weather.track_wetness = 0.0f;

	/* Wind drifts slowly. */
	s->weather.wind_direction += (float)(sin(t / 1800.0) * 2.0);
	s->weather.wind_speed = (float)(0.1 + 0.1 * sin(t / 2400.0));

	/* Significant change threshold: 5% in clouds or rain. */
	if (dc * dc > 0.0025f || dr * dr > 0.0025f) {
		log_debug("weather: clouds=%.2f rain=%.2f wet=%.2f "
		    "t=%.0fs", s->weather.clouds,
		    s->weather.current_rain, s->weather.track_wetness,
		    (double)s->session.weekend_time_s);
		return 1;
	}
	return 0;
}

int
weather_build_broadcast(struct Server *s, struct ByteBuf *bb)
{
	float ambient, road;

	if (wr_u8(bb, SRV_WEATHER_STATUS) < 0)
		return -1;

	ambient = s->session.ambient_temp > 0
	    ? (float)s->session.ambient_temp : 22.0f;
	road = s->session.track_temp > 0
	    ? (float)s->session.track_temp : ambient + 4.0f;

	/*
	 * 17 × f32 body, layout reverse-engineered from a Kunos
	 * accServer.exe v1.10.2 capture:
	 *
	 *   [0]  grip estimation (current)
	 *   [1]  grip green-flag (constant from randomizeGreenFlagTriggers)
	 *   [2..4] reserved (always 0)
	 *   [5]  track wetness (current)
	 *   [6]  track wetness (target/mirror of [5])
	 *   [7]  ambient temp °C
	 *   [8]  road temp °C
	 *   [9]  cloud level (0..1)
	 *   [10] wind direction (degrees, signed)
	 *   [11] rain level (0..1)
	 *   [12] wind speed
	 *   [13] dry-line wetness / puddles factor
	 *   [14..15] reserved (always 0)
	 *   [16] weekend time (seconds, as f32)
	 */
	if (wr_f32(bb, 1.0f - s->weather.clouds * 0.3f) < 0) return -1;
	if (wr_f32(bb, 1.0f - s->weather.clouds * 0.4f) < 0) return -1;
	if (wr_f32(bb, 0.0f) < 0) return -1;
	if (wr_f32(bb, 0.0f) < 0) return -1;
	if (wr_f32(bb, 0.0f) < 0) return -1;
	if (wr_f32(bb, s->weather.track_wetness) < 0) return -1;
	if (wr_f32(bb, s->weather.track_wetness) < 0) return -1;

	if (wr_f32(bb, ambient) < 0) return -1;
	if (wr_f32(bb, road) < 0) return -1;
	if (wr_f32(bb, s->weather.clouds) < 0) return -1;
	if (wr_f32(bb, s->weather.wind_direction) < 0) return -1;
	if (wr_f32(bb, s->weather.current_rain) < 0) return -1;
	if (wr_f32(bb, s->weather.wind_speed) < 0) return -1;
	if (wr_f32(bb, s->weather.dry_line_wetness) < 0) return -1;
	if (wr_f32(bb, 0.0f) < 0) return -1;
	if (wr_f32(bb, 0.0f) < 0) return -1;

	if (wr_f32(bb, (float)s->session.weekend_time_s) < 0)
		return -1;
	return 0;
}
