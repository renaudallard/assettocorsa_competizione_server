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

/* Initialize the weather state from event.json values. */
void	weather_init(struct Server *s, float base_clouds,
		float base_rain, int randomness);

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
