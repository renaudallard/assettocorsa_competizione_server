/*
 * state.h -- per-connection and per-car / global server state.
 *
 * Modeled on what the binary stores in its server state struct,
 * but flat and minimal.  Just enough to support phase 1 (handshake)
 * and to be a credible foundation for later phases.
 */

#ifndef ACCD_STATE_H
#define ACCD_STATE_H

#include <stddef.h>
#include <stdint.h>
#include <netinet/in.h>

#include "io.h"
#include "msg.h"

#define ACC_MAX_CARS		64
#define ACC_MAX_DRIVERS_PER_CAR	4
#define ACC_MAX_NAME_LEN	64
#define ACC_TRACK_NAME_LEN	48

/*
 * Per-driver record loaded from entrylist.json (and / or sent by
 * the client during handshake).
 */
struct DriverInfo {
	char		first_name[ACC_MAX_NAME_LEN];
	char		last_name[ACC_MAX_NAME_LEN];
	char		short_name[8];
	uint8_t		driver_category;	/* Bronze=0..Platinum=3 */
	uint16_t	nationality;		/* see SDK enum */
	char		steam_id[32];
};

/*
 * Per-car record (entry list slot).
 */
struct CarEntry {
	uint16_t	car_id;			/* assigned by server */
	int32_t		race_number;
	uint8_t		car_model;		/* see HB §IX.3 */
	uint8_t		cup_category;
	uint16_t	nationality;
	char		team_name[ACC_MAX_NAME_LEN];
	int32_t		default_grid_position;	/* -1 = unset */
	uint8_t		ballast_kg;		/* clamped 0..40 */
	float		restrictor;		/* normalized 0..0.99 */
	uint8_t		current_driver_index;
	uint8_t		driver_count;
	struct DriverInfo drivers[ACC_MAX_DRIVERS_PER_CAR];
	int		used;			/* slot occupied? */
};

/*
 * Per-connection state.  One of these per accepted TCP socket.
 */
struct Conn {
	int		fd;
	struct sockaddr_in
			peer;
	enum conn_state	state;
	uint16_t	conn_id;	/* server-assigned, also "carIndex" */
	int32_t		car_id;		/* index into server.cars[], -1 if spectator */
	int		is_admin;
	int		is_spectator;
	struct ByteBuf	rx;		/* incoming TCP byte stream */
	struct ByteBuf	tx;		/* not yet used; for batched sends */
};

/*
 * Global server state.
 */
struct Server {
	/* config */
	int		tcp_port;
	int		udp_port;
	int		max_connections;
	int		lan_discovery;
	char		server_name[ACC_MAX_NAME_LEN];
	char		password[64];
	char		admin_password[64];
	char		spectator_password[64];
	char		track[ACC_TRACK_NAME_LEN];
	int		ignore_premature_disconnects;
	int		dump_leaderboards;

	/* runtime */
	int		tcp_fd;
	int		udp_fd;
	int		lan_fd;
	struct Conn	*conns[ACC_MAX_CARS];	/* indexed by conn_id */
	int		nconns;

	struct CarEntry	cars[ACC_MAX_CARS];

	/* timing */
	uint64_t	tick_count;
	uint64_t	session_start_ms;
};

void	server_init(struct Server *s);
void	server_free(struct Server *s);

/* Allocate a new Conn for an accepted fd.  Returns NULL on full. */
struct Conn *
	conn_new(struct Server *s, int fd, const struct sockaddr_in *peer);

/* Drop a connection: close the fd, free the buffers, slot returns
 * to the free pool. */
void	conn_drop(struct Server *s, struct Conn *c);

/* Find a connection by its (server-assigned) conn_id. */
struct Conn *
	server_find_conn(struct Server *s, uint16_t conn_id);

/*
 * Allocate a free CarEntry slot and return its index, or -1 if
 * the entry list is full.  car_id matches the index by design.
 */
int	server_alloc_car(struct Server *s);

#endif /* ACCD_STATE_H */
