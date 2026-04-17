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

static float
clamp01(float v)
{
	if (v < 0.0f) return 0.0f;
	if (v > 1.0f) return 1.0f;
	return v;
}

void
weather_init(struct Server *s, float base_clouds, float base_rain,
    int randomness, uint32_t start_time_s)
{
	(void)start_time_s;

	s->weather.base_clouds = clamp01(base_clouds);
	s->weather.base_rain = clamp01(base_rain);
	s->weather.randomness = (uint8_t)(randomness < 0 ? 0
	    : (randomness > 7 ? 7 : randomness));

	s->weather.clouds = s->weather.base_clouds;
	s->weather.current_rain = s->weather.base_rain;
	s->weather.target_rain = s->weather.base_rain;
	s->weather.track_wetness = s->weather.base_rain > 0.0f
	    ? s->weather.base_rain * 0.7f : 0.0f;
	s->weather.dry_line_wetness = 0.0f;
	s->weather.puddles = 0.0f;
	s->weather.wind_speed = 0.0f;
	s->weather.wind_direction = 0.0f;
	s->weather.last_step_ms = 0;
}

int
weather_step(struct Server *s)
{
	double t;
	float new_clouds, new_rain, span;
	float dc, dr;

	/*
	 * randomness=0 freezes the weather at its baseline.  This is
	 * exactly what event.json "weatherRandomness": 0 means in
	 * Kunos and what most operators want for predictable races.
	 */
	if (s->weather.randomness == 0)
		return 0;

	t = (double)s->session.weekend_time_s;

	/*
	 * Drift band: ~0.05 per randomness step on each side of the
	 * baseline (so randomness=1 gives a ±0.05 wobble, randomness=7
	 * a ±0.35 wobble).  The cycle period stays in the 30-90 min
	 * range so changes are perceptible but not jarring.
	 */
	span = 0.05f * (float)s->weather.randomness;
	new_clouds = clamp01(s->weather.base_clouds +
	    span * (float)sin(t / 1800.0));
	/*
	 * Rain only develops if base_rain > 0 OR clouds climb above
	 * DAT_14014bcc0 (0.6) — the exe's cloud→rain gate extracted
	 * from accServer.exe .rdata.  We had 0.7 before which kept
	 * drizzle from starting as early as Kunos's simulator does.
	 */
	if (s->weather.base_rain > 0.0f) {
		new_rain = clamp01(s->weather.base_rain +
		    span * (float)sin(t / 2400.0 + WX_RAIN_PHASE));
	} else if (new_clouds > 0.6f) {
		new_rain = clamp01((new_clouds - 0.6f) * 0.5f);
	} else {
		new_rain = 0.0f;
	}

	dc = new_clouds - s->weather.clouds;
	dr = new_rain - s->weather.target_rain;

	s->weather.clouds = new_clouds;
	s->weather.target_rain = new_rain;
	s->weather.current_rain += (new_rain - s->weather.current_rain) * 0.1f;
	s->weather.track_wetness += (s->weather.current_rain -
	    s->weather.track_wetness) * 0.05f;
	s->weather.track_wetness = clamp01(s->weather.track_wetness);

	s->weather.wind_direction += (float)(sin(t / 1800.0) * 2.0);
	s->weather.wind_speed = (float)(0.1 + 0.1 * sin(t / 2400.0));

	if (dc * dc > 0.0025f || dr * dr > 0.0025f) {
		log_debug("weather: clouds=%.2f rain=%.2f wet=%.2f "
		    "t=%.0fs", s->weather.clouds,
		    s->weather.current_rain, s->weather.track_wetness,
		    (double)s->session.weekend_time_s);
		return 1;
	}
	return 0;
}

/*
 * tanhf normalization used by Kunos on rain/cloud/wetness/dry-line when
 * the session runs in dynamic (snowflake) weather mode.  Constant K is
 * DAT_14014bcd4 in accServer.exe .rdata (f32 0.9).
 */
#define WX_TANH_K	0.9f

static inline float
wx_norm(float x)
{
	return tanhf(tanhf(x) * WX_TANH_K);
}

int
weather_build_broadcast(struct Server *s, struct ByteBuf *bb)
{
	float ambient, road;
	int dyn = s->weather.randomness > 0;
	float rain = dyn ? wx_norm(s->weather.current_rain)
	    : s->weather.current_rain;
	float clouds = dyn ? wx_norm(s->weather.clouds)
	    : s->weather.clouds;
	float wet = dyn ? wx_norm(s->weather.track_wetness)
	    : s->weather.track_wetness;
	float dry = dyn ? wx_norm(s->weather.dry_line_wetness)
	    : s->weather.dry_line_wetness;

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
	 *   [1]  grip green-flag (constant DAT_14014bcd8 = 0.96)
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
	 *
	 * In dynamic mode (weatherRandomness > 0) the exe applies
	 * tanhf(tanhf(x) * 0.9) to rain / clouds / wet / dry-line —
	 * see FUN_1400330e0 Branch B in notebook-a.
	 */
	if (wr_f32(bb, 1.0f - clouds * 0.3f) < 0) return -1;
	if (wr_f32(bb, 0.96f) < 0) return -1;	/* DAT_14014bcd8 */
	if (wr_f32(bb, 0.0f) < 0) return -1;
	if (wr_f32(bb, 0.0f) < 0) return -1;
	if (wr_f32(bb, 0.0f) < 0) return -1;
	if (wr_f32(bb, wet) < 0) return -1;
	if (wr_f32(bb, wet) < 0) return -1;

	if (wr_f32(bb, ambient) < 0) return -1;
	if (wr_f32(bb, road) < 0) return -1;
	if (wr_f32(bb, clouds) < 0) return -1;
	if (wr_f32(bb, s->weather.wind_direction) < 0) return -1;
	if (wr_f32(bb, rain) < 0) return -1;
	if (wr_f32(bb, s->weather.wind_speed) < 0) return -1;
	if (wr_f32(bb, dry) < 0) return -1;
	if (wr_f32(bb, 0.0f) < 0) return -1;
	if (wr_f32(bb, 0.0f) < 0) return -1;

	if (wr_f32(bb, (float)s->session.weekend_time_s) < 0)
		return -1;
	return 0;
}
