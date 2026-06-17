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
 * weather.c: Kunos-faithful Fourier weather simulator.
 *
 * Direct port of accServer.exe FUN_140116c50 (coefficient gen),
 * FUN_140116830 (per-tick evolution), and FUN_1400ee510 (Mersenne
 * Twister + Box-Muller polar Gaussian).  See
 * reference_weather_algorithm.md for the line-by-line breakdown.
 */

#define _POSIX_C_SOURCE 200809L

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

#include "io.h"
#include "log.h"
#include "msg.h"
#include "prim.h"
#include "state.h"
#include "weather.h"

/* Frequencies and phases from accServer.exe .rdata. */
#define WX_OMEGA_CLOUD	7.2721974e-05f	/* 2π / 86400 (24-hour cycle) */
#define WX_OMEGA_RAIN	3.6360987e-05f	/* 2π / 172800 (48-hour cycle) */
#define WX_PHASE_CLOUD	0.5235984f	/* π / 6 */
#define WX_LIN_DRIFT	1.1574074e-05f	/* 1 / 86400 (per-day drift) */
#define WX_CARRIER_K	27502.006f	/* high-freq carrier scale */

static float
clamp01(float v)
{
	if (v < 0.0f) return 0.0f;
	if (v > 1.0f) return 1.0f;
	return v;
}

static float
clampf(float v, float lo, float hi)
{
	if (v < lo) return lo;
	if (v > hi) return hi;
	return v;
}

/* ================================================================== */
/* Mersenne Twister MT19937 + Box-Muller polar Gaussian sampler.       */
/* Transient state, only used during weather_generate_fourier.         */
/* Mirrors FUN_1400ee510's algorithm; tempering uses standard masks    */
/* (functionally equivalent to the exe's non-standard variant for our  */
/* downstream Box-Muller use which only needs uniform-quality output). */
/* ================================================================== */

#define MT_N	624

struct mt_rng {
	uint32_t state[MT_N];
	uint32_t index;
	int has_cached;
	float cached_g;
};

static void
mt_init(struct mt_rng *m, uint32_t seed)
{
	int i;
	m->state[0] = seed;
	for (i = 1; i < MT_N; i++)
		m->state[i] = 1812433253u *
		    (m->state[i - 1] ^ (m->state[i - 1] >> 30)) +
		    (uint32_t)i;
	m->index = MT_N;
	m->has_cached = 0;
	m->cached_g = 0.0f;
}

static uint32_t
mt_next(struct mt_rng *m)
{
	uint32_t y;
	int i;

	if (m->index == MT_N) {
		for (i = 0; i < MT_N; i++) {
			y = (m->state[i] & 0x80000000u) |
			    (m->state[(i + 1) % MT_N] & 0x7fffffffu);
			m->state[i] = m->state[(i + 397) % MT_N] ^ (y >> 1);
			if (y & 1u)
				m->state[i] ^= 0x9908b0dfu;
		}
		m->index = 0;
	}
	y = m->state[m->index++];
	y ^= y >> 11;
	y ^= (y << 7) & 0x9d2c5680u;
	y ^= (y << 15) & 0xefc60000u;
	y ^= y >> 18;
	return y;
}

/*
 * Box-Muller polar method.  Returns a Gaussian with given mean and
 * sigma.  Caches the unused leg so every other call is free.
 */
static float
mt_gaussian(struct mt_rng *m, float mean, float sigma)
{
	float u, v, r2, factor;

	if (m->has_cached) {
		m->has_cached = 0;
		return m->cached_g * sigma + mean;
	}
	do {
		u = (float)mt_next(m) / 2147483648.0f - 1.0f;
		v = (float)mt_next(m) / 2147483648.0f - 1.0f;
		r2 = u * u + v * v;
	} while (r2 >= 1.0f || r2 == 0.0f);
	factor = sqrtf(-2.0f * logf(r2) / r2);
	m->cached_g = factor * v;
	m->has_cached = 1;
	return factor * u * sigma + mean;
}

/* ================================================================== */
/* FUN_140116c50 port: generate sine[]/cosine[] coefficients, derived  */
/* wind seeds, and the dominant-harmonic index.                        */
/* ================================================================== */

static void
weather_generate_fourier(struct Server *s, uint32_t seed)
{
	struct mt_rng m;
	struct WeatherStatus *w = &s->weather;
	float c_first, sigma_var;
	int k, max_idx, ns;

	mt_init(&m, seed);

	if (w->wind_speed_dev < 0.01f)
		w->wind_speed_dev = 0.01f;
	if (w->variability_dev < 0.01f)
		w->variability_dev = 0.01f;
	w->is_dynamic = 1;
	sigma_var = w->variability_dev * 0.4f;

	/*
	 * First cosine sample seeds the chained-Gaussian mean for the
	 * cosine push below; clamp [-1.6, +1.6].
	 */
	c_first = clampf(mt_gaussian(&m, 0.0f, 1.2f), -1.6f, 1.6f);

	/*
	 * sine[0] uses a 0.2× scale (slow linear drift term in the
	 * per-tick step); clamp [-0.8 varDev, +0.8 varDev].
	 */
	{
		float v = clampf(mt_gaussian(&m, 0.0f, sigma_var),
		    -2.0f * sigma_var, 2.0f * sigma_var);
		w->sine_coeffs[0] = v * 0.2f;
	}

	/*
	 * cosine[0] sample with mean = clamped first cosine, sigma 1.3.
	 */
	w->cosine_coeffs[0] = mt_gaussian(&m, c_first, 1.3f);
	w->n_cosine = 1;

	/*
	 * sine[1..nHarmonics-1] each scaled by 2.5 / nHarmonics.
	 */
	ns = w->n_harmonics;
	if (ns > ACCD_WX_MAX_SINE)
		ns = ACCD_WX_MAX_SINE;
	if (ns < 1)
		ns = 1;
	for (k = 2; k <= ns; k++) {
		float v = clampf(mt_gaussian(&m, 0.0f, sigma_var),
		    -2.0f * sigma_var, 2.0f * sigma_var);
		w->sine_coeffs[k - 1] = (2.5f / (float)ns) * v;
	}
	w->n_sine = (uint8_t)ns;
	w->n_harmonics = ns;

	/*
	 * Wind-speed base: |Gaussian(windSpeedMean, windSpeedDeviation)|
	 * matching FUN_140116c50 lines 141-146.  wind_speed_dev already
	 * has the 0.01 floor applied in weather_init.
	 */
	w->wind_speed_base = fabsf(mt_gaussian(&m, w->wind_speed_mean,
	    w->wind_speed_dev));

	/*
	 * Wind direction: FUN_140116c50 (line 147) regenerates the base
	 * only when it is a strictly negative sentinel; the configured or
	 * default 0 is preserved as due North.  The exe draws from libc
	 * rand(); we draw from the MT stream since accd does not aim for
	 * byte parity with the kunos coefficient sequence here.
	 */
	if (w->wind_direction_base < 0.0f)
		w->wind_direction_base = (float)(mt_next(&m) % 360u);

	/*
	 * Wind direction change amplitude: Gaussian(0, 70).
	 */
	w->wind_direction_change = mt_gaussian(&m, 0.0f, 70.0f);

	/*
	 * Find the dominant harmonic index across BOTH arrays (1-based,
	 * sine first).  This becomes wind_harmonic, used by the per-tick
	 * step as the wind-modulation frequency multiplier.
	 */
	max_idx = 1;
	{
		float max_abs = 0.0f;
		int idx = 1;
		for (k = 0; k < (int)w->n_sine; k++) {
			float a = fabsf(w->sine_coeffs[k]);
			if (a > max_abs) {
				max_abs = a;
				max_idx = idx;
			}
			idx++;
		}
		idx = 1;
		for (k = 0; k < (int)w->n_cosine; k++) {
			float a = fabsf(w->cosine_coeffs[k]);
			if (a > max_abs) {
				max_abs = a;
				max_idx = idx;
			}
			idx++;
		}
	}
	w->wind_harmonic = max_idx;

	log_info("weather: generated %d sine + %d cosine harmonics, "
	    "dominant index %d, varDev=%.4f",
	    w->n_sine, w->n_cosine, w->wind_harmonic, w->variability_dev);
}

void
weather_init(struct Server *s, float base_clouds, float base_rain,
    int randomness, uint32_t start_time_s,
    float wind_speed_mean, float wind_speed_dev,
    float weather_base_mean, float weather_base_dev)
{
	struct WeatherStatus *w = &s->weather;

	w->base_clouds = clamp01(base_clouds);
	w->base_rain = clamp01(base_rain);
	w->randomness = (uint8_t)(randomness < 0 ? 0
	    : (randomness > 7 ? 7 : randomness));

	w->clouds = w->base_clouds;
	w->current_rain = w->base_rain;
	w->target_rain = w->base_rain;
	w->track_wetness = w->base_rain > 0.0f
	    ? w->base_rain * 0.7f : 0.0f;
	w->dry_line_wetness = 0.0f;
	w->puddles = 0.0f;
	w->wind_speed = 0.0f;
	w->wind_direction = 0.0f;
	w->ambient_current = 0.0f;
	w->road_current = 0.0f;
	w->last_step_ms = 0;
	w->start_time_s = start_time_s;

	/*
	 * Static-defaults for the Fourier model fields when randomness
	 * is 0.  Mirror Kunos's initial values from FUN_140e3acf0 (AC2)
	 * / FUN_14000e1b0 (server) ctor: ambientMean ~ 24, baseMean 0.4,
	 * baseDev 0.3, varDev 1/7 ≈ 0.143.  These show up on the wire
	 * inside the welcome's TopLevel WeatherData even with no
	 * harmonic drift.
	 * wind_speed_mean/dev and weather_base_mean/dev come from event.json
	 * (FUN_140109bc0 +0x38/+0x3c/+0x50/+0x54); apply the 0.01 minimum
	 * on dev here so weather_generate_fourier sees the clamped value.
	 */
	w->is_dynamic = (uint8_t)(w->randomness > 0);
	w->ambient_mean = 24.0f;
	w->wind_speed_base = 0.0f;
	w->wind_speed_mean = wind_speed_mean;
	w->wind_speed_dev = wind_speed_dev < 0.01f ? 0.01f : wind_speed_dev;
	w->wind_direction_base = 0.0f;
	w->wind_direction_change = 0.0f;
	w->wind_harmonic = 0;
	w->n_harmonics = 0;
	w->weather_base_mean = weather_base_mean;
	w->weather_base_dev = weather_base_dev;
	w->variability_dev = 0.142857f;
	w->n_sine = 0;
	w->n_cosine = 0;

	if (w->randomness > 0) {
		float r = (float)w->randomness * 0.1f;	/* 0..0.7 per JSON map */
		uint32_t seed = start_time_s ^ 0x9e3779b9u ^
		    ((uint32_t)w->randomness << 16);

		/*
		 * nHarmonics = 4 + floor(weatherRandomness * 10).  For
		 * JSON 1..7 this is 5..11 sine coefficients, matching exe.
		 */
		w->n_harmonics = 4 + (int)(r * 10.0f);
		if (w->n_harmonics > ACCD_WX_MAX_SINE)
			w->n_harmonics = ACCD_WX_MAX_SINE;
		w->variability_dev = 0.1f + r * 0.5f;
		weather_generate_fourier(s, seed);
	}
}

/* ================================================================== */
/* FUN_140116830 core: instantaneous cloud / rain / temperature at an  */
/* arbitrary weekend time, with no mutation of the live WeatherStatus.  */
/* weather_step uses it for the live tick; weather_validate samples it  */
/* across the session window.  Excludes the wind and track-wetness lag, */
/* which are stateful live-only fields the rule validator never reads.  */
/* ================================================================== */

static void
weather_eval(const struct WeatherStatus *w, uint32_t time_s,
    float *out_cloud, float *out_rain, float *out_ambient,
    float *out_road, float *out_dryline)
{
	float t = (float)time_s;
	float dt = (float)((double)time_s - (double)w->start_time_s);
	float cloud_phase;	/* fVar7 in decomp: cos(t·ω - π/6) */
	float dryline;		/* fVar12 = -0.2 - cloud_phase */
	float accum, new_cloud, new_rain;
	float ambient_factor, ambient, road, sun;

	/* === Cloud baseline cycle (24h) */
	cloud_phase = cosf(t * WX_OMEGA_CLOUD - WX_PHASE_CLOUD);
	dryline = -0.2f - cloud_phase;

	/*
	 * === Fourier sum into cloud level
	 *
	 * FUN_140116830 is stateless: each call seeds the accumulator and
	 * cloud base from the model's configured base (model+0x38 / +0x3c,
	 * copied into a fresh output struct by FUN_14000e350) and never
	 * mutates the base, so cloud = base + FourierSum(absolute dt) is
	 * recomputed deterministically.  Read the base from the immutable
	 * base_rain / base_clouds, not from the live (written-back)
	 * current_rain / clouds, otherwise each tick re-adds the full
	 * Fourier delta onto the previous output and drifts to saturation.
	 */
	accum = w->base_rain;
	new_cloud = w->base_clouds;
	if (w->n_harmonics > 0 && w->n_sine > 0) {
		int k;
		float cos0 = w->cosine_coeffs[0];
		for (k = 1; k < w->n_sine; k++) {
			/*
			 * Per FUN_140116830 line 38-50: the loop walks
			 * sine[k] for k=1..nSine-1 (sine[0] is reserved
			 * for the linear-drift term applied below) and
			 * uses cosine[0] as a fixed phase modulator
			 * (pfVar2 is never advanced).
			 */
			float phase = sinf(dt * WX_OMEGA_RAIN + cos0);
			float carrier = sinf((phase * WX_CARRIER_K + dt) *
			    (float)(2 * k) * WX_OMEGA_CLOUD);
			accum += carrier * w->sine_coeffs[k];
		}
		accum += dt * w->sine_coeffs[0] * WX_LIN_DRIFT;
		new_cloud = clamp01(accum + w->base_clouds);
	}

	/* === Cloud → rain mapping */
	if (new_cloud <= 0.6f || !w->is_dynamic) {
		new_rain = 0.0f;
	} else {
		new_rain = (new_cloud - 0.6f) * 1.4875001f + 0.15f;
		if (accum <= new_rain && accum <= 0.0f)
			new_rain = 0.0f;
		else if (accum <= new_rain)
			new_rain = accum;
	}

	/* === Ambient temperature */
	ambient_factor = w->ambient_mean * 0.04f;
	ambient = (w->ambient_mean -
	    (new_rain * 4.0f + new_cloud) * ambient_factor) +
	    (6.0f - new_cloud * 3.0f) * dryline;

	/* Cold-night dry-rain suppression: don't surprise-rain the
	 * driver in the dark when it's cold and currently dry. */
	if (dryline <= 0.0f && ambient < 15.0f && new_rain == 0.0f)
		new_rain = 0.0f;

	/* === Road temperature */
	sun = clamp01(dryline);
	road = ((ambient * 0.25f + 5.0f) -
	    (ambient * (new_cloud + new_rain) * 0.125f +
	    (new_cloud + new_rain) * 2.5f) * ambient_factor) * sun + ambient;

	/* === Snow / cold-cloudy tiny-rain path */
	if (cloud_phase >= 0.6f && ambient < 12.0f && new_rain == 0.0f) {
		float fade = clamp01((12.0f - ambient) * 0.14285715f);
		float cloud_extra = clamp01((0.3f - cloud_phase) * -2.0f);
		new_rain = fade * cloud_extra * 0.0002f;
	}

	*out_cloud = new_cloud;
	*out_rain = new_rain;
	*out_ambient = ambient;
	*out_road = road;
	*out_dryline = dryline;
}

/* ================================================================== */
/* FUN_140116830 port: per-tick weather evolution.                     */
/* ================================================================== */

int
weather_step(struct Server *s)
{
	struct WeatherStatus *w = &s->weather;
	float dt;
	float cloud, rain, ambient, road, dryline;
	float prev_cloud = w->clouds;
	float prev_rain = w->current_rain;

	if (w->randomness == 0) {
		/* Static weather: hold all fields at init values. */
		return 0;
	}

	weather_eval(w, s->session.weekend_time_s,
	    &cloud, &rain, &ambient, &road, &dryline);

	w->clouds = cloud;
	w->current_rain = rain;
	w->target_rain = rain;
	w->dry_line_wetness = dryline;
	w->ambient_current = ambient;
	w->road_current = road;

	/* === Wind speed / direction (live-only, not part of validation) */
	dt = (float)((double)s->session.weekend_time_s -
	    (double)w->start_time_s);
	{
		float wind_factor = cosf((float)w->wind_harmonic *
		    WX_OMEGA_CLOUD * dt);
		w->wind_speed = w->wind_speed_base * (1.0f + wind_factor);
		w->wind_direction = w->wind_direction_base +
		    wind_factor * w->wind_direction_change * 3.0f;
		/*
		 * Keep direction in plausible range for HUD.  fmodf
		 * preserves the sign of the dividend, so adding 360 to a
		 * negative modulo (or even -0.0) can land back outside
		 * [0, 360) — collapse with one extra fmodf so wrap is
		 * idempotent.
		 */
		w->wind_direction = fmodf(w->wind_direction, 360.0f);
		if (w->wind_direction < 0.0f)
			w->wind_direction += 360.0f;
		if (w->wind_direction >= 360.0f)
			w->wind_direction = 0.0f;
	}

	/* Track wetness lags rain. */
	w->track_wetness += (w->current_rain - w->track_wetness) * 0.05f;
	w->track_wetness = clamp01(w->track_wetness);

	{
		float dc = cloud - prev_cloud;
		float dr = w->current_rain - prev_rain;
		if (dc * dc > 0.0025f || dr * dr > 0.0025f) {
			log_debug("weather: clouds=%.3f rain=%.3f wet=%.3f "
			    "ambient=%.1f road=%.1f t=%.0fs",
			    cloud, w->current_rain, w->track_wetness,
			    ambient, road, (double)s->session.weekend_time_s);
			return 1;
		}
	}
	return 0;
}

/* ================================================================== */
/* FUN_14011ef30 mirror: re-draw the weekend weather coefficients.     */
/* ================================================================== */

void
weather_redraw(struct Server *s)
{
	struct WeatherStatus *w = &s->weather;
	uint32_t seed;

	if (w->randomness == 0)
		return;	/* static weather: nothing to re-pick */

	/*
	 * Vary the seed per re-draw so each weekend reset (and each retry
	 * inside weather_validate's loop) produces a fresh forecast, the
	 * way the exe advances its global Mersenne state on every pick.
	 */
	s->weather_draw_seq++;
	seed = w->start_time_s ^ 0x9e3779b9u ^
	    ((uint32_t)w->randomness << 16) ^
	    (s->weather_draw_seq * 0x85ebca6bu);
	weather_generate_fourier(s, seed);
}

/* ================================================================== */
/* FUN_140133770 port: validate the forecast against weatherRules.json. */
/* ================================================================== */

static int
weather_reject(const struct WeatherRules *r, const char *fmt, ...)
{
	if (r->verbose) {
		va_list ap;
		char buf[160];

		va_start(ap, fmt);
		(void)vsnprintf(buf, sizeof(buf), fmt, ap);
		va_end(ap);
		log_info("weather: rules rejected draw: %s", buf);
	}
	return 0;
}

int
weather_validate(struct Server *s)
{
	const struct WeatherRules *r = &s->weather_rules;
	const struct SessionDef *sd = &s->sessions[0];
	double start, end, t;
	float factor;
	float max_temp = 0.0f, min_temp = 99.0f;
	float max_rain = 0.0f, min_rain = 1.0f;

	if (!r->active)
		return 1;	/* no validation configured */

	/*
	 * Scan the first session's window (from 4 h before its start to
	 * its simulated end, every 10 min), sampling the forecast and
	 * checking each set bound, mirroring FUN_140133770's loop.
	 */
	factor = sd->time_multiplier > 0 ? (float)sd->time_multiplier : 1.0f;
	start = (double)sd->hour_of_day * 3600.0;
	end = start + (double)sd->duration_min * 60.0 * (double)factor;

	for (t = start - 14400.0; t < end; t += 600.0) {
		float cloud, rain, ambient, road, dryline;
		uint32_t ts = (uint32_t)(t < 0.0 ? 0.0 : t);

		weather_eval(&s->weather, ts, &cloud, &rain, &ambient,
		    &road, &dryline);

		if (rain > max_rain) max_rain = rain;
		if (rain < min_rain) min_rain = rain;
		if (ambient > max_temp) max_temp = ambient;
		if (ambient < min_temp) min_temp = ambient;

		if (r->temp_max_diff > -1 &&
		    (float)r->temp_max_diff < max_temp - min_temp)
			return weather_reject(r, "temp spread %.0f-%.0f > %d",
			    (double)max_temp, (double)min_temp, r->temp_max_diff);
		if (r->rain_max_diff > -1.0f &&
		    r->rain_max_diff < max_rain - min_rain)
			return weather_reject(r, "rain spread %.2f-%.2f > %.2f",
			    (double)max_rain, (double)min_rain,
			    (double)r->rain_max_diff);
		if (r->rain_max > -1.0f && r->rain_max < rain)
			return weather_reject(r, "rain %.2f > %.2f",
			    (double)rain, (double)r->rain_max);
		if (r->rain_min > -1.0f && rain < r->rain_min)
			return weather_reject(r, "rain %.2f < %.2f",
			    (double)rain, (double)r->rain_min);
		if (r->temp_max > -1 && (float)r->temp_max < ambient)
			return weather_reject(r, "temp %.0f > %d",
			    (double)ambient, r->temp_max);
		if (r->temp_min > -1 && ambient < (float)r->temp_min)
			return weather_reject(r, "temp %.0f < %d",
			    (double)ambient, r->temp_min);
		if (r->cloud_max > -1.0f && r->cloud_max < cloud)
			return weather_reject(r, "cloud %.2f > %.2f",
			    (double)cloud, (double)r->cloud_max);
		if (r->cloud_min > -1.0f && cloud < r->cloud_min)
			return weather_reject(r, "cloud %.2f < %.2f",
			    (double)cloud, (double)r->cloud_min);
	}

	/* Post-scan rain-variation rules. */
	if (r->rain_min_diff > -1.0f && max_rain - min_rain < r->rain_min_diff)
		return weather_reject(r, "rain spread %.2f-%.2f < %.2f",
		    (double)max_rain, (double)min_rain,
		    (double)r->rain_min_diff);
	if (r->rain_changes >= 1 && !(min_rain <= 0.05f && max_rain >= 0.25f))
		return weather_reject(r, "rain changes %.2f-%.2f need dry+wet",
		    (double)max_rain, (double)min_rain);
	return 1;
}

/* ================================================================== */
/* 0x37 SRV_WEATHER_STATUS broadcast: periodic body.                   */
/* ================================================================== */

#define WX_TANH_K	0.9f	/* DAT_14014bcd4 */

static inline float
wx_norm(float x)
{
	return tanhf(tanhf(x) * WX_TANH_K);
}

/*
 * 7-float TrackConditions head built by FUN_1400330e0.  Pcap-verified
 * slot layout:
 *   0 grip = 1 - clouds * 0.3
 *   1 0.96 (DAT_14014bcd8)
 *   2..4 0.0
 *   5..6 track wetness
 * Shared with the welcome trailer (write_trailer_additional_state) so
 * the two heads cannot drift; clouds and wet are already wx_norm'd by
 * the caller when the weather is dynamic.
 */
int
weather_write_track_conditions_head(struct ByteBuf *bb, float clouds,
    float wet)
{
	if (wr_f32(bb, 1.0f - clouds * 0.3f) < 0) return -1;
	if (wr_f32(bb, 0.96f) < 0) return -1;	/* DAT_14014bcd8 */
	if (wr_f32(bb, 0.0f) < 0) return -1;
	if (wr_f32(bb, 0.0f) < 0) return -1;
	if (wr_f32(bb, 0.0f) < 0) return -1;
	if (wr_f32(bb, wet) < 0) return -1;
	if (wr_f32(bb, wet) < 0) return -1;
	return 0;
}

int
weather_build_broadcast(struct Server *s, struct ByteBuf *bb)
{
	struct WeatherStatus *w = &s->weather;
	float ambient, road;
	int dyn = w->randomness > 0;
	float rain = dyn ? wx_norm(w->current_rain) : w->current_rain;
	float clouds = dyn ? wx_norm(w->clouds) : w->clouds;
	float wet = dyn ? wx_norm(w->track_wetness) : w->track_wetness;
	float dry = dyn ? wx_norm(w->dry_line_wetness)
	    : w->dry_line_wetness;

	if (wr_u8(bb, SRV_WEATHER_STATUS) < 0)
		return -1;

	if (dyn && w->ambient_current != 0.0f)
		ambient = w->ambient_current;
	else if (s->session.ambient_temp > 0)
		ambient = (float)s->session.ambient_temp;
	else
		ambient = (float)ACC_DEFAULT_AMBIENT_C;
	if (dyn && w->road_current != 0.0f)
		road = w->road_current;
	else if (s->session.track_temp > 0)
		road = (float)s->session.track_temp;
	else
		road = ambient + 4.0f;

	/* 17 × f32 body.  See reference_weather_wire_format.md slot table. */
	if (weather_write_track_conditions_head(bb, clouds, wet) < 0)
		return -1;

	if (wr_f32(bb, ambient) < 0) return -1;
	if (wr_f32(bb, road) < 0) return -1;
	if (wr_f32(bb, w->wind_speed) < 0) return -1;
	if (wr_f32(bb, w->wind_direction) < 0) return -1;
	if (wr_f32(bb, clouds) < 0) return -1;
	if (wr_f32(bb, rain) < 0) return -1;
	if (wr_f32(bb, dry) < 0) return -1;
	if (wr_f32(bb, 0.0f) < 0) return -1;
	if (wr_f32(bb, 0.0f) < 0) return -1;

	if (wr_f32(bb, fmodf((float)s->session.weekend_time_s, 86400.0f)) < 0)
		return -1;
	return 0;
}
