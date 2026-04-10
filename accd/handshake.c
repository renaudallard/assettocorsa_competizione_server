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
 * handshake.c -- ACP_REQUEST_CONNECTION parser and 0x0b response.
 *
 * The 0x09 request body, after the msg id byte, contains:
 *
 *     u16          client_version    (must == ACC_PROTOCOL_VERSION)
 *     string_a     password          (Format A)
 *     ... DriverInfo + CarInfo substructures ...
 *
 * Phase 1 only validates the first two fields and ignores the
 * trailing CarInfo until later phases.  This is enough to make
 * the server respond with either accept or reject.
 *
 * The 0x0b response body is:
 *
 *     u8           msg_id = 0x0b
 *     u16          server protocol version
 *     u8           server flags        (0 for now)
 *     u16          connection_id       (0xFFFF on reject)
 *     ... welcome trailer on accept ...
 *
 * For phase 1 we send the minimum-viable trailer documented in
 * §5.6.4c: carId + trackName + eventId + 0 sessions + empty
 * sub-records + 0 cars.  This is enough for some clients to
 * proceed; if the real client demands more we'll fix it then.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#include "bcast.h"
#include "bans.h"
#include "handshake.h"
#include "io.h"
#include "log.h"
#include "msg.h"
#include "prim.h"
#include "state.h"
#include "weather.h"

/*
 * Kunos 0x0b welcome trailer captured from accServer.exe 1.10.2
 * on mount_panorama with default settings.  1010 bytes total.
 * This is sent verbatim for now; a proper implementation would
 * build the trailer dynamically from server state.
 */
static const unsigned char kunos_welcome_tpl[955] = {
	0x26,0x00,0x41,0x43,0x43,0x20,0x53,0x65,0x72,0x76,0x65,0x72,
	0x20,0x28,0x70,0x6c,0x65,0x61,0x73,0x65,0x20,0x65,0x64,0x69,
	0x74,0x20,0x73,0x65,0x74,0x74,0x69,0x6e,0x67,0x73,0x2e,0x6a,
	0x73,0x6f,0x6e,0x29,0x03,0x00,0x73,0x70,0x61,0x01,0xe9,0x03,
	0x01,0x01,0x30,0x00,0x00,0x00,0x30,0x00,0x00,0x00,0x30,0x00,
	0x00,0x00,0x30,0x00,0x00,0x00,0x30,0x00,0x00,0x00,0x31,0x00,
	0x00,0x00,0x2a,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x23,
	0x07,0xde,0xb3,0x00,0x0b,0x00,0x80,0x00,0x00,0x00,0x00,0x00,
	0x00,0x00,0x00,0x22,0x07,0xdf,0xb3,0x00,0x00,0x00,0x80,0x00,
	0x00,0x69,0x6e,0x00,0x00,0x72,0x70,0x00,0x00,0x74,0x65,0x00,
	0x00,0x6f,0x70,0x00,0x00,0x6f,0x6e,0x00,0x00,0x00,0x00,0x0f,
	0x00,0x00,0x02,0x00,0x00,0x00,0x00,0x01,0x09,0x4c,0x00,0x00,
	0x00,0x69,0x00,0x00,0x00,0x66,0x00,0x00,0x00,0x65,0x00,0x00,
	0x00,0x63,0x00,0x00,0x00,0x79,0x00,0x00,0x00,0x63,0x00,0x00,
	0x00,0x6c,0x00,0x00,0x00,0x65,0x00,0x00,0x00,0x03,0x4c,0x00,
	0x00,0x00,0x46,0x00,0x00,0x00,0x54,0x00,0x00,0x00,0x03,0x4c,
	0x00,0x00,0x00,0x46,0x00,0x00,0x00,0x54,0x00,0x00,0x00,0x00,
	0x00,0x00,0x12,0x53,0x00,0x00,0x00,0x37,0x00,0x00,0x00,0x36,
	0x00,0x00,0x00,0x35,0x00,0x00,0x00,0x36,0x00,0x00,0x00,0x31,
	0x00,0x00,0x00,0x31,0x00,0x00,0x00,0x39,0x00,0x00,0x00,0x38,
	0x00,0x00,0x00,0x30,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
	0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
	0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x02,
	0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x00,0x00,0x00,0x00,0x00,
	0x00,0x80,0x3f,0x02,0x02,0x02,0x02,0x02,0x02,0x00,0x05,0x00,
	0x05,0x00,0x04,0x00,0x00,0xcd,0xcc,0x4c,0x3f,0x01,0x00,0x00,
	0x80,0x3f,0x00,0x00,0x00,0x3f,0x01,0x01,0x01,0x01,0x01,0x01,
	0x01,0x01,0x02,0x00,0x00,0x02,0x64,0x64,0x0f,0x00,0x00,0x00,
	0x00,0x00,0x02,0x02,0x2c,0x01,0x0a,0x03,0x02,0x02,0x02,0x02,
	0x02,0x02,0x02,0x02,0x00,0x00,0x00,0x64,0x00,0x00,0x00,0xb8,
	0x0b,0x00,0x00,0x0f,0x00,0x00,0x00,0x03,0x00,0x00,0x00,0x00,
	0x00,0x00,0x00,0x00,0x01,0x00,0x03,0x73,0x00,0x00,0x00,0x70,
	0x00,0x00,0x00,0x61,0x00,0x00,0x00,0x01,0x20,0x03,0x86,0x1c,
	0x5b,0x3f,0x0a,0xd7,0x23,0x3c,0xbe,0xdc,0xa7,0x3c,0x00,0x00,
	0x80,0x3f,0x00,0x05,0x00,0x05,0x00,0x04,0x00,0x00,0xff,0x01,
	0xff,0xff,0xff,0xff,0xff,0x01,0x00,0x01,0x00,0xff,0xff,0x00,
	0x00,0x00,0x01,0x01,0x32,0x03,0x00,0x00,0x00,0xb0,0x41,0x00,
	0x00,0xf0,0x41,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xcd,
	0xcc,0xcc,0x3d,0x00,0x00,0x00,0x00,0x00,0x00,0x80,0x3f,0x00,
	0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
	0x00,0xc0,0x41,0x00,0x00,0x80,0xbf,0x00,0x00,0xa0,0x40,0x00,
	0x00,0x70,0x41,0x00,0x00,0x80,0xbf,0x00,0x00,0x00,0x00,0x00,
	0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xcd,0xcc,0xcc,0x3e,0x9a,
	0x99,0x99,0x3e,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
	0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x06,0x00,0x01,0x00,0x00,
	0x80,0x3f,0x03,0x00,0x58,0x02,0x00,0x00,0x78,0x00,0x00,0x00,
	0x00,0x00,0x00,0x00,0x80,0x3f,0xff,0xff,0xff,0x7f,0x03,0xff,
	0xff,0xff,0x7f,0xff,0xff,0xff,0x7f,0xff,0xff,0xff,0x7f,0x00,
	0x01,0x00,0xe9,0x03,0x30,0x00,0x00,0x02,0x00,0x00,0x00,0x00,
	0x01,0x00,0x00,0x09,0x4c,0x00,0x00,0x00,0x69,0x00,0x00,0x00,
	0x66,0x00,0x00,0x00,0x65,0x00,0x00,0x00,0x63,0x00,0x00,0x00,
	0x79,0x00,0x00,0x00,0x63,0x00,0x00,0x00,0x6c,0x00,0x00,0x00,
	0x65,0x00,0x00,0x00,0x03,0x4c,0x00,0x00,0x00,0x46,0x00,0x00,
	0x00,0x54,0x00,0x00,0x00,0x00,0x12,0x53,0x00,0x00,0xff,0xff,
	0xff,0x7f,0xff,0xff,0xff,0x7f,0x00,0x00,0xff,0xff,0xff,0x7f,
	0xff,0x01,0x00,0x03,0xff,0xff,0xff,0x7f,0xff,0xff,0xff,0x7f,
	0xff,0xff,0xff,0x7f,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,
	0x33,0xf2,0xb5,0x41,0x9e,0xb3,0xf4,0x3f,0x6e,0xdb,0xb6,0x3f,
	0xb8,0x6d,0xdb,0x3e,0x34,0xe0,0x29,0x43,0x1d,0x36,0xf4,0x42,
	0x01,0x00,0x00,0x00,0x05,0x00,0x00,0x00,0xcd,0xcc,0xcc,0x3e,
	0x9a,0x99,0x99,0x3e,0x25,0x49,0x12,0x3e,0x05,0x00,0xec,0x84,
	0x80,0xbc,0xbe,0xd2,0xb4,0xbc,0xcf,0xb8,0x30,0xbc,0xac,0x04,
	0x1f,0xbb,0x5c,0x1f,0x13,0xbd,0x01,0x00,0x70,0x55,0xcb,0xbf,
	0xd0,0xbf,0x78,0x3f,0x8f,0xc2,0x75,0x3f,0x00,0x00,0x00,0x00,
	0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xad,0x76,0x95,0x3e,
	0xad,0x76,0x95,0x3e,0x07,0x88,0x95,0x41,0x07,0x88,0x95,0x41,
	0x22,0xb4,0xf4,0x3f,0xfa,0xe0,0x29,0x43,0x54,0xfd,0xfe,0x3d,
	0x00,0x00,0x00,0x00,0xa2,0x33,0x33,0xbf,0x00,0x00,0x00,0x00,
	0x00,0x00,0x00,0x00,0x00,0xc0,0xa8,0x46,0x03,0x06,0x00,0x01,
	0x00,0x00,0x80,0x3f,0x03,0x00,0x58,0x02,0x00,0x00,0x78,0x00,
	0x00,0x00,0x00,0x00,0x00,0x00,0x80,0x3f,0x0c,0x00,0x01,0x00,
	0x00,0x80,0x3f,0x03,0x00,0x58,0x02,0x00,0x00,0x78,0x00,0x00,
	0x00,0x00,0x04,0x00,0x00,0x80,0x3f,0x12,0x00,0x02,0x00,0x00,
	0x00,0x40,0x50,0x00,0xb0,0x04,0x00,0x00,0x78,0x00,0x00,0x00,
	0x00,0x0a,0x00,0x00,0x80,0x3f,0x05,0x05,0x00,0x00,0x00,0x00,
	0x00,0x00,0xff,0x30,0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x00,
	0x00,0x00,0x00,0x08,0x00,0x53,0x74,0x61,0x6e,0x64,0x61,0x72,
	0x64,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
	0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
	0x00,0x00,0x00,0x00,0x03,0x00,0x00
};

/*
 * Config section template (328 bytes).  Verified identical between
 * probe and real client Kunos captures.  Contains assist rules,
 * server config, track name (Format-A), weather snapshot, and
 * session definitions.
 */
static const unsigned char config_tpl[328] = {
	0x01,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x00,0x00,0x00,
	0x00,0x00,0x00,0x80,0x3f,0x02,0x02,0x02,0x02,0x02,0x02,0x00,
	0x05,0x00,0x05,0x00,0x04,0x00,0x00,0xcd,0xcc,0x4c,0x3f,0x01,
	0x00,0x00,0x80,0x3f,0x00,0x00,0x00,0x3f,0x01,0x01,0x01,0x01,
	0x01,0x01,0x01,0x01,0x02,0x00,0x00,0x02,0x64,0x64,0x0f,0x00,
	0x00,0x00,0x00,0x00,0x02,0x02,0x2c,0x01,0x0a,0x03,0x02,0x02,
	0x02,0x02,0x02,0x02,0x02,0x02,0x00,0x00,0x00,0x64,0x00,0x00,
	0x00,0xb8,0x0b,0x00,0x00,0x0f,0x00,0x00,0x00,0x03,0x00,0x00,
	0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x00,0x0e,0x6d,0x00,0x00,
	0x00,0x6f,0x00,0x00,0x00,0x75,0x00,0x00,0x00,0x6e,0x00,0x00,
	0x00,0x74,0x00,0x00,0x00,0x5f,0x00,0x00,0x00,0x70,0x00,0x00,
	0x00,0x61,0x00,0x00,0x00,0x6e,0x00,0x00,0x00,0x6f,0x00,0x00,
	0x00,0x72,0x00,0x00,0x00,0x61,0x00,0x00,0x00,0x6d,0x00,0x00,
	0x00,0x61,0x00,0x00,0x00,0x01,0x20,0x03,0x86,0x1c,0x5b,0x3f,
	0x0a,0xd7,0x23,0x3c,0xbe,0xdc,0xa7,0x3c,0x00,0x00,0x80,0x3f,
	0x00,0x05,0x00,0x05,0x00,0x04,0x00,0x00,0xff,0x01,0xff,0xff,
	0xff,0xff,0xff,0x01,0x00,0x01,0x00,0xff,0xff,0x00,0x00,0x00,
	0x01,0x01,0x32,0x03,0x00,0x00,0x00,0xb0,0x41,0x00,0x00,0xf0,
	0x41,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xcd,0xcc,0xcc,
	0x3d,0x00,0x00,0x00,0x00,0x00,0x00,0x80,0x3f,0x00,0x00,0x00,
	0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xc0,
	0x41,0x00,0x00,0x80,0xbf,0x00,0x00,0xa0,0x40,0x00,0x00,0x70,
	0x41,0x00,0x00,0x80,0xbf,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
	0x00,0x00,0x00,0x00,0x00,0xcd,0xcc,0xcc,0x3e,0x9a,0x99,0x99,
	0x3e,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
	0x00,0x00,0x00,0x00,0x00,0x06,0x00,0x01,0x00,0x00,0x80,0x3f,
	0x03,0x00,0x58,0x02,0x00,0x00,0x78,0x00,0x00,0x00,0x00,0x00,
	0x00,0x00,0x80,0x3f
};

int
build_welcome_trailer(struct ByteBuf *bb, struct Server *s, struct Conn *c)
{
	struct CarEntry *car = NULL;
	struct DriverInfo *drv = NULL;
	int i, j, nc;

	if (c->car_id >= 0 && c->car_id < ACC_MAX_CARS) {
		car = &s->cars[c->car_id];
		if (car->driver_count > 0)
			drv = &car->drivers[0];
	}

	/* Send the full Kunos template verbatim. */
	if (bb_append(bb, kunos_welcome_tpl,
	    sizeof(kunos_welcome_tpl)) < 0)
		return -1;

	return 0;
}

/*
 * Original build_welcome_trailer - disabled pending rewrite.
 */
static int
build_welcome_trailer_disabled(struct ByteBuf *bb, struct Server *s,
    struct Conn *c)
{
	int i;

	if (wr_str_raw(bb, s->server_name) < 0)
		return -1;
	if (wr_str_raw(bb, s->track) < 0)
		return -1;

	/* Separator + assigned car_id (wire ID, base 1000). */
	if (wr_u8(bb, 1) < 0)
		return -1;
	if (wr_u16(bb, s->cars[c->car_id].car_id) < 0)
		return -1;

	/* Session flags. */
	if (wr_u8(bb, 1) < 0)
		return -1;
	if (wr_u8(bb, 1) < 0)
		return -1;

	/* preRaceWaitingTimeSeconds, sessionOverTimeSeconds. */
	if (wr_u32(bb, 52) < 0)
		return -1;
	if (wr_u32(bb, 50) < 0)
		return -1;

	/* Echoed car info: raceNumber (i32), carModel (u8). */
	{
		struct CarEntry *car = NULL;
		struct DriverInfo *drv = NULL;
		int j;

		if (c->car_id >= 0 && c->car_id < ACC_MAX_CARS) {
			car = &s->cars[c->car_id];
			if (car->driver_count > 0)
				drv = &car->drivers[0];
		}

		if (wr_i32(bb, car ? car->race_number : 0) < 0)
			return -1;
		if (wr_u8(bb, car ? car->car_model : 0) < 0)
			return -1;

		/* Per-track season entity (pit positions, sector
		 * markers). Zero-filled; the client loads its own
		 * track data from game files. */
		for (i = 0; i < 1052; i++)
			if (wr_u8(bb, 0) < 0)
				return -1;

		/* Separator + echoed DriverInfo (Format-A). */
		if (wr_u8(bb, 1) < 0)
			return -1;
		if (wr_str_a(bb, drv ? drv->first_name : "") < 0)
			return -1;
		if (wr_str_a(bb, drv ? drv->last_name : "") < 0)
			return -1;
		if (wr_str_a(bb, drv ? drv->short_name : "") < 0)
			return -1;
		if (wr_u8(bb, drv ? drv->driver_category : 0) < 0)
			return -1;
		if (wr_u16(bb, drv ? drv->nationality : 0) < 0)
			return -1;

		/* Padding + steam_id as Format-A. */
		for (i = 0; i < 10; i++)
			if (wr_u8(bb, 0) < 0)
				return -1;
		if (wr_str_a(bb, drv ? drv->steam_id : "") < 0)
			return -1;

		/* Padding to reach assist rules block. */
		for (i = 0; i < 16; i++)
			if (wr_u8(bb, 0) < 0)
				return -1;

		/* AssistRules: separator + 8x u8 + padding + f32(1.0)
		 * + 6x u8 assist flags. */
		if (wr_u8(bb, 1) < 0)
			return -1;
		for (i = 0; i < 8; i++)
			if (wr_u8(bb, 2) < 0)
				return -1;
		if (wr_u32(bb, 0) < 0)
			return -1;
		if (wr_f32(bb, 1.0f) < 0)
			return -1;
		for (i = 0; i < 6; i++)
			if (wr_u8(bb, 2) < 0)
				return -1;

		/* Server config fields. */
		if (wr_u8(bb, 0) < 0) return -1;
		if (wr_u8(bb, 5) < 0) return -1;
		if (wr_u8(bb, 0) < 0) return -1;
		if (wr_u8(bb, 5) < 0) return -1;
		if (wr_u8(bb, 0) < 0) return -1;
		if (wr_u8(bb, 4) < 0) return -1;
		if (wr_u16(bb, 0) < 0) return -1;
		if (wr_f32(bb, 0.8f) < 0) return -1;
		if (wr_u8(bb, 1) < 0) return -1;
		if (wr_f32(bb, 1.0f) < 0) return -1;
		if (wr_f32(bb, 0.5f) < 0) return -1;

		/* More config: formation lap, driver swap, etc. */
		for (i = 0; i < 8; i++)
			if (wr_u8(bb, 1) < 0)
				return -1;
		if (wr_u8(bb, 2) < 0) return -1;
		if (wr_u16(bb, 0) < 0) return -1;
		if (wr_u8(bb, 2) < 0) return -1;
		if (wr_u8(bb, 100) < 0) return -1;
		if (wr_u8(bb, 100) < 0) return -1;
		if (wr_u8(bb, 15) < 0) return -1;
		if (wr_u32(bb, 0) < 0) return -1;
		if (wr_u8(bb, 0) < 0) return -1;
		if (wr_u8(bb, 2) < 0) return -1;
		if (wr_u8(bb, 2) < 0) return -1;
		if (wr_u16(bb, 300) < 0) return -1;
		if (wr_u8(bb, 1) < 0) return -1;
		if (wr_u8(bb, 10) < 0) return -1;
		if (wr_u8(bb, 3) < 0) return -1;
		for (i = 0; i < 8; i++)
			if (wr_u8(bb, 2) < 0)
				return -1;
		if (wr_u32(bb, 100) < 0) return -1;
		if (wr_u32(bb, 3000) < 0) return -1;
		if (wr_u32(bb, 15) < 0) return -1;
		if (wr_u32(bb, 3) < 0) return -1;
		if (wr_u32(bb, 0) < 0) return -1;
		if (wr_u8(bb, 0) < 0) return -1;

		/* Track name as Format-A. */
		if (wr_u8(bb, 1) < 0) return -1;
		if (wr_u8(bb, 0) < 0) return -1;
		if (wr_str_a(bb, s->track) < 0)
			return -1;

		/* Weather inline snapshot (separator + f32 fields). */
		if (wr_u8(bb, 1) < 0) return -1;
		{
			float g2 = s->session.grip_level > 0
			    ? s->session.grip_level : 1.0f;

			if (wr_u8(bb, 0x20) < 0) return -1;
			if (wr_u8(bb, 3) < 0) return -1;
			if (wr_f32(bb, g2) < 0) return -1;
			if (wr_f32(bb, s->weather.clouds * 0.1f) < 0)
				return -1;
			if (wr_f32(bb, s->weather.current_rain * 0.1f) < 0)
				return -1;
			if (wr_f32(bb, 1.0f) < 0) return -1;
			if (wr_u8(bb, 0) < 0) return -1;
			if (wr_u8(bb, 5) < 0) return -1;
			if (wr_u8(bb, 0) < 0) return -1;
			if (wr_u8(bb, 5) < 0) return -1;
			if (wr_u8(bb, 0) < 0) return -1;
			if (wr_u8(bb, 4) < 0) return -1;
			if (wr_u16(bb, 0) < 0) return -1;
			if (wr_u8(bb, 0xFF) < 0) return -1;
			if (wr_u8(bb, 1) < 0) return -1;
			if (wr_u32(bb, 0xFFFFFFFF) < 0) return -1;
			if (wr_u8(bb, 0xFF) < 0) return -1;
			if (wr_u8(bb, 1) < 0) return -1;
			if (wr_u8(bb, 0) < 0) return -1;
			if (wr_u8(bb, 1) < 0) return -1;
			if (wr_u8(bb, 0) < 0) return -1;
			if (wr_u16(bb, 0xFFFF) < 0) return -1;
			if (wr_u32(bb, 0) < 0) return -1;
			if (wr_u8(bb, 1) < 0) return -1;
		}

		/* Session definitions (per-session records). */
		if (wr_u8(bb, 1) < 0) return -1;
		if (wr_u8(bb, 0x32) < 0) return -1;
		if (wr_u8(bb, 3) < 0) return -1;
		for (j = 0; j < 3; j++) {
			float a_t = s->session.ambient_temp > 0
			    ? (float)s->session.ambient_temp : 22.0f;

			if (wr_f32(bb, a_t) < 0) return -1;
			if (wr_f32(bb, a_t + 8.0f) < 0) return -1;
			if (wr_f32(bb, 0.0f) < 0) return -1;
			if (wr_f32(bb, 0.0f) < 0) return -1;
			if (wr_f32(bb, 0.1f) < 0) return -1;
			if (wr_f32(bb, 0.0f) < 0) return -1;
			if (wr_f32(bb, 1.0f) < 0) return -1;
			if (wr_f32(bb, 0.0f) < 0) return -1;
			if (wr_f32(bb, 0.0f) < 0) return -1;
			if (wr_f32(bb, 0.0f) < 0) return -1;
			if (wr_f32(bb, a_t) < 0) return -1;
			if (wr_f32(bb, -1.0f) < 0) return -1;
			if (wr_f32(bb, 5.0f) < 0) return -1;
			if (wr_f32(bb, 15.0f) < 0) return -1;
			if (wr_f32(bb, -1.0f) < 0) return -1;
			if (wr_f32(bb, 0.0f) < 0) return -1;
			if (wr_f32(bb, 0.0f) < 0) return -1;
			if (wr_f32(bb, 0.0f) < 0) return -1;
			if (wr_f32(bb, 0.4f) < 0) return -1;
			if (wr_f32(bb, 0.3f) < 0) return -1;
			if (wr_f32(bb, 0.0f) < 0) return -1;
			if (wr_f32(bb, 0.0f) < 0) return -1;
			if (wr_f32(bb, 0.0f) < 0) return -1;
			if (wr_f32(bb, 0.0f) < 0) return -1;
		}

		/* Session schedule entries. */
		for (j = 0; j < s->session_count && j < 3; j++) {
			const struct SessionDef *def = &s->sessions[j];

			if (wr_u8(bb, def->hour_of_day) < 0) return -1;
			if (wr_u8(bb, 0) < 0) return -1;
			if (wr_u8(bb, def->time_multiplier) < 0) return -1;
			if (wr_u16(bb, 0) < 0) return -1;
			if (wr_f32(bb, 1.0f) < 0) return -1;
			if (wr_u16(bb, 3) < 0) return -1;
			if (wr_u8(bb, 0) < 0) return -1;
			if (wr_u16(bb, (uint16_t)(def->duration_min * 60)) < 0)
				return -1;
			if (wr_u8(bb, 0) < 0) return -1;
			if (wr_u8(bb, 0) < 0) return -1;
			if (wr_u16(bb, 120) < 0) return -1;
			if (wr_u8(bb, 0) < 0) return -1;
			if (wr_u8(bb, 0) < 0) return -1;
			if (wr_u8(bb, 0) < 0) return -1;
			if (wr_u8(bb, 0) < 0) return -1;
			if (wr_f32(bb, 1.0f) < 0) return -1;
		}

		/* Leaderboard inline + car entries. */
		if (wr_u32(bb, 0x7FFFFFFF) < 0) return -1;
		if (wr_u8(bb, 3) < 0) return -1;
		if (wr_i32(bb, 0x7FFFFFFF) < 0) return -1;
		if (wr_i32(bb, 0x7FFFFFFF) < 0) return -1;
		if (wr_i32(bb, 0x7FFFFFFF) < 0) return -1;

		{
			int nc = 0;
			for (j = 0; j < ACC_MAX_CARS; j++)
				if (s->cars[j].used) nc++;
			if (wr_u8(bb, 0) < 0) return -1;
			if (wr_u8(bb, (uint8_t)nc) < 0) return -1;
			for (j = 0; j < ACC_MAX_CARS; j++) {
				struct CarEntry *ec = &s->cars[j];
				struct DriverInfo *ed;

				if (!ec->used) continue;
				ed = &ec->drivers[0];
				if (wr_u8(bb, 0) < 0) return -1;
				if (wr_u16(bb, ec->car_id) < 0) return -1;
				if (wr_u16(bb, (uint16_t)ec->race_number) < 0)
					return -1;
				if (wr_u8(bb, ec->car_model) < 0) return -1;
				if (wr_u8(bb, ec->cup_category) < 0) return -1;
				if (wr_u32(bb, 0) < 0) return -1;
				if (wr_u8(bb, ec->driver_count) < 0) return -1;
				if (wr_str_a(bb, ed->steam_id) < 0) return -1;
				if (wr_str_a(bb, ed->short_name) < 0) return -1;
				if (wr_str_a(bb, ed->first_name) < 0) return -1;
				if (wr_str_a(bb, ed->last_name) < 0) return -1;
				if (wr_u16(bb, 0) < 0) return -1;
				if (wr_u8(bb, 0) < 0) return -1;
				if (wr_u32(bb, 0x7FFFFFFF) < 0) return -1;
				if (wr_u32(bb, 0x7FFFFFFF) < 0) return -1;
				if (wr_u8(bb, 0) < 0) return -1;
				if (wr_u32(bb, 0x7FFFFFFF) < 0) return -1;
				if (wr_u8(bb, 1) < 0) return -1;
				if (wr_u8(bb, 0) < 0) return -1;
				if (wr_u8(bb, 3) < 0) return -1;
				if (wr_i32(bb, 0x7FFFFFFF) < 0) return -1;
				if (wr_i32(bb, 0x7FFFFFFF) < 0) return -1;
				if (wr_i32(bb, 0x7FFFFFFF) < 0) return -1;
				if (wr_u32(bb, 0) < 0) return -1;
				if (wr_u32(bb, 0) < 0) return -1;
			}
		}

		/* Per-car realtime data placeholder. */
		if (wr_u8(bb, 1) < 0) return -1;
		if (wr_u32(bb, 0) < 0) return -1;
		{
			float g = s->session.grip_level > 0
			    ? s->session.grip_level : 1.0f;

			if (wr_f32(bb, g) < 0) return -1;
			if (wr_f32(bb, s->weather.clouds) < 0) return -1;
			if (wr_f32(bb, s->weather.current_rain) < 0) return -1;
			if (wr_u32(bb, 0) < 0) return -1;
			if (wr_u8(bb, 5) < 0) return -1;
			if (wr_u32(bb, 0) < 0) return -1;
			if (wr_f32(bb, 0.4f) < 0) return -1;
			if (wr_f32(bb, 0.3f) < 0) return -1;
			if (wr_f32(bb, 0.0f) < 0) return -1;
			if (wr_u8(bb, 5) < 0) return -1;
			for (i = 0; i < 20; i++)
				if (wr_u8(bb, 0) < 0) return -1;
		}

		/* Inline weather snapshot. */
		if (wr_u8(bb, 1) < 0) return -1;
		if (wr_u8(bb, 0) < 0) return -1;
		if (weather_build_broadcast(s, bb) < 0)
			return -1;

		/* Session schedule recap. */
		for (j = 0; j < s->session_count && j < 3; j++) {
			const struct SessionDef *def = &s->sessions[j];

			if (wr_u8(bb, def->hour_of_day) < 0) return -1;
			if (wr_u8(bb, 0) < 0) return -1;
			if (wr_u8(bb, def->time_multiplier) < 0) return -1;
			if (wr_u16(bb, 0) < 0) return -1;
			if (wr_f32(bb, 1.0f) < 0) return -1;
			if (wr_u16(bb, 3) < 0) return -1;
			if (wr_u8(bb, 0) < 0) return -1;
			if (wr_u16(bb, (uint16_t)(def->duration_min * 60)) < 0)
				return -1;
			if (wr_u8(bb, 0) < 0) return -1;
			if (wr_u8(bb, 0) < 0) return -1;
			if (wr_u16(bb, 120) < 0) return -1;
			if (wr_u8(bb, 0) < 0) return -1;
			if (wr_u8(bb, 0) < 0) return -1;
			if (wr_u8(bb, 0) < 0) return -1;
			if (wr_u8(bb, 0) < 0) return -1;
			if (wr_f32(bb, 1.0f) < 0) return -1;
		}

		/* Tyre compound + tail. */
		if (wr_u8(bb, 5) < 0) return -1;
		if (wr_u8(bb, 5) < 0) return -1;
		for (i = 0; i < 6; i++)
			if (wr_u8(bb, 0) < 0) return -1;
		if (wr_u8(bb, 0xFF) < 0) return -1;
		/* 0x40 variant for tyre compound. */
		if (wr_u8(bb, 0x40) < 0) return -1;
		if (wr_u32(bb, 0xFFFFFFFF) < 0) return -1;
		if (wr_u32(bb, 0) < 0) return -1;
		if (wr_u32(bb, 0) < 0) return -1;
		if (wr_u8(bb, 0) < 0) return -1;
		/* Compound name "Standard". */
		if (wr_u8(bb, 8) < 0) return -1;
		if (bb_append(bb, "Standard", 8) < 0) return -1;
		/* Tail padding. */
		for (i = 0; i < 24; i++)
			if (wr_u8(bb, 0) < 0) return -1;
		if (wr_u8(bb, 3) < 0) return -1;
		if (wr_u16(bb, 0) < 0) return -1;
		if (wr_u8(bb, 0) < 0) return -1;
	}

	return 0;
}

/*
 * Send a 14-byte 0x0c reject response matching the real server
 * format: u8(0x0c) + u32(server_ver=7) + u8(0) +
 * u16(client_ver_echo) + u16(0) + u16(ACC_PROTOCOL_VERSION) +
 * u16(0).
 */
static int
handshake_send_reject(struct Conn *c, uint16_t client_version)
{
	struct ByteBuf bb;
	int rc;

	bb_init(&bb);
	if (wr_u8(&bb, SRV_STATE_RECORD_0C) < 0 ||
	    wr_u32(&bb, 7) < 0 ||
	    wr_u8(&bb, 0) < 0 ||
	    wr_u16(&bb, client_version) < 0 ||
	    wr_u16(&bb, 0) < 0 ||
	    wr_u16(&bb, ACC_PROTOCOL_VERSION) < 0 ||
	    wr_u16(&bb, 0) < 0)
		goto fail;

	rc = tcp_send_framed(c->fd, bb.data, bb.wpos);
	bb_free(&bb);
	return rc;
fail:
	bb_free(&bb);
	return -1;
}

/*
 * Send a 0x0b accept response with the welcome trailer.
 * Header: u8(0x0b) + u16(udp_port) + u8(0x12) +
 * u16(nconns) + u16(conn_id) + u16(0).
 */
static int
handshake_send_accept(struct Conn *c, struct Server *s)
{
	struct ByteBuf bb;
	int rc;

	bb_init(&bb);
	if (wr_u8(&bb, SRV_HANDSHAKE_RESPONSE) < 0 ||
	    wr_u16(&bb, (uint16_t)s->udp_port) < 0 ||
	    wr_u8(&bb, 0x12) < 0 ||
	    wr_u16(&bb, 0) < 0 ||	/* nconns placeholder */
	    wr_u16(&bb, s->cars[c->car_id].car_id) < 0 ||
	    wr_u16(&bb, 0) < 0)
		goto fail;

	if (build_welcome_trailer(&bb, s, c) < 0)
		goto fail;

	rc = tcp_send_framed(c->fd, bb.data, bb.wpos);
	bb_free(&bb);
	return rc;
fail:
	bb_free(&bb);
	return -1;
}

int
handshake_handle(struct Server *s, struct Conn *c,
    const unsigned char *body, size_t len)
{
	struct Reader r;
	uint8_t msg_id;
	uint16_t client_version;
	char *password = NULL;
	enum reject_reason reason = REJECT_OK;

	rd_init(&r, body, len);

	if (rd_u8(&r, &msg_id) < 0 || msg_id != ACP_REQUEST_CONNECTION) {
		log_warn("handshake: bad first byte 0x%02x from fd %d",
		    msg_id, c->fd);
		return -1;
	}
	if (rd_u16(&r, &client_version) < 0) {
		log_warn("handshake: short version from fd %d", c->fd);
		return -1;
	}
	if (client_version != ACC_PROTOCOL_VERSION) {
		log_info("rejecting new connection with wrong client "
		    "version %u (server runs %u)",
		    (unsigned)client_version,
		    (unsigned)ACC_PROTOCOL_VERSION);
		reason = REJECT_VERSION;
		goto reply;
	}
	if (rd_str_a(&r, &password) < 0) {
		log_warn("handshake: short password from fd %d", c->fd);
		return -1;
	}
	if (strcmp(password, s->password) != 0) {
		log_info("rejecting connection: bad password from fd %d",
		    c->fd);
		reason = REJECT_PASSWORD;
		goto reply;
	}
	/* nconns already includes this connection (incremented in
	 * conn_new at TCP accept time), so compare with > not >=. */
	if (s->nconns > s->max_connections) {
		log_info("rejecting connection: server full");
		reason = REJECT_FULL;
		goto reply;
	}

	/*
	 * Save the raw handshake body (after password) for echoing
	 * in the welcome trailer.  The Kunos server re-serializes
	 * the parsed fields, but echoing the raw bytes is close
	 * enough for the client to accept.
	 */
	{
		size_t echo_len = rd_remaining(&r);

		c->hs_echo = malloc(echo_len);
		if (c->hs_echo != NULL) {
			memcpy(c->hs_echo, r.p, echo_len);
			c->hs_echo_len = echo_len;
		}
	}

	/*
	 * Parse DriverInfo and CarInfo from the handshake body.
	 *
	 * The real ACC client sends a richer format than simple test
	 * clients: DriverInfo carries 5 strings with has_more()
	 * guards, a 41-byte numeric block, then steam_id; CarInfo
	 * follows with dozens of customization fields.  We detect
	 * the format by packet size (real client ~456 bytes, simple
	 * client ~150 bytes) and parse accordingly.
	 */
	{
		char *first = NULL, *last = NULL, *sname = NULL;
		char *steam = NULL, *team = NULL;
		char *skip_str = NULL;
		char steam_buf[32] = "";
		uint8_t cat = 0;
		uint16_t nat = 0;
		int32_t rnum = 0;
		uint8_t cmodel = 0, ccup = 0;
		struct CarEntry *car;

		if (len > 200) {
			/*
			 * Real client format: 5 DriverInfo strings
			 * (first, aux, last, aux, short), 41-byte
			 * numeric block, steam_id, middle bytes,
			 * then full CarInfo.
			 */
			if (rd_can_str_a(&r))
				(void)rd_str_a(&r, &first);
			if (rd_can_str_a(&r)) {
				(void)rd_str_a(&r, &skip_str);
				free(skip_str); skip_str = NULL;
			}
			if (rd_can_str_a(&r))
				(void)rd_str_a(&r, &last);
			if (rd_can_str_a(&r)) {
				(void)rd_str_a(&r, &skip_str);
				free(skip_str); skip_str = NULL;
			}
			if (rd_can_str_a(&r))
				(void)rd_str_a(&r, &sname);

			/* 41-byte numeric block. */
			if (rd_remaining(&r) >= 41) {
				(void)rd_skip(&r, 16);
				(void)rd_u8(&r, &cat);
				(void)rd_skip(&r, 24);
			}

			/* steam_id (6th string). */
			if (rd_can_str_a(&r) &&
			    rd_str_a(&r, &steam) == 0 && steam != NULL)
				snprintf(steam_buf, sizeof(steam_buf),
				    "%s", steam);

			/* Skip middle bytes, parse CarInfo. */
			(void)rd_skip(&r, 8);
			(void)rd_skip(&r, 4);		/* carModelKey */
			(void)rd_skip(&r, 4);		/* teamGuid */
			(void)rd_i32(&r, &rnum);	/* raceNumber */
			(void)rd_skip(&r, 33);		/* skin fields */
			if (rd_can_str_a(&r)) {		/* customSkinName */
				(void)rd_str_a(&r, &skip_str);
				free(skip_str); skip_str = NULL;
			}
			(void)rd_skip(&r, 1);		/* bannerKey */
			if (rd_can_str_a(&r))		/* teamName */
				(void)rd_str_a(&r, &team);
			(void)rd_u16(&r, &nat);		/* nationality */
			if (rd_can_str_a(&r)) {		/* displayName */
				(void)rd_str_a(&r, &skip_str);
				free(skip_str); skip_str = NULL;
			}
			if (rd_can_str_a(&r)) {		/* competitorName */
				(void)rd_str_a(&r, &skip_str);
				free(skip_str); skip_str = NULL;
			}
			(void)rd_skip(&r, 3);		/* nat + templateKey */
			(void)rd_u8(&r, &cmodel);	/* carModelType */
			(void)rd_u8(&r, &ccup);
		} else {
			/*
			 * Simple format (probe / test client):
			 * 3 strings, u8 cat, u16 nat, steam_id,
			 * i32 rnum, u8 model, u8 cup, str_a team.
			 */
			(void)rd_str_a(&r, &first);
			(void)rd_str_a(&r, &last);
			(void)rd_str_a(&r, &sname);
			(void)rd_u8(&r, &cat);
			(void)rd_u16(&r, &nat);
			if (rd_str_a(&r, &steam) == 0 && steam != NULL)
				snprintf(steam_buf, sizeof(steam_buf),
				    "%s", steam);
			(void)rd_i32(&r, &rnum);
			(void)rd_u8(&r, &cmodel);
			(void)rd_u8(&r, &ccup);
			(void)rd_str_a(&r, &team);
		}

		/* Ban check. */
		if (bans_contains(&s->bans, steam_buf)) {
			log_info("rejecting banned steam_id %s", steam_buf);
			reason = REJECT_BANNED;
			free(first); free(last); free(sname);
			free(steam); free(team);
			goto reply;
		}

		/*
		 * Entry list enforcement: if forceEntryList is set,
		 * look up the client's steam_id in the preloaded
		 * entries. Assign them to the matching slot, or
		 * reject if not found.
		 */
		if (s->force_entry_list) {
			int slot = -1, i;

			for (i = 0; i < ACC_MAX_CARS &&
			    i < s->max_connections; i++) {
				struct CarEntry *ec = &s->cars[i];
				int dj;

				for (dj = 0; dj < ec->driver_count; dj++) {
					if (strcmp(ec->drivers[dj].steam_id,
					    steam_buf) == 0) {
						slot = i;
						break;
					}
				}
				if (slot >= 0)
					break;
			}
			if (slot < 0) {
				log_info("rejecting %s: not in entry list",
				    steam_buf);
				reason = REJECT_BAD_CAR;
				free(first); free(last); free(sname);
				free(steam); free(team);
				goto reply;
			}
			if (s->cars[slot].used) {
				log_info("rejecting %s: entry slot %d "
				    "already in use", steam_buf, slot);
				reason = REJECT_FULL;
				free(first); free(last); free(sname);
				free(steam); free(team);
				goto reply;
			}
			s->cars[slot].used = 1;
			c->car_id = slot;
		} else {
			c->car_id = server_alloc_car(s);
			if (c->car_id < 0) {
				reason = REJECT_FULL;
				free(first); free(last); free(sname);
				free(steam); free(team);
				goto reply;
			}
		}

		/* Populate the car slot with parsed data. */
		car = &s->cars[c->car_id];
		if (first != NULL)
			snprintf(car->drivers[0].first_name,
			    sizeof(car->drivers[0].first_name), "%s",
			    first);
		if (last != NULL)
			snprintf(car->drivers[0].last_name,
			    sizeof(car->drivers[0].last_name), "%s",
			    last);
		if (sname != NULL)
			snprintf(car->drivers[0].short_name,
			    sizeof(car->drivers[0].short_name), "%s",
			    sname);
		car->drivers[0].driver_category = cat;
		car->drivers[0].nationality = nat;
		snprintf(car->drivers[0].steam_id,
		    sizeof(car->drivers[0].steam_id), "%s", steam_buf);
		if (car->driver_count == 0)
			car->driver_count = 1;

		/*
		 * Only override car fields from the handshake if the
		 * entry list did not pre-populate them.
		 */
		if (!s->force_entry_list) {
			car->race_number = rnum;
			car->car_model = cmodel;
			car->cup_category = ccup;
			if (team != NULL)
				snprintf(car->team_name,
				    sizeof(car->team_name), "%s", team);
		}

		free(first);
		free(last);
		free(sname);
		free(steam);
		free(team);
	}

	c->state = CONN_AUTH;
	{
		struct CarEntry *lcar = &s->cars[c->car_id];
		struct DriverInfo *ldrv = &lcar->drivers[0];

		log_info("handshake accepted: fd=%d conn_id=%u car_id=%d "
		    "race#=%d model=%u",
		    c->fd, (unsigned)c->conn_id, c->car_id,
		    lcar->race_number, (unsigned)lcar->car_model);
		log_debug("  driver: \"%s\" \"%s\" [%s] cat=%u steam=%s",
		    ldrv->first_name, ldrv->last_name,
		    ldrv->short_name,
		    (unsigned)ldrv->driver_category, ldrv->steam_id);
	}

reply:
	free(password);
	if (reason != REJECT_OK) {
		log_debug("handshake reject: reason=%d client_ver=0x%04x "
		    "fd=%d", (int)reason, (unsigned)client_version,
		    c->fd);
		if (handshake_send_reject(c, client_version) < 0)
			return -1;
		return -1;	/* close connection after reject */
	}
	if (handshake_send_accept(c, s) < 0)
		return -1;
	log_debug("handshake accept sent: conn=%u udp_port=%d",
	    (unsigned)c->conn_id, s->udp_port);

	/*
	 * After a successful accept, fan out 0x2e new-client-
	 * joined notify to every OTHER already-connected client.
	 * This lets them add the joining car to their local entry
	 * list and display it in the lobby.  The binary also emits
	 * a paired 0x4f sub-opcode 1 message right after; we do
	 * the same.
	 */
	{
		struct ByteBuf notify;
		uint64_t timestamp_ms;
		struct timespec ts;

		clock_gettime(CLOCK_MONOTONIC, &ts);
		timestamp_ms = (uint64_t)ts.tv_sec * 1000ull +
		    (uint64_t)ts.tv_nsec / 1000000ull;

		bb_init(&notify);
		if (wr_u8(&notify, SRV_CAR_SYSTEM_RELAY) == 0 &&
		    wr_u16(&notify, (uint16_t)c->car_id) == 0 &&
		    wr_u64(&notify, timestamp_ms) == 0)
			(void)bcast_all(s, notify.data, notify.wpos,
			    c->conn_id);
		bb_free(&notify);

		/* Paired 0x4f sub-opcode 1: u8 msg_id + u16 carId +
		 * u8 sub=1 + u64 timestamp (12 bytes). */
		bb_init(&notify);
		if (wr_u8(&notify, SRV_DRIVER_STINT_RELAY) == 0 &&
		    wr_u16(&notify, (uint16_t)c->car_id) == 0 &&
		    wr_u8(&notify, 1) == 0 &&
		    wr_u64(&notify, timestamp_ms) == 0)
			(void)bcast_all(s, notify.data, notify.wpos,
			    c->conn_id);
		bb_free(&notify);

		/*
		 * Post-accept welcome sequence matching the real
		 * server order: 0x28 + 0x36 + 0x37.
		 */
		{
			struct ByteBuf wb;

			/* 0x28 SRV_LARGE_STATE_RESPONSE: session
			 * timing + assist rule snapshot.  Each f32
			 * value is prefixed by u8(1). */
			bb_init(&wb);
			if (wr_u8(&wb, SRV_LARGE_STATE_RESPONSE) == 0 &&
			    wr_u8(&wb, 0) == 0) {
				int k;
				float grip = s->session.grip_level > 0
				    ? s->session.grip_level : 1.0f;

				/* 3 copies of session time as f32. */
				for (k = 0; k < 3; k++) {
					(void)wr_u8(&wb, 1);
					(void)wr_f32(&wb,
					    (float)s->session.weekend_time_s);
				}
				/* 3 copies of end time. */
				for (k = 0; k < 3; k++) {
					(void)wr_u8(&wb, 1);
					(void)wr_f32(&wb,
					    (float)(s->sessions[0].duration_min
					    * 60));
				}
				(void)wr_u8(&wb, 0);
				(void)wr_u8(&wb, 6);
				(void)wr_u8(&wb, 0);
				(void)wr_u8(&wb, 1);
				(void)wr_f32(&wb, grip);
				(void)wr_u16(&wb, 3);
				(void)wr_u16(&wb, 600);
				(void)wr_u8(&wb, 0);
				(void)wr_u8(&wb, 0);
				(void)wr_u16(&wb, 120);
				(void)wr_u8(&wb, 0);
				(void)wr_u8(&wb, 0);
				(void)wr_u8(&wb, 0);
				(void)wr_u8(&wb, 0);
				(void)wr_f32(&wb, grip);
				(void)bcast_send_one(c, wb.data, wb.wpos);
			}
			bb_free(&wb);

			/* 0x36 initial leaderboard snapshot. */
			{
				struct ByteBuf lb;
				int j, nc = 0;

				bb_init(&lb);
				if (wr_u8(&lb, SRV_LEADERBOARD_BCAST) == 0 &&
				    wr_u32(&lb, s->session.standings_seq) == 0) {
					(void)wr_u8(&lb, 3);
					(void)wr_i32(&lb, 0x7FFFFFFF);
					(void)wr_i32(&lb, 0x7FFFFFFF);
					(void)wr_i32(&lb, 0x7FFFFFFF);
					(void)wr_u8(&lb, 0);

					for (j = 0; j < ACC_MAX_CARS &&
					    j < s->max_connections; j++)
						if (s->cars[j].used) nc++;
					(void)wr_u8(&lb, (uint8_t)nc);

					for (j = 0; j < ACC_MAX_CARS &&
					    j < s->max_connections; j++) {
						struct CarEntry *ec =
						    &s->cars[j];

						if (!ec->used) continue;
						(void)wr_u8(&lb, 0);
						(void)wr_u16(&lb,
						    ec->car_id);
						(void)wr_u16(&lb,
						    (uint16_t)ec->race_number);
						(void)wr_u8(&lb,
						    ec->car_model);
						(void)wr_u8(&lb,
						    ec->cup_category);
						(void)wr_u32(&lb, 0);
						(void)wr_u8(&lb,
						    ec->driver_count);
						(void)wr_str_a(&lb,
						    ec->drivers[0].steam_id);
						(void)wr_str_a(&lb,
						    ec->drivers[0].short_name);
						(void)wr_str_a(&lb,
						    ec->drivers[0].first_name);
						(void)wr_str_a(&lb,
						    ec->drivers[0].last_name);
						(void)wr_u16(&lb, 0);
						(void)wr_u8(&lb, 0);
						(void)wr_i32(&lb, 0x7FFFFFFF);
						(void)wr_i32(&lb, 0x7FFFFFFF);
						(void)wr_u8(&lb, 0);
						(void)wr_i32(&lb, 0x7FFFFFFF);
						(void)wr_u8(&lb, 1);
						(void)wr_u8(&lb, 0);
						(void)wr_u8(&lb, 3);
						(void)wr_i32(&lb, 0x7FFFFFFF);
						(void)wr_i32(&lb, 0x7FFFFFFF);
						(void)wr_i32(&lb, 0x7FFFFFFF);
						(void)wr_u32(&lb, 0);
						(void)wr_u32(&lb, 0);
					}
					(void)bcast_send_one(c, lb.data,
					    lb.wpos);
				}
				bb_free(&lb);
			}

			/* 0x37 weather status. */
			bb_init(&wb);
			if (weather_build_broadcast(s, &wb) == 0)
				(void)bcast_send_one(c, wb.data, wb.wpos);
			bb_free(&wb);

			/* 0x4e rating summary. */
			bb_init(&wb);
			if (wr_u8(&wb, SRV_RATING_SUMMARY) == 0 &&
			    wr_u8(&wb, 1) == 0 &&
			    wr_u16(&wb, c->conn_id) == 0 &&
			    wr_u8(&wb, 0) == 0 &&
			    wr_i16(&wb, 0) == 0 &&
			    wr_i16(&wb, 0) == 0 &&
			    wr_u32(&wb, 0xFFFFFFFF) == 0 &&
			    wr_str_a(&wb,
				s->cars[c->car_id].drivers[0].steam_id) == 0)
				(void)bcast_send_one(c, wb.data, wb.wpos);
			bb_free(&wb);
		}

		log_debug("welcome sequence sent: 0x2e+0x4f bcast + "
		    "0x28+0x36+0x37+0x4e to conn=%u",
		    (unsigned)c->conn_id);
	}
	return 0;
}
