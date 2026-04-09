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
 * handlers.h -- per-msg-id handlers called from the TCP dispatcher.
 *
 * Each handler function takes the full framed message body (with
 * the msg id byte already verified, but still included at body[0])
 * and the connection that sent it.  Returns 0 on success, -1 to
 * drop the connection.
 */

#ifndef ACCD_HANDLERS_H
#define ACCD_HANDLERS_H

#include <stddef.h>

#include "state.h"

/* 0x19 ACP_LAP_COMPLETED -> broadcast 0x1b */
int	h_lap_completed(struct Server *s, struct Conn *c,
		const unsigned char *body, size_t len);

/* 0x20 ACP_SECTOR_SPLIT (bulk) -> broadcast 0x3a */
int	h_sector_split_bulk(struct Server *s, struct Conn *c,
		const unsigned char *body, size_t len);

/* 0x21 ACP_SECTOR_SPLIT (single) -> broadcast 0x3b */
int	h_sector_split_single(struct Server *s, struct Conn *c,
		const unsigned char *body, size_t len);

/* 0x2a ACP_CHAT -> chat_process + broadcast 0x2b */
int	h_chat(struct Server *s, struct Conn *c,
		const unsigned char *body, size_t len);

/* 0x2e ACP_CAR_SYSTEM_UPDATE -> broadcast 0x2e */
int	h_car_system_update(struct Server *s, struct Conn *c,
		const unsigned char *body, size_t len);

/* 0x2f ACP_TYRE_COMPOUND_UPDATE -> broadcast 0x2f */
int	h_tyre_compound_update(struct Server *s, struct Conn *c,
		const unsigned char *body, size_t len);

/* 0x32 ACP_CAR_LOCATION_UPDATE -> broadcast 0x32 */
int	h_car_location_update(struct Server *s, struct Conn *c,
		const unsigned char *body, size_t len);

/* 0x3d ACP_OUT_OF_TRACK -> broadcast 0x3c */
int	h_out_of_track(struct Server *s, struct Conn *c,
		const unsigned char *body, size_t len);

/* 0x41 ACP_REPORT_PENALTY -- log + state update only */
int	h_report_penalty(struct Server *s, struct Conn *c,
		const unsigned char *body, size_t len);

/* 0x42 ACP_LAP_TICK -- log + timing state update only */
int	h_lap_tick(struct Server *s, struct Conn *c,
		const unsigned char *body, size_t len);

/* 0x43 ACP_DAMAGE_ZONES_UPDATE -> broadcast 0x44 */
int	h_damage_zones(struct Server *s, struct Conn *c,
		const unsigned char *body, size_t len);

/* 0x45 ACP_CAR_DIRT_UPDATE -> broadcast 0x46 */
int	h_car_dirt(struct Server *s, struct Conn *c,
		const unsigned char *body, size_t len);

/* 0x47 ACP_UPDATE_DRIVER_SWAP_STATE */
int	h_update_driver_swap_state(struct Server *s, struct Conn *c,
		const unsigned char *body, size_t len);

/* 0x48 ACP_EXECUTE_DRIVER_SWAP -> reply 0x49, maybe broadcast 0x58 */
int	h_execute_driver_swap(struct Server *s, struct Conn *c,
		const unsigned char *body, size_t len);

/* 0x4a ACP_DRIVER_SWAP_STATE_REQUEST */
int	h_driver_swap_state_request(struct Server *s, struct Conn *c,
		const unsigned char *body, size_t len);

/* 0x4f ACP_DRIVER_STINT_RESET */
int	h_driver_stint_reset(struct Server *s, struct Conn *c,
		const unsigned char *body, size_t len);

/* 0x51 ACP_ELO_UPDATE */
int	h_elo_update(struct Server *s, struct Conn *c,
		const unsigned char *body, size_t len);

/* 0x54 ACP_MANDATORY_PITSTOP_SERVED */
int	h_mandatory_pitstop_served(struct Server *s, struct Conn *c,
		const unsigned char *body, size_t len);

/* 0x55 ACP_LOAD_SETUP -> reply 0x56 */
int	h_load_setup(struct Server *s, struct Conn *c,
		const unsigned char *body, size_t len);

/* 0x5b ACP_CTRL_INFO */
int	h_ctrl_info(struct Server *s, struct Conn *c,
		const unsigned char *body, size_t len);

/* UDP 0x1e ACP_CAR_UPDATE -- parses into per-car state */
int	h_udp_car_update(struct Server *s, struct Conn *c,
		const unsigned char *body, size_t len);

/* UDP 0x22 CAR_INFO_REQUEST -> reply 0x23 */
int	h_udp_car_info_request(struct Server *s,
		const unsigned char *body, size_t len);

#endif /* ACCD_HANDLERS_H */
