/*
 * bot.c — driving bot for accd / accServer.exe.
 *
 * Connects via TCP, runs the ACP_REQUEST_CONNECTION (0x09) handshake,
 * then drives a real-track polyline at ~30 Hz on UDP.  Models:
 *   - formation lap (lap 0 at <70 km/h, accelerates after lap wrap)
 *   - pit-stop with sub-22 m/s velocity + location=Pitlane (so the
 *     server's pit-speeding DQ never fires)
 *   - racing line loaded from a CSV waypoint file (norm_pos x y z),
 *     defaulting to a stadium-shape loop if no file is supplied
 *
 * Build:   cc -O2 -Wall -o bot bot.c -lm
 * Usage:   ./bot --host H --tcp P [--race N] [--name S] [--track FILE]
 *                [--length M] [--pit-on-lap N] [--laps N]
 *
 * Not part of the project's build system; lives under tmp/bot/ for
 * server stress / multi-car relay validation.
 */

#define _POSIX_C_SOURCE 200809L
#define _USE_MATH_DEFINES
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <math.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define TICK_MS			33		/* ~30 Hz */
#define V_FORMATION		16.0f		/* 57.6 km/h, under 70 limit */
#define V_RACE			85.0f		/* 306 km/h, GT3 top speed */
#define V_PITLANE		18.0f		/* 64.8 km/h, under 80 limit */
#define DEFAULT_LENGTH_M	4000.0f
#define MAX_WAYPOINTS		2048	/* monza: 1152, brands_hatch: 777 */

/*
 * Kinematic physics model — see compute_corner_radius() and the
 * main loop's speed update.  GT3-class car with separable tyre /
 * aero / wear contributions to effective µ:
 *
 *   µ_dry      = base mechanical grip (fresh tyres, no aero)
 *   µ_aero(v)  = µ_dry + K_AERO · v²   (downforce; ~+0.5 at top sp)
 *   µ_eff(v,t) = µ_aero(v) · wear_factor(t)   (linear wear with time)
 *
 *   accel = 8 m/s² (out of corner; constant — kinematic approx)
 *   brake = 25 m/s² (≈ 2.5 g, GT3 dry-track typical)
 */
#define MU_DRY			1.0f
#define K_AERO			6.0e-5f		/* µ adds ≈ 0.43 at 85 m/s */
#define G_ACCEL			9.81f
#define A_ACCEL			8.0f
#define A_BRAKE			25.0f
#define BRAKE_LOOKAHEAD_S	4.0f
#define TYRE_WEAR_PER_MIN	0.005f		/* 30 % grip loss after 60' */
#define TYRE_WEAR_FLOOR		0.7f		/* µ never drops below 70 % */
#define LATERAL_RECOVERY_TAU_S	1.8f		/* time const for bump back */

#define LOC_NONE	0
#define LOC_TRACK	1
#define LOC_PITLANE	2
#define LOC_PITENTRY	3
#define LOC_PITEXIT	4

static volatile sig_atomic_t g_stop;
static void on_sigint(int s) { (void)s; g_stop = 1; }

static uint32_t mono_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)((uint64_t)ts.tv_sec * 1000 +
	    (uint64_t)ts.tv_nsec / 1000000);
}

/* ------------------------------------------------------------------ */
/* tiny byte buffer */
static uint8_t bb_buf[8192];
static size_t bb_n;
static void bb_reset(void) { bb_n = 0; }
static void bb_u8(uint8_t v) { bb_buf[bb_n++] = v; }
static void bb_u16(uint16_t v)
{
	bb_buf[bb_n++] = v & 0xff;
	bb_buf[bb_n++] = (v >> 8) & 0xff;
}
static void bb_u32(uint32_t v)
{
	bb_buf[bb_n++] = v & 0xff;
	bb_buf[bb_n++] = (v >> 8) & 0xff;
	bb_buf[bb_n++] = (v >> 16) & 0xff;
	bb_buf[bb_n++] = (v >> 24) & 0xff;
}
static void bb_f32(float f) { uint32_t u; memcpy(&u, &f, 4); bb_u32(u); }
static void bb_pad(size_t n) { memset(bb_buf + bb_n, 0, n); bb_n += n; }

/* Format-A wstring: u8 count + count * u32 codepoint. */
static void bb_fmta(const char *s)
{
	size_t i, n = strlen(s);
	bb_u8((uint8_t)n);
	for (i = 0; i < n; i++)
		bb_u32((uint8_t)s[i]);
}

/* ------------------------------------------------------------------ */
/* DriverInfo + CarInfo bodies — match fake_client.py byte-for-byte. */

static void build_driver_info(const char *first, const char *last,
    const char *shortn, const char *steam, uint8_t driver_cat)
{
	bb_fmta(first);
	bb_fmta("");
	bb_fmta(last);
	bb_fmta("");
	bb_fmta(shortn);
	bb_u8(1);
	bb_u16(0);
	bb_u8(0);
	bb_u32(0x1f7); bb_u32(0x11); bb_u32(0xf3);
	bb_u8(driver_cat);		/* byte 16 of numeric block = driver_category */
	bb_u32(0); bb_u32(0);
	bb_u32(200); bb_u32(0x1f8); bb_u32(0xf3); bb_u32(0x155);
	bb_fmta(steam);
}

static void build_car_info(uint8_t car_model, uint32_t race_number)
{
	/*
	 * accd reads i32 raceNumber at CarInfo +0x08 (handshake.c
	 * real-client parser at line 1850: skip 8 + 8, then rd_i32).
	 * carModelType lives at +0xf0 (read at line 1869 after the
	 * string skips), so put race_number in the +0x08 slot and
	 * leave a filler 0 where the old car_model field was — the
	 * +0xf0 byte below still carries the real model.
	 */
	bb_u32(0); bb_u32(0); bb_u32(race_number);
	bb_u8(0); bb_u8(0);
	bb_u32(0);
	bb_u8(0);
	bb_u32(0); bb_u32(0); bb_u32(0);
	bb_u8(0); bb_u8(0); bb_u8(0); bb_u8(0);
	bb_u32(0);	/* was race_number at +0x23, now redundant filler */
	bb_u32(0);
	bb_u8(0); bb_u8(0);
	bb_fmta("");
	bb_u8(0);
	bb_fmta("BotTeam");
	bb_u16(0);
	bb_fmta("");
	bb_fmta("");
	bb_u16(0);
	bb_u8(0);				/* +0xca */
	bb_u8(car_model);			/* +0xf0 carModelType */
	bb_u8(0);				/* +0xf1 cup */
	bb_u8(0);				/* +0xf2 */
	bb_u8(0); bb_u8(0); bb_u8(0);		/* +0xf4..+0xf6 trailing bools */
}

static int build_handshake(const char *first, const char *last,
    const char *shortn, const char *steam, uint32_t race_number,
    uint8_t driver_cat, uint16_t client_version, const char *password,
    uint8_t *out, size_t *out_len)
{
	uint8_t body[2048];
	size_t blen;

	bb_reset();
	bb_u8(0x09);
	bb_u16(client_version);
	bb_fmta(password);
	build_driver_info(first, last, shortn, steam, driver_cat);
	bb_pad(8);
	build_car_info(35, race_number);
	while (bb_n <= 200)
		bb_u8(0);
	blen = bb_n;
	memcpy(body, bb_buf, blen);

	if (*out_len < blen + 2)
		return -1;
	out[0] = blen & 0xff;
	out[1] = (blen >> 8) & 0xff;
	memcpy(out + 2, body, blen);
	*out_len = blen + 2;
	return 0;
}

/* ------------------------------------------------------------------ */
/* TCP frame I/O */

static int tcp_connect(const char *host, uint16_t port)
{
	struct sockaddr_in sa;
	int fd;
	memset(&sa, 0, sizeof sa);
	sa.sin_family = AF_INET;
	sa.sin_port = htons(port);
	if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) {
		struct hostent *he = gethostbyname(host);
		if (!he) { fprintf(stderr, "bad host\n"); return -1; }
		memcpy(&sa.sin_addr, he->h_addr_list[0], sizeof sa.sin_addr);
	}
	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) return -1;
	if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
		perror("connect");
		close(fd);
		return -1;
	}
	return fd;
}

static int recv_n(int fd, void *p, size_t n)
{
	uint8_t *q = p;
	while (n) {
		ssize_t r = recv(fd, q, n, 0);
		if (r <= 0) return -1;
		q += r;
		n -= r;
	}
	return 0;
}

static int recv_frame(int fd, uint8_t *out, size_t cap, size_t *got)
{
	uint8_t lh[2];
	uint16_t ln;
	if (recv_n(fd, lh, 2) < 0) return -1;
	ln = lh[0] | (lh[1] << 8);
	if (ln > cap) return -1;
	if (recv_n(fd, out, ln) < 0) return -1;
	*got = ln;
	return 0;
}

/* ------------------------------------------------------------------ */
/* UDP packet builders */

static size_t pkt_keepalive(uint8_t *out, uint16_t conn_id)
{
	bb_reset();
	bb_u8(0x13);
	bb_u16(conn_id);
	memcpy(out, bb_buf, bb_n);
	return bb_n;
}

static size_t pkt_pong(uint8_t *out, uint16_t conn_id,
    uint32_t srv_ts_echo, uint32_t client_ts)
{
	bb_reset();
	bb_u8(0x16);
	bb_u16(conn_id);
	bb_u32(srv_ts_echo);
	bb_u32(client_ts);
	memcpy(out, bb_buf, bb_n);
	return bb_n;
}

static size_t pkt_location(uint8_t *out, uint16_t car_id, uint8_t loc)
{
	bb_reset();
	bb_u8(0x32);
	bb_u16(car_id);
	bb_u8(loc);
	memcpy(out, bb_buf, bb_n);
	return bb_n;
}

/* 0x21 ACP_SECTOR_SPLIT_SINGLE — TCP framed.
 * Body: u8 + i32 split_ms + i32 lap_ms + u8 sector + u16 car_field +
 *       u8 flag.  Server transforms to 0x3b for relay. */
static size_t pkt_sector_split(uint8_t *out, int32_t split_ms,
    int32_t lap_ms, uint8_t sector, uint16_t car_field)
{
	bb_reset();
	bb_u8(0x21);
	bb_u32((uint32_t)split_ms);
	bb_u32((uint32_t)lap_ms);
	bb_u8(sector);
	bb_u16(car_field);
	bb_u8(0);			/* flag_d, unused */
	memcpy(out, bb_buf, bb_n);
	return bb_n;
}

/* 0x20 ACP_SECTOR_SPLIT (bulk / lap-complete) — TCP framed.
 * Body: u8 + i32 sector_time_ms + u8 sector_index + i32 clock_ms +
 *       u16 car_field.  This is the kunos-side "lap completed"
 *       trigger: accd's h_sector_split_bulk increments lap_count,
 *       updates best_lap_ms, and fires lobby_notify_lap.  Sent
 *       only at sector_index=2 (S/F crossing) once per lap. */
static size_t pkt_sector_bulk(uint8_t *out, int32_t sector_time_ms,
    uint8_t sector_index, int32_t clock_ms, uint16_t car_field)
{
	bb_reset();
	bb_u8(0x20);
	bb_u32((uint32_t)sector_time_ms);
	bb_u8(sector_index);
	bb_u32((uint32_t)clock_ms);
	bb_u16(car_field);
	memcpy(out, bb_buf, bb_n);
	return bb_n;
}

/* 0x54 ACP_MANDATORY_PITSTOP_SERVED — TCP framed.
 * Body: u8 + u16 car_id. */
static size_t pkt_mandatory_pit_served(uint8_t *out, uint16_t car_id)
{
	bb_reset();
	bb_u8(0x54);
	bb_u16(car_id);
	memcpy(out, bb_buf, bb_n);
	return bb_n;
}

/* 0x42 ACP_PENALTY_CLEARED — TCP framed.
 * Body: u8 0x42 + u64 ts.  Emitted by the real AC2 client engine when
 * it decides its dwell on a DT/SG penalty was sufficient; kunos's
 * FUN_140126b50 removes the front DT/SG queue entry on receipt. */
static size_t pkt_penalty_cleared(uint8_t *out, uint64_t ts_ms)
{
	bb_reset();
	bb_u8(0x42);
	bb_u32((uint32_t)ts_ms);
	bb_u32((uint32_t)(ts_ms >> 32));
	memcpy(out, bb_buf, bb_n);
	return bb_n;
}

/* 0x4f ACP_DRIVER_STINT_RESET — TCP framed.
 * Body: u8 0x4f + u8 force + u64 ts.  Server relays as
 * `0x4f + u16 car_id + u8 force [+ u64 ts if force=1]`.  force=0 is
 * voluntary (ESC-to-garage / tow); force=1 is forced (e.g. server-
 * initiated swap demand). */
static size_t pkt_driver_stint_reset(uint8_t *out, uint8_t force,
    uint64_t ts_ms)
{
	bb_reset();
	bb_u8(0x4f);
	bb_u8(force);
	bb_u32((uint32_t)ts_ms);
	bb_u32((uint32_t)(ts_ms >> 32));
	memcpy(out, bb_buf, bb_n);
	return bb_n;
}

/* 0x55 ACP_LOAD_SETUP — TCP framed.
 * Body: u8 0x55 + u8 sess_type + u16 car_id + u32 revision.
 * Server replies with 0x56 SRV_SETUP_DATA_RESPONSE carrying the
 * car's lap history for the requested session. */
static size_t pkt_load_setup(uint8_t *out, uint8_t sess_type,
    uint16_t car_id, uint32_t revision)
{
	bb_reset();
	bb_u8(0x55);
	bb_u8(sess_type);
	bb_u16(car_id);
	bb_u32(revision);
	memcpy(out, bb_buf, bb_n);
	return bb_n;
}

/* 0x4a ACP_DRIVER_SWAP_STATE_REQUEST — TCP framed.
 * Body: u8 0x4a + u16 car_id + u8 sub_state + u8 conn_state.
 * sub_state 2 = initiate, 3 = confirm, 4 = execute.
 * Server applies + broadcasts SRV_DRIVER_SWAP_STATE_BCAST (0x47). */
static size_t pkt_swap_state_request(uint8_t *out, uint16_t car_id,
    uint8_t sub_state, uint8_t conn_state)
{
	bb_reset();
	bb_u8(0x4a);
	bb_u16(car_id);
	bb_u8(sub_state);
	bb_u8(conn_state);
	memcpy(out, bb_buf, bb_n);
	return bb_n;
}

/* 0x47 ACP_UPDATE_DRIVER_SWAP_STATE — TCP framed.
 * Body: u8 0x47 + u16 car_id + u8 driver_count + driver_count × u8 state.
 * Server broadcasts SRV_DRIVER_SWAP_STATE_BCAST (also 0x47) with the
 * same body shape to every conn. */
static size_t pkt_update_swap_state(uint8_t *out, uint16_t car_id,
    const uint8_t *states, uint8_t dcnt)
{
	uint8_t i;
	bb_reset();
	bb_u8(0x47);
	bb_u16(car_id);
	bb_u8(dcnt);
	for (i = 0; i < dcnt; i++)
		bb_u8(states[i]);
	memcpy(out, bb_buf, bb_n);
	return bb_n;
}

/* 0x41 ACP_REPORT_PENALTY — TCP framed.
 * Body: u8 + u8 cat + u8 kind + u64 ts_ms + i32 value.  Used by
 * the diff-test flag --report-penalty. */
static size_t pkt_report_penalty(uint8_t *out, uint8_t cat, uint8_t kind,
    uint64_t ts_ms, int32_t value)
{
	bb_reset();
	bb_u8(0x41);
	bb_u8(cat);
	bb_u8(kind);
	bb_u32((uint32_t)ts_ms);
	bb_u32((uint32_t)(ts_ms >> 32));
	bb_u32((uint32_t)value);
	memcpy(out, bb_buf, bb_n);
	return bb_n;
}

/* 0x2a ACP_CHAT — TCP framed.
 * Body: u8 0x2a + str_a text + i32 client_ts.  The AC2 client
 * doesn't put a sender field on the wire — the server resolves
 * the sender from the connection's car_id.  Sender arg here is
 * kept for the log line in the caller only. */
static size_t pkt_chat(uint8_t *out, const char *sender, const char *text,
    uint32_t client_ts)
{
	(void)sender;
	bb_reset();
	bb_u8(0x2a);
	bb_fmta(text);
	bb_u32(client_ts);
	memcpy(out, bb_buf, bb_n);
	return bb_n;
}

/* 0x2f ACP_TYRE_COMPOUND_UPDATE — TCP framed.
 * Body: u8 + u16 car_id + u8 compound (0=dry, 1=wet). */
static size_t pkt_tyre_compound(uint8_t *out, uint16_t car_id,
    uint8_t compound)
{
	bb_reset();
	bb_u8(0x2f);
	bb_u16(car_id);
	bb_u8(compound);
	memcpy(out, bb_buf, bb_n);
	return bb_n;
}

/* 0x43 ACP_DAMAGE_ZONES_UPDATE — TCP framed.
 * Body: u8 + 5 × u8 zone (front, rear, left, right, centre).  No
 * car_id field — the server uses the conn's c->car_id. */
static size_t pkt_damage_zones(uint8_t *out, const uint8_t zones[5])
{
	bb_reset();
	bb_u8(0x43);
	bb_u8(zones[0]); bb_u8(zones[1]); bb_u8(zones[2]);
	bb_u8(zones[3]); bb_u8(zones[4]);
	memcpy(out, bb_buf, bb_n);
	return bb_n;
}

/* 0x46 ACP_CAR_DIRT_UPDATE — TCP framed.
 * Body: u8 + 5 × u8 dirt-level per tyre.  Server stores but doesn't
 * relay (per Kunos pcap); the welcome trailer carries dirt for late
 * joiners. */
static size_t pkt_car_dirt(uint8_t *out, const uint8_t dirt[5])
{
	bb_reset();
	bb_u8(0x45);  /* ACP_CAR_DIRT_UPDATE -- 0x46 is the server relay id */
	bb_u8(dirt[0]); bb_u8(dirt[1]); bb_u8(dirt[2]);
	bb_u8(dirt[3]); bb_u8(dirt[4]);
	memcpy(out, bb_buf, bb_n);
	return bb_n;
}

/* 0x19 ACP_LAP_COMPLETED (SA contact) — TCP framed.
 * Body: u8 0x19 + u16 reporter + u16 target + i32 ts + u8 quality. */
static size_t pkt_sa_contact(uint8_t *out, uint16_t reporter, uint16_t target,
    int32_t ts, uint8_t quality)
{
	bb_reset();
	bb_u8(0x19);
	bb_u16(reporter);
	bb_u16(target);
	bb_u32((uint32_t)ts);
	bb_u8(quality);
	memcpy(out, bb_buf, bb_n);
	return bb_n;
}

/* 0x3d ACP_OUT_OF_TRACK — TCP framed.
 * Body: u8 0x3d + u8 force + i32 ts. */
static size_t pkt_out_of_track(uint8_t *out, uint8_t force, int32_t ts)
{
	bb_reset();
	bb_u8(0x3d);
	bb_u8(force);
	bb_u32((uint32_t)ts);
	memcpy(out, bb_buf, bb_n);
	return bb_n;
}

/* 0x5e ACP_TIME_EVENT — UDP, not framed.
 * Body: u8 0x5e + u16 source_conn + u16 target_conn + u64 latency + u8 chat. */
static size_t pkt_time_event(uint8_t *out, uint16_t src, uint16_t dst,
    uint64_t latency_ms, uint8_t enable_chat)
{
	bb_reset();
	bb_u8(0x5e);
	bb_u16(src);
	bb_u16(dst);
	bb_u32((uint32_t)(latency_ms & 0xffffffffu));
	bb_u32((uint32_t)(latency_ms >> 32));
	bb_u8(enable_chat);
	memcpy(out, bb_buf, bb_n);
	return bb_n;
}

/* TCP-frame helper: prepend u16 length and send via the TCP socket. */
static void send_tcp_framed(int tcp_fd, const uint8_t *body, size_t n)
{
	uint8_t buf[280];
	if (n + 2 > sizeof buf) return;
	buf[0] = (uint8_t)(n & 0xff);
	buf[1] = (uint8_t)((n >> 8) & 0xff);
	memcpy(buf + 2, body, n);
	(void)send(tcp_fd, buf, n + 2, MSG_NOSIGNAL);
}

/*
 * Per-tick physics snapshot the bot feeds into pkt_car_update so the
 * 8 input bytes (input_a + wheel_slip) and the RPM / gear / fuel /
 * damage bytes carry content-varying values instead of all-zero.
 * `zero` opts back into the legacy all-zero behaviour for byte-diff
 * tests that compare against a kunos pcap captured under similar
 * "stationary input" conditions.
 */
struct CarInputs {
	int      zero;        /* 1 = emit legacy all-zero inputs */
	float    v_current;   /* m/s */
	float    v_target;    /* m/s */
	float    yaw_delta;   /* radians since last tick */
	uint8_t  gear;        /* 0 = neutral, 1.. = N+1 */
	uint8_t  fuel;        /* 0..255 (255 = full tank) */
	uint8_t  damage;      /* 0..255 (0 = pristine) */
	uint16_t rpm;         /* 1..65000 */
};

/*
 * 0x1e ACP_CAR_UPDATE — 68 bytes.
 * scalar_44 (offset 57) is reinterpreted as f32 by accd to read the
 * normalized track position used for formation/green triggers.
 */
static size_t pkt_car_update(uint8_t *out, uint16_t conn_id,
    uint16_t car_id, uint8_t seq, uint32_t client_ts_ms,
    float pos_x, float pos_y, float pos_z,
    float vel_x, float vel_y, float vel_z,
    float norm_pos, const struct CarInputs *in)
{
	uint8_t steering = 0, throttle = 0, brake = 0, slip = 0, lat_g = 127;
	uint8_t gear = 0, fuel = 0, damage = 0;
	uint16_t rpm = 0;

	if (in != NULL && !in->zero) {
		float dv = in->v_target - in->v_current;
		float s;
		int t, b, st;

		/*
		 * Steering: yaw_delta in radians, mapped onto signed-
		 * biased u8 with full-lock at ~0.05 rad / tick (~15
		 * deg/s on a real car, plenty for the synthetic loop).
		 */
		st = 127 + (int)(in->yaw_delta * (127.0f / 0.05f));
		if (st < 0) st = 0;
		if (st > 255) st = 255;
		steering = (uint8_t)st;

		/*
		 * Throttle / brake: derived from the v_target gap.  When
		 * the bot wants to accelerate, throttle scales with the
		 * normalised positive gap; when it wants to brake,
		 * brake scales with the negative gap.  Approximate but
		 * non-zero across every tick the bot actually drives.
		 */
		t = dv > 0 ? (int)((dv / V_RACE) * 255.0f) : 0;
		b = dv < 0 ? (int)((-dv / V_RACE) * 255.0f) : 0;
		if (t > 255) t = 255;
		if (b > 255) b = 255;
		throttle = (uint8_t)t;
		brake = (uint8_t)b;

		/*
		 * Wheel slip proxy: v_current normalised × 25 (scaling
		 * matches the kunos receiver's `slip/25` decode per
		 * memory:reference_0x1e_field_semantics).  Same byte
		 * on all four wheels in this approximation.
		 */
		s = (in->v_current / V_RACE) * 25.0f * 4.0f;
		if (s < 0) s = 0;
		if (s > 255) s = 255;
		slip = (uint8_t)s;

		gear = in->gear;
		fuel = in->fuel;
		damage = in->damage;
		rpm = in->rpm;
		/*
		 * lateral_g_or_head_lean stays at signed-bias zero
		 * (127); we don't model cornering G in the synthetic
		 * loop precisely enough to fake the sign.
		 */
		lat_g = 127;
	}

	bb_reset();
	bb_u8(0x1e);
	bb_u16(conn_id);
	bb_u16(car_id);
	bb_u8(seq);
	bb_u32(client_ts_ms);
	bb_f32(pos_x); bb_f32(pos_y); bb_f32(pos_z);
	bb_f32(0); bb_f32(0); bb_f32(0);
	bb_f32(vel_x); bb_f32(vel_y); bb_f32(vel_z);
	/* input_a[4]: steering, throttle, brake, clutch */
	bb_u8(steering); bb_u8(throttle); bb_u8(brake); bb_u8(0);
	/* yaw_or_torque + steer_wheel_deg_half + engine_rpm */
	bb_u8(0); bb_u8(0); bb_u16(rpm);
	/* gear + fuel + damage */
	bb_u8(gear); bb_u8(fuel); bb_u8(damage);
	{
		uint32_t u; memcpy(&u, &norm_pos, 4);
		bb_u32(u);
	}
	/* wheel_slip[4] (a.k.a. input_b in older spec versions) */
	bb_u8(slip); bb_u8(slip); bb_u8(slip); bb_u8(slip);
	/* lateral_g_or_head_lean */
	bb_u8(lat_g);
	/* i16 alive_sentinel — sender side leaves at 0; server gates
	 * its own checks on the relayed value. */
	bb_u8(0); bb_u8(0);
	memcpy(out, bb_buf, bb_n);
	return bb_n;
}

/* ------------------------------------------------------------------ */
/* Racing line: array of waypoints sorted by norm_pos. */

struct Waypoint {
	float u, x, y, z, v;
	float radius;		/* local curvature radius in m, 1e6 for straights */
};
static struct Waypoint g_wp[MAX_WAYPOINTS];
static int g_wp_n;
static float g_track_length_m;	/* derived from cumulative chord distance */

/*
 * Per-waypoint local radius of curvature.  Circumradius of the
 * triangle formed by the waypoint and its two neighbours:
 *   R = |a| |b| |c| / (4 · area)
 * Smooths over a 5-point window so single-waypoint noise doesn't
 * create false "corners" on straights.  Cornering speed is computed
 * per tick from current effective grip (tyre wear × aero), not
 * baked in here, so changing µ live recomputes the limit.
 */
static void compute_corner_radii(void)
{
	int i, j;
	if (g_wp_n < 3) {
		for (i = 0; i < g_wp_n; i++)
			g_wp[i].radius = 1e6f;
		return;
	}
	for (i = 0; i < g_wp_n; i++) {
		int prev = (i + g_wp_n - 1) % g_wp_n;
		int next = (i + 1) % g_wp_n;
		float ax = g_wp[prev].x, az = g_wp[prev].z;
		float bx = g_wp[i].x,    bz = g_wp[i].z;
		float cx = g_wp[next].x, cz = g_wp[next].z;
		float ab = sqrtf((bx-ax)*(bx-ax) + (bz-az)*(bz-az));
		float bc = sqrtf((cx-bx)*(cx-bx) + (cz-bz)*(cz-bz));
		float ca = sqrtf((ax-cx)*(ax-cx) + (az-cz)*(az-cz));
		float area2 = fabsf((bx-ax)*(cz-az) - (bz-az)*(cx-ax));
		float r;
		if (area2 < 1e-3f || ab < 0.1f || bc < 0.1f)
			r = 1e6f;	/* near-collinear → straight */
		else
			r = (ab * bc * ca) / (2.0f * area2);
		g_wp[i].radius = r;
	}
	/* 5-point box smooth on radius (wraps). */
	{
		float tmp[MAX_WAYPOINTS];
		for (i = 0; i < g_wp_n; i++) {
			float sum = 0;
			for (j = -2; j <= 2; j++) {
				int k = (i + j + g_wp_n) % g_wp_n;
				sum += g_wp[k].radius;
			}
			tmp[i] = sum / 5.0f;
		}
		for (i = 0; i < g_wp_n; i++)
			g_wp[i].radius = tmp[i];
	}
}

/*
 * Solve v² = µ_eff(v) · g · R  for v, where
 *   µ_eff(v) = (µ_dry + K_AERO · v²) · wear_factor
 * Closed-form: v² · (1 − K_AERO · g · R · wear) = µ_dry · g · R · wear
 *           ⇒ v² = µ_dry · g · R · wear / (1 − K_AERO · g · R · wear)
 * Falls back to V_RACE if the denominator goes non-positive
 * (very long radius + lots of downforce; corner is effectively flat).
 */
static float v_corner_for_radius(float r, float wear)
{
	float gR = G_ACCEL * r * wear;
	float denom = 1.0f - K_AERO * gR;
	float v2;
	if (denom <= 1e-3f)
		return V_RACE;
	v2 = MU_DRY * gR / denom;
	if (v2 < 0)
		return V_RACE;
	float v = sqrtf(v2);
	return v > V_RACE ? V_RACE : v;
}

/* Lowest cornering speed within brake_dist metres ahead of u_now,
 * using the current effective grip (tyre wear).  Used as the look-
 * ahead target so the bot starts braking before the apex. */
static float min_corner_ahead(float u_now, float brake_dist, float wear)
{
	float u_step = brake_dist / g_track_length_m;
	float u_end = u_now + u_step;
	int i;
	float lo = V_RACE;
	for (i = 0; i < g_wp_n; i++) {
		float u = g_wp[i].u;
		int in_range;
		if (u_end < 1.0f)
			in_range = (u >= u_now && u <= u_end);
		else
			in_range = (u >= u_now || u <= u_end - 1.0f);
		if (in_range) {
			float v = v_corner_for_radius(g_wp[i].radius, wear);
			if (v < lo)
				lo = v;
		}
	}
	return lo;
}

/* Compute total racing-line length by summing chord distance between
 * consecutive waypoints (closing the loop back to wp[0]). */
static void compute_track_length(void)
{
	int i;
	g_track_length_m = 0;
	for (i = 1; i < g_wp_n; i++) {
		float dx = g_wp[i].x - g_wp[i - 1].x;
		float dy = g_wp[i].y - g_wp[i - 1].y;
		float dz = g_wp[i].z - g_wp[i - 1].z;
		g_track_length_m += sqrtf(dx*dx + dy*dy + dz*dz);
	}
	if (g_wp_n > 1) {
		float dx = g_wp[0].x - g_wp[g_wp_n - 1].x;
		float dy = g_wp[0].y - g_wp[g_wp_n - 1].y;
		float dz = g_wp[0].z - g_wp[g_wp_n - 1].z;
		g_track_length_m += sqrtf(dx*dx + dy*dy + dz*dz);
	}
}

/*
 * Native .ai loader — Kunos AISpline binary format from
 * `<install>/AC2/Content/Cache/<track>/fastlane.ai`.  See
 * reference_acc_ai_file_format.md for full RE notes.  Layout:
 *   u32 version, num_points, reserved_a, reserved_b
 *   num_points × 36 B: 3 f64 position + 12 B unused
 *   u32 num_payloads (== num_points)
 *   num_payloads × 80 B (v ≥ 8) or 72 B (v < 8): per-point AI data;
 *      payload+0x00 (speed) is zero in every shipped fastlane.ai —
 *      Kunos computes speeds dynamically.  We skip the payload
 *      block entirely and let V_RACE drive the bot.
 */
static int load_waypoints_ai(const char *path)
{
	FILE *f;
	uint32_t header[4];
	uint32_t num_points;
	int i;

	f = fopen(path, "rb");
	if (!f) {
		fprintf(stderr, "track: cannot open %s: %s\n", path,
		    strerror(errno));
		return -1;
	}
	if (fread(header, sizeof header, 1, f) != 1) {
		fprintf(stderr, "track: %s: header read failed\n", path);
		fclose(f);
		return -1;
	}
	num_points = header[1];
	if (num_points < 2 || num_points > MAX_WAYPOINTS) {
		fprintf(stderr, "track: %s: implausible num_points=%u "
		    "(MAX_WAYPOINTS=%d)\n", path, num_points, MAX_WAYPOINTS);
		fclose(f);
		return -1;
	}
	g_wp_n = 0;
	for (i = 0; i < (int)num_points; i++) {
		double xyz[3];
		uint8_t tail[12];
		if (fread(xyz, sizeof xyz, 1, f) != 1 ||
		    fread(tail, sizeof tail, 1, f) != 1) {
			fprintf(stderr, "track: %s: short spline read at "
			    "%d\n", path, i);
			fclose(f);
			return -1;
		}
		g_wp[g_wp_n].u = (float)i / (float)num_points;
		g_wp[g_wp_n].x = (float)xyz[0];
		g_wp[g_wp_n].y = (float)xyz[1];
		g_wp[g_wp_n].z = (float)xyz[2];
		g_wp[g_wp_n].v = V_RACE;
		g_wp_n++;
	}
	/* Don't bother reading payloads or the GRID DATA tail — we
	 * have everything we need (positions). */
	fclose(f);
	compute_track_length();
	/* Re-derive norm_pos from cumulative chord distance now that
	 * we have all points loaded. */
	{
		float cum = 0;
		g_wp[0].u = 0;
		for (i = 1; i < g_wp_n; i++) {
			float dx = g_wp[i].x - g_wp[i - 1].x;
			float dy = g_wp[i].y - g_wp[i - 1].y;
			float dz = g_wp[i].z - g_wp[i - 1].z;
			cum += sqrtf(dx*dx + dy*dy + dz*dz);
			g_wp[i].u = g_track_length_m > 0 ?
			    cum / g_track_length_m : (float)i / num_points;
		}
	}
	compute_corner_radii();
	printf("[bot] loaded %d waypoints from %s (length=%.0f m, "
	    "version=%u, .ai format — speeds derived from curvature)\n",
	    g_wp_n, path, g_track_length_m, header[0]);
	return 0;
}

/* Detect format by extension — .ai goes through the binary loader,
 * everything else through the CSV path. */
static int load_waypoints(const char *path)
{
	const char *ext;
	FILE *f;
	char line[256];
	int with_speed = 0, no_speed = 0;

	ext = strrchr(path, '.');
	if (ext && strcmp(ext, ".ai") == 0)
		return load_waypoints_ai(path);

	f = fopen(path, "r");
	if (!f) {
		fprintf(stderr, "track: cannot open %s: %s\n", path,
		    strerror(errno));
		return -1;
	}
	g_wp_n = 0;
	while (fgets(line, sizeof line, f)) {
		float u, x, y, z, v;
		int n;
		if (line[0] == '#' || line[0] == '\n') continue;
		n = sscanf(line,
		    "%f%*[ ,\t]%f%*[ ,\t]%f%*[ ,\t]%f%*[ ,\t]%f",
		    &u, &x, &y, &z, &v);
		if (n < 4)
			continue;
		if (g_wp_n >= MAX_WAYPOINTS) {
			fprintf(stderr, "track: > %d waypoints, truncating\n",
			    MAX_WAYPOINTS);
			break;
		}
		g_wp[g_wp_n].u = u;
		g_wp[g_wp_n].x = x;
		g_wp[g_wp_n].y = y;
		g_wp[g_wp_n].z = z;
		/* Some .ai files (Kunos fastlane.ai) carry geometry but
		 * leave the speed column zero — the AI computes speeds
		 * from curvature at runtime.  Treat 0 as "use V_RACE"
		 * so the bot doesn't stall. */
		if (n == 5 && v > 0.1f) {
			g_wp[g_wp_n].v = v;
			with_speed++;
		} else {
			g_wp[g_wp_n].v = V_RACE;
			no_speed++;
		}
		g_wp_n++;
	}
	fclose(f);
	if (g_wp_n < 2) {
		fprintf(stderr, "track: %s has < 2 waypoints\n", path);
		return -1;
	}
	compute_track_length();
	compute_corner_radii();
	printf("[bot] loaded %d waypoints from %s (length=%.0f m, "
	    "with_speed=%d no_speed=%d)\n",
	    g_wp_n, path, g_track_length_m, with_speed, no_speed);
	return 0;
}

/* Default trajectory: stadium shape (two semicircles + two straights).
 * Synthesized when no --track CSV is given so the bot still has a
 * non-trivial closed loop.  Speed is constant V_RACE — there's no
 * recorded racing line to derive a profile from. */
static void default_waypoints(void)
{
	const int N = 64;
	const float L = 200.0f;
	const float R = 80.0f;
	int i;

	g_wp_n = N + 1;
	for (i = 0; i <= N; i++) {
		float u = (float)i / (float)N;
		float s = u * (2.0f * L + 2.0f * (float)M_PI * R);
		float x, z;
		if (s < L) { x = s; z = R; }
		else if (s < L + (float)M_PI * R) {
			float t = (s - L) / R;
			x = L + R * sinf(t);
			z = R * cosf(t);
		} else if (s < 3.0f * L + (float)M_PI * R) {
			x = L - (s - (L + (float)M_PI * R));
			z = -R;
		} else {
			float t = (s - 3.0f * L - (float)M_PI * R) / R;
			x = -L - R * sinf(t);
			z = -R * cosf(t);
		}
		g_wp[i].u = u;
		g_wp[i].x = x;
		g_wp[i].y = 0;
		g_wp[i].z = z;
		g_wp[i].v = V_RACE;
	}
	compute_track_length();
	compute_corner_radii();
}

/* Linear-interpolated speed at the given norm_pos (matches
 * waypoint_at's interpolation strategy). */
static float waypoint_speed(float u)
{
	int i;
	while (u < 0) u += 1.0f;
	while (u >= 1.0f) u -= 1.0f;
	for (i = 0; i < g_wp_n - 1; i++) {
		if (u >= g_wp[i].u && u <= g_wp[i + 1].u) {
			float du = g_wp[i + 1].u - g_wp[i].u;
			float t = du > 1e-6f ? (u - g_wp[i].u) / du : 0;
			return g_wp[i].v + t * (g_wp[i + 1].v - g_wp[i].v);
		}
	}
	return g_wp[0].v;
}

/* Lookup: interpolate (x,y,z) at given norm_pos in [0,1). */
static void waypoint_at(float u, float *x, float *y, float *z)
{
	int i;
	while (u < 0) u += 1.0f;
	while (u >= 1.0f) u -= 1.0f;
	for (i = 0; i < g_wp_n - 1; i++) {
		if (u >= g_wp[i].u && u <= g_wp[i + 1].u) {
			float du = g_wp[i + 1].u - g_wp[i].u;
			float t = du > 1e-6f ? (u - g_wp[i].u) / du : 0;
			*x = g_wp[i].x + t * (g_wp[i + 1].x - g_wp[i].x);
			*y = g_wp[i].y + t * (g_wp[i + 1].y - g_wp[i].y);
			*z = g_wp[i].z + t * (g_wp[i + 1].z - g_wp[i].z);
			return;
		}
	}
	*x = g_wp[0].x; *y = g_wp[0].y; *z = g_wp[0].z;
}

/* ------------------------------------------------------------------ */
/* Session bring-up: TCP connect → handshake → 0x0b parse → UDP setup.
 * Returns 0 on success, -1 on failure (caller should back off + retry).
 * On success populates *tcp_fd_out, *udp_fd_out, *udp_peer_out, and
 * *conn_id_out / *car_id_out from the welcome header. */
static int
connect_session(const char *host, uint16_t tcp_port,
    const char *first_name, const char *last_name,
    const char *short_name, const char *steam,
    uint32_t race_number, uint8_t driver_cat,
    uint16_t client_version, const char *password,
    int *tcp_fd_out, int *udp_fd_out,
    struct sockaddr_in *udp_peer_out,
    uint16_t *conn_id_out, uint32_t *car_id_out,
    size_t *trailer_len_out)
{
	uint8_t hs[2048], welcome[65536];
	size_t hs_len = sizeof hs, wl;
	int tcp_fd, udp_fd;
	uint16_t udp_port;
	struct sockaddr_in udp_peer;

	tcp_fd = tcp_connect(host, tcp_port);
	if (tcp_fd < 0)
		return -1;
	if (build_handshake(first_name, last_name, short_name, steam,
	    race_number, driver_cat, client_version, password,
	    hs, &hs_len) < 0) {
		close(tcp_fd);
		return -1;
	}
	if (send(tcp_fd, hs, hs_len, MSG_NOSIGNAL) != (ssize_t)hs_len) {
		close(tcp_fd);
		return -1;
	}
	if (recv_frame(tcp_fd, welcome, sizeof welcome, &wl) < 0) {
		close(tcp_fd);
		return -1;
	}
	if (wl < 10 || welcome[0] != 0x0b) {
		if (welcome[0] == 0x0c && wl >= 14) {
			printf("[bot] received 0x0c reject reason=%u sub=%u "
			    "detail_a=%u detail_b=%u\n",
			    welcome[1],
			    welcome[2] | (welcome[3] << 8) |
			        (welcome[4] << 16) | (welcome[5] << 24),
			    welcome[6] | (welcome[7] << 8) |
			        (welcome[8] << 16) | (welcome[9] << 24),
			    welcome[10] | (welcome[11] << 8) |
			        (welcome[12] << 16) | (welcome[13] << 24));
		} else {
			fprintf(stderr, "[bot] reply 0x%02x len=%zu "
			    "(expected 0x0b)\n", welcome[0], wl);
		}
		close(tcp_fd);
		return -1;
	}
	udp_port = welcome[1] | (welcome[2] << 8);
	*conn_id_out = welcome[4] | (welcome[5] << 8);
	*car_id_out = welcome[6] | (welcome[7] << 8) |
	    (welcome[8] << 16) | (welcome[9] << 24);
	*trailer_len_out = wl;

	udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (udp_fd < 0) { close(tcp_fd); return -1; }
	memset(&udp_peer, 0, sizeof udp_peer);
	udp_peer.sin_family = AF_INET;
	udp_peer.sin_port = htons(udp_port);
	if (inet_pton(AF_INET, host, &udp_peer.sin_addr) != 1) {
		struct hostent *he = gethostbyname(host);
		if (!he) { close(tcp_fd); close(udp_fd); return -1; }
		memcpy(&udp_peer.sin_addr, he->h_addr_list[0],
		    sizeof udp_peer.sin_addr);
	}

	{
		int fl = fcntl(udp_fd, F_GETFL, 0);
		fcntl(udp_fd, F_SETFL, fl | O_NONBLOCK);
		fl = fcntl(tcp_fd, F_GETFL, 0);
		fcntl(tcp_fd, F_SETFL, fl | O_NONBLOCK);
	}

	*tcp_fd_out = tcp_fd;
	*udp_fd_out = udp_fd;
	*udp_peer_out = udp_peer;
	return 0;
}

/* ------------------------------------------------------------------ */

static void usage(const char *p)
{
	fprintf(stderr,
	    "Usage: %s --host H --tcp P [--race N] [--name S] [--track FILE]\n"
	    "          [--length M] [--pit-on-lap N] [--laps N]\n"
	    "\n"
	    "  --host           server hostname or IP\n"
	    "  --tcp            TCP port (default 9232)\n"
	    "  --race           race number (default 911)\n"
	    "  --name           driver first name (default Bot)\n"
	    "  --track FILE     CSV waypoints (norm_pos x y z [speed]);\n"
	    "                   default is a synthetic stadium loop\n"
	    "  --length M       override the track length (default: derive\n"
	    "                   from cumulative waypoint distance)\n"
	    "  --pit-on-lap N   enter pit on lap N (1-based; default never)\n"
	    "  --pit-speed M    override pit-lane speed cap (m/s; default 18,\n"
	    "                   server pit-speeding gate fires above 22.22)\n"
	    "  --laps N         stop after N completed laps (default loop)\n"
	    "  --grid N         starting grid position (1-based; 1=pole)\n"
	    "  --mid-race       join an in-progress race (skip formation)\n"
	    "  --no-mandatory-pit  do not send 0x54 after pit traversal\n"
	    "  --send-penalty-served  emit 0x42 after pit traversal,\n"
	    "                   signalling the AC2 client served a DT/SG\n"
	    "  --stint-reset T:F  emit 0x4f at tick T with force=F\n"
	    "                   (force=0 voluntary / 1 forced)\n"
	    "  --load-setup T:S  emit 0x55 at tick T for session_type S\n"
	    "                   (S = 0 P, 4 Q, 10 R)\n"
	    "  --swap-state T:S1[,S2,...]  emit 0x47 at tick T with the\n"
	    "                   given per-driver swap-state bytes\n"
	    "  --damage T:z1,z2,z3,z4,z5  emit 0x43 damage zones at tick T\n"
	    "                   (5 u8 zones front/rear/left/right/centre)\n"
	    "  --sa-contact T:target:quality  emit 0x19 SA contact at tick T\n"
	    "                   (target = race number, 0xffff = wall hit)\n"
	    "  --oot-at T       emit 0x3d ACP_OUT_OF_TRACK at tick T (force=0)\n"
	    "  --time-event T   emit 0x5e UDP latency event at tick T\n"
	    "                   (source=target=self, latency=42 ms, chat=1)\n"
	    "  --flap-at T      close TCP at tick T and let the bots\n"
	    "                   reconnect path re-handshake (deliberate\n"
	    "                   socket-flap regression for server-side\n"
	    "                   quick-reconnect detection)\n"
	    "  --swap-request T:sub:state  emit 0x4a at tick T (sub 2=init,\n"
	    "                   3=confirm, 4=execute)\n"
	    "  --client-version V  override the 0x09 handshake version u16\n"
	    "                   (default 0x0100, the protocol version)\n"
	    "  --password P     password to send in the 0x09 handshake\n"
	    "  --expect-reject  exit 0 when the server replies 0x0c reject\n"
	    "  --bump M         one-shot lateral kick of M metres\n"
	    "  --bump-at-lap N  apply the kick at the start of lap N\n"
	    "  --report-penalty cat:kind:value\n"
	    "                   send one 0x41 ACP_REPORT_PENALTY at tick 200\n"
	    "  --zero-inputs    emit legacy all-zero input bytes in every\n"
	    "                   0x1e car_update (steering / throttle / brake /\n"
	    "                   wheel_slip / rpm / gear / fuel / damage all\n"
	    "                   zero).  Use when pcap-diffing against a kunos\n"
	    "                   capture that itself was taken with stationary\n"
	    "                   inputs.\n",
	    p);
}

int main(int argc, char **argv)
{
	const char *host = NULL, *track_path = NULL;
	const char *name = "Bot";
	uint16_t tcp_port = 9232;
	uint32_t race_number = 911;
	float track_length_override = -1.0f;
	int pit_on_lap = -1;
	float pit_speed_cap = V_PITLANE;
	int max_laps = -1;
	int grid_pos = 1;		/* 1-based; 1 = pole */
	int mid_race = 0;		/* skip formation lap */
	int mandatory_pit = 1;		/* send 0x54 after pit traversal */
	int send_penalty_served = 0;	/* send 0x42 after pit traversal */
	int stint_reset_tick = -1;	/* >=0 = emit 0x4f at this tick */
	int stint_reset_force = 0;	/* force byte for stint reset */
	int stint_reset_sent = 0;
	int load_setup_tick = -1;	/* >=0 = emit 0x55 at this tick */
	int load_setup_sess = 0;	/* session_type byte for load_setup */
	int load_setup_sent = 0;
	int swap_state_tick = -1;	/* >=0 = emit 0x47 at this tick */
	uint8_t swap_state_bytes[8];	/* per-driver state bytes */
	uint8_t swap_state_count = 0;	/* number of states populated */
	int swap_state_sent = 0;
	int damage_tick = -1;		/* >=0 = emit 0x43 at this tick */
	uint8_t damage_bytes[5] = {0,0,0,0,0};
	int damage_sent = 0;
	int sa_tick = -1;		/* >=0 = emit 0x19 at this tick */
	uint16_t sa_target_race = 0;
	uint8_t  sa_quality = 0;
	int sa_sent = 0;
	int oot_tick = -1;		/* >=0 = emit 0x3d at this tick */
	int oot_sent = 0;
	int te_tick = -1;		/* >=0 = emit 0x5e at this tick */
	int te_sent = 0;
	int flap_at_tick = -1;		/* close+reopen TCP once at this tick */
	int flap_done = 0;
	int swap_req_tick = -1;		/* >=0 = emit 0x4a at this tick */
	uint8_t swap_req_sub = 2;	/* 2=init, 3=confirm, 4=execute */
	uint8_t swap_req_state = 0;
	int swap_req_sent = 0;
	uint16_t client_version = 0x0100;	/* override with --client-version */
	const char *password = "";	/* override with --password */
	int expect_reject = 0;		/* --expect-reject suppresses fatal on reject */
	float bump_metres = 0;		/* one-shot lateral kick magnitude */
	int bump_at_lap = -1;		/* lap on which to apply the bump */
	char steam[32];
	uint8_t pkt[256];
	uint16_t conn_id;
	uint32_t car_id;
	int tcp_fd, udp_fd;
	struct sockaddr_in udp_peer;
	uint32_t tick = 0, t0_ms;
	uint8_t seq = 0;
	float u_pos = 0.0f;
	int lap = 0;
	int last_loc = LOC_TRACK;
	float last_x = 0, last_z = 0, last_y = 0;
	int have_last = 0;
	uint32_t lap_start_ms = 0;
	uint32_t sector_start_ms = 0;
	int last_sector = 0;
	int pit_served_this_visit = 0;
	int reconnect_count = 0;
	uint32_t reconnect_backoff_ms = 1000;
	float v_current = 0;	/* physics: actual speed, smoothed toward target */
	float prev_yaw = 0;	/* atan2(vz, vx) from previous tick; for steering proxy */
	int   have_yaw = 0;
	float lateral_offset = 0;	/* metres off the racing line; > 0 = right */
	uint32_t on_track_ms = 0;	/* total ms with location=Track for tyre wear */
	int bump_applied_for_lap = -1;
	uint8_t damage[5] = {0,0,0,0,0};	/* front, rear, left, right, centre */
	uint8_t car_dirt[5] = {0,0,0,0,0};	/* per-tyre dirt accumulator */
	uint8_t tyre_compound = 0;		/* 0 = dry, 1 = wet */

	static struct option opts[] = {
		{"host",        required_argument, 0, 'H'},
		{"tcp",         required_argument, 0, 'T'},
		{"race",        required_argument, 0, 'R'},
		{"name",        required_argument, 0, 'N'},
		{"track",       required_argument, 0, 't'},
		{"length",      required_argument, 0, 'L'},
		{"pit-on-lap",  required_argument, 0, 'P'},
		{"pit-speed",   required_argument, 0, 'K'},
		{"laps",        required_argument, 0, 'l'},
		{"grid",        required_argument, 0, 'G'},
		{"mid-race",    no_argument,       0, 'M'},
		{"no-mandatory-pit", no_argument,  0, 'X'},
		{"send-penalty-served", no_argument, 0, 'Y'},
		{"stint-reset", required_argument, 0, 'r'},
		{"load-setup",  required_argument, 0, 'q'},
		{"swap-state",  required_argument, 0, 'k'},
		{"damage",      required_argument, 0, 'g'},
		{"sa-contact",  required_argument, 0, 'a'},
		{"oot-at",      required_argument, 0, 'o'},
		{"time-event",  required_argument, 0, 'E'},
		{"flap-at",     required_argument, 0, 'F'},
		{"swap-request", required_argument, 0, 'Q'},
		{"client-version", required_argument, 0, 'v'},
		{"password",    required_argument, 0, 'W'},
		{"expect-reject", no_argument,     0, 'j'},
		{"bump",        required_argument, 0, 'B'},
		{"bump-at-lap", required_argument, 0, 'A'},
		{"report-penalty", required_argument, 0, 'p'},
		{"driver-cat", required_argument, 0, 'D'},
		{"penalty-start-tick", required_argument, 0, 'S'},
		{"chat", required_argument, 0, 'C'},
		{"chat-start-tick", required_argument, 0, 'Z'},
		{"zero-inputs", no_argument,       0, 'I'},
		{"help",        no_argument,       0, 'h'},
		{0,0,0,0}
	};
	int o;
	#define MAX_RPS 64
	int rp_cat[MAX_RPS], rp_kind[MAX_RPS];
	int32_t rp_value[MAX_RPS];
	int rp_n = 0, rp_sent_n = 0;
	uint8_t driver_cat = 0;
	uint32_t penalty_start_tick = 200;
	#define MAX_CHATS 16
	const char *chat_msgs[MAX_CHATS];
	int chat_n = 0, chat_sent_n = 0;
	uint32_t chat_start_tick = 60;
	int zero_inputs = 0;	/* --zero-inputs: emit all-zero input bytes
				 * for byte-diff parity with legacy pcaps. */
	while ((o = getopt_long(argc, argv, "H:T:R:N:t:L:P:l:G:MXYr:q:k:g:F:Q:v:W:jB:A:p:D:S:C:Z:Ih",
	    opts, NULL)) != -1) {
		switch (o) {
		case 'H': host = optarg; break;
		case 'T': tcp_port = (uint16_t)atoi(optarg); break;
		case 'R': race_number = (uint32_t)atoi(optarg); break;
		case 'N': name = optarg; break;
		case 't': track_path = optarg; break;
		case 'L': track_length_override = (float)atof(optarg); break;
		case 'P': pit_on_lap = atoi(optarg); break;
		case 'K': pit_speed_cap = (float)atof(optarg); break;
		case 'l': max_laps = atoi(optarg); break;
		case 'G': grid_pos = atoi(optarg); break;
		case 'M': mid_race = 1; break;
		case 'X': mandatory_pit = 0; break;
		case 'Y': send_penalty_served = 1; break;
		case 'r': {
			int t, f;
			if (sscanf(optarg, "%d:%d", &t, &f) != 2) {
				fprintf(stderr,
				    "[bot] --stint-reset needs tick:force\n");
				return 2;
			}
			stint_reset_tick = t;
			stint_reset_force = f;
			break;
		}
		case 'q': {
			int t, st;
			if (sscanf(optarg, "%d:%d", &t, &st) != 2) {
				fprintf(stderr,
				    "[bot] --load-setup needs tick:sess_type\n");
				return 2;
			}
			load_setup_tick = t;
			load_setup_sess = st;
			break;
		}
		case 'F':
			flap_at_tick = atoi(optarg);
			break;
		case 'Q': {
			int t, sub, st;
			if (sscanf(optarg, "%d:%d:%d", &t, &sub, &st) != 3) {
				fprintf(stderr,
				    "[bot] --swap-request needs tick:sub:state\n");
				return 2;
			}
			swap_req_tick = t;
			swap_req_sub = (uint8_t)sub;
			swap_req_state = (uint8_t)st;
			break;
		}
		case 'g': {
			/* --damage T:z1,z2,z3,z4,z5 */
			int t, z[5];
			if (sscanf(optarg, "%d:%d,%d,%d,%d,%d",
			    &t, &z[0], &z[1], &z[2], &z[3], &z[4]) != 6) {
				fprintf(stderr,
				    "[bot] --damage needs tick:z1,z2,z3,z4,z5\n");
				return 2;
			}
			damage_tick = t;
			for (int dz = 0; dz < 5; dz++)
				damage_bytes[dz] = (uint8_t)z[dz];
			break;
		}
		case 'a': {
			/* --sa-contact T:target:quality */
			int t, tgt, q;
			if (sscanf(optarg, "%d:%d:%d", &t, &tgt, &q) != 3) {
				fprintf(stderr,
				    "[bot] --sa-contact needs tick:target:quality\n");
				return 2;
			}
			sa_tick = t;
			sa_target_race = (uint16_t)tgt;
			sa_quality = (uint8_t)q;
			break;
		}
		case 'o':
			oot_tick = atoi(optarg);
			break;
		case 'E':
			te_tick = atoi(optarg);
			break;
		case 'k': {
			/* --swap-state T:S1[,S2,...] */
			char *colon = strchr(optarg, ':');
			char *p, *end;
			if (!colon) {
				fprintf(stderr,
				    "[bot] --swap-state needs tick:state[,state...]\n");
				return 2;
			}
			swap_state_tick = atoi(optarg);
			swap_state_count = 0;
			for (p = colon + 1;
			    swap_state_count <
			        (uint8_t)(sizeof(swap_state_bytes));
			    p = end + 1) {
				long v = strtol(p, &end, 0);
				if (p == end)
					break;
				swap_state_bytes[swap_state_count++] =
				    (uint8_t)v;
				if (*end != ',')
					break;
			}
			break;
		}
		case 'v':
			client_version = (uint16_t)strtoul(optarg, NULL, 0);
			break;
		case 'W':
			password = optarg;
			break;
		case 'j':
			expect_reject = 1;
			break;
		case 'B': bump_metres = (float)atof(optarg); break;
		case 'A': bump_at_lap = atoi(optarg); break;
		case 'p': {
			int a, b, c;
			if (sscanf(optarg, "%d:%d:%d", &a, &b, &c) != 3) {
				fprintf(stderr,
				    "[bot] --report-penalty needs cat:kind:value\n");
				return 2;
			}
			if (rp_n >= MAX_RPS) {
				fprintf(stderr, "[bot] too many penalties\n");
				return 2;
			}
			rp_cat[rp_n] = a; rp_kind[rp_n] = b;
			rp_value[rp_n] = c; rp_n++;
			break;
		}
		case 'D':
			driver_cat = (uint8_t)atoi(optarg);
			break;
		case 'S':
			penalty_start_tick = (uint32_t)atoi(optarg);
			break;
		case 'C':
			if (chat_n >= MAX_CHATS) {
				fprintf(stderr, "[bot] too many --chat\n");
				return 2;
			}
			{
				/*
				 * Underscores in --chat are translated to spaces
				 * so the test runners can pass slash commands
				 * like `/admin_admin` or `/dq_911` without
				 * tripping POSIX sh word-splitting.
				 */
				char *m = strdup(optarg);
				char *p;
				if (m == NULL) {
					fprintf(stderr,
					    "[bot] strdup oom\n");
					return 2;
				}
				for (p = m; *p; p++)
					if (*p == '_')
						*p = ' ';
				chat_msgs[chat_n++] = m;
			}
			break;
		case 'Z':
			chat_start_tick = (uint32_t)atoi(optarg);
			break;
		case 'I':
			zero_inputs = 1;
			break;
		case 'h': usage(argv[0]); return 0;
		default:  usage(argv[0]); return 2;
		}
	}
	if (!host) { usage(argv[0]); return 2; }

	signal(SIGINT, on_sigint);
	signal(SIGTERM, on_sigint);
	setvbuf(stdout, NULL, _IOLBF, 0);

	if (track_path) {
		if (load_waypoints(track_path) < 0)
			return 1;
	} else {
		default_waypoints();
		printf("[bot] using synthetic stadium loop (%d waypoints, "
		    "%.0f m)\n", g_wp_n, g_track_length_m);
	}
	if (track_length_override > 0)
		g_track_length_m = track_length_override;

	snprintf(steam, sizeof steam, "S7656119900%07u",
	    (unsigned)race_number);

	/* Initial connection (must succeed before driving). */
	{
		size_t wl;
		int rc = connect_session(host, tcp_port, name, "Driver",
		    "BOT", steam, race_number, driver_cat, client_version,
		    password, &tcp_fd, &udp_fd, &udp_peer, &conn_id,
		    &car_id, &wl);
		if (rc < 0) {
			if (expect_reject) {
				fprintf(stderr, "[bot] reject path "
				    "completed (expected)\n");
				return 0;
			}
			fprintf(stderr, "[bot] initial connect failed\n");
			return 1;
		}
		printf("[bot] welcome ok: conn=%u car=%u udp=%u "
		    "(trailer %zu B)\n", conn_id, (unsigned)car_id,
		    ntohs(udp_peer.sin_port), wl);
	}

	{
		size_t n = pkt_keepalive(pkt, conn_id);
		sendto(udp_fd, pkt, n, 0,
		    (struct sockaddr *)&udp_peer, sizeof udp_peer);
	}

	/* Tell the server which tyre compound we're starting on so the
	 * leaderboard / spectator HUD shows it.  Default = dry (0). */
	{
		size_t n = pkt_tyre_compound(pkt, (uint16_t)car_id,
		    tyre_compound);
		send_tcp_framed(tcp_fd, pkt, n);
	}

	t0_ms = mono_ms();
	/*
	 * Grid offset: each slot sits ~5 m behind the next on a real
	 * starting grid.  Express as a fraction of the lap: 5 m on a
	 * 4 km track is 0.00125 — small enough to keep grid positions
	 * inside the pre-start segment near norm_pos = 1.0.  Pole
	 * (grid=1) sits at u = 1 - 0.00125, slot 20 at u = 1 - 0.025.
	 */
	{
		float grid_step = 5.0f / g_track_length_m;
		float u0 = 1.0f - (float)grid_pos * grid_step;
		while (u0 < 0) u0 += 1.0f;
		u_pos = u0;
	}
	if (mid_race) {
		lap = 1;
		printf("[bot] --mid-race: skipping formation, starting at "
		    "lap=1 (race in progress)\n");
	}
	printf("[bot] formation @ %.1f m/s, racing = recorded line speed "
	    "(pit cap %.1f m/s), track=%.0fm grid=%d pit_on_lap=%d "
	    "mandatory_pit=%d\n",
	    V_FORMATION, V_PITLANE, g_track_length_m, grid_pos,
	    pit_on_lap, mandatory_pit);
	lap_start_ms = 0;
	sector_start_ms = 0;
	last_sector = (u_pos < 1.0f / 3.0f) ? 0 :
	    (u_pos < 2.0f / 3.0f) ? 1 : 2;

	while (!g_stop) {
		struct timespec slp = {0, TICK_MS * 1000000L};
		uint32_t now = mono_ms();
		uint32_t client_ts = now - t0_ms;
		float dt = TICK_MS / 1000.0f;
		float v_target;
		uint8_t loc;
		float px, py, pz, vx, vy, vz;
		uint8_t rxbuf[2048];
		ssize_t rn;

		/*
		 * Phase model:
		 *   - lap 0 = formation: cap velocity at V_FORMATION
		 *     (under 70 km/h) so the client's formation-lap speed cap
		 *     never trips.  Server's leader-position trigger fires
		 *     formation_end and green when we cross the per-track
		 *     ranges regardless of how slowly we're going.
		 *   - lap >= 1 = racing: drive at the *recorded racing-line
		 *     speed* at the current norm_pos.  Slow in corners, fast
		 *     on straights — geometry comes from the pcap-extracted
		 *     waypoints, not a hand-tuned constant.
		 *   - if --pit-on-lap matches and we're in the pit window
		 *     (last/first 5% of norm_pos), report Pitlane and cap at
		 *     V_PITLANE so the 22.22 m/s pit-speeding DQ never fires.
		 */
		/*
		 * Tyre wear factor based on time spent on track this
		 * session.  Linear taper, never below 70 % grip.
		 */
		float wear_factor;
		{
			float minutes = on_track_ms / 60000.0f;
			wear_factor = 1.0f - TYRE_WEAR_PER_MIN * minutes;
			if (wear_factor < TYRE_WEAR_FLOOR)
				wear_factor = TYRE_WEAR_FLOOR;
		}

		/*
		 * Target speed for this tick.  Recorded racing-line speed
		 * (from a pcap) wins if present; otherwise the curvature-
		 * derived corner limit kicks in, with a brake-distance
		 * look-ahead so we slow before the apex, not at it.
		 * Effective grip blends mechanical µ + aero µ_aero(v) +
		 * tyre wear; v_corner_for_radius() does the closed-form.
		 */
		v_target = waypoint_speed(u_pos);
		{
			int wp_i = (int)(u_pos * g_wp_n) % g_wp_n;
			float v_here, v_ahead;
			if (wp_i < 0) wp_i += g_wp_n;
			v_here = v_corner_for_radius(
			    g_wp[wp_i].radius, wear_factor);
			v_ahead = min_corner_ahead(u_pos,
			    v_current * BRAKE_LOOKAHEAD_S, wear_factor);
			if (v_here < v_target)
				v_target = v_here;
			if (v_ahead < v_target)
				v_target = v_ahead;
		}
		loc = LOC_TRACK;
		if (lap == 0 && v_target > V_FORMATION)
			v_target = V_FORMATION;
		if (pit_on_lap > 0 && lap == pit_on_lap) {
			if (u_pos > 0.95f) {
				if (v_target > pit_speed_cap)
					v_target = pit_speed_cap;
				loc = (last_loc == LOC_TRACK) ?
				    LOC_PITENTRY : LOC_PITLANE;
			} else if (u_pos < 0.05f) {
				if (v_target > pit_speed_cap)
					v_target = pit_speed_cap;
				loc = (last_loc == LOC_PITLANE) ?
				    LOC_PITEXIT : LOC_PITLANE;
			}
		}

		/*
		 * Smooth speed toward target with separate accel / brake
		 * limits.  Real GT3 cars accelerate at ~8 m/s² off-corner
		 * and brake at ~25 m/s² (~2.5 g).  The instantaneous
		 * speed v_current is what we report in vec_c and what
		 * advances u_pos — physically consistent.
		 */
		{
			float dv = v_target - v_current;
			float dv_max = (dv > 0 ? A_ACCEL : A_BRAKE) * dt;
			if (dv > dv_max) dv = dv_max;
			else if (dv < -dv_max) dv = -dv_max;
			v_current += dv;
			if (v_current < 0) v_current = 0;
		}

		/* Advance norm_pos at v_current / track_length. */
		u_pos += v_current * dt / g_track_length_m;
		if (u_pos >= 1.0f) {
			u_pos -= 1.0f;
			lap++;
			printf("[bot] lap %d completed at t=%us "
			    "(tyre %.0f%%)\n",
			    lap, (unsigned)(client_ts / 1000),
			    wear_factor * 100.0f);
			if (max_laps > 0 && lap >= max_laps)
				g_stop = 1;
			/* Bump test: at the start of lap N, kick the bot
			 * laterally off the racing line by --bump metres.
			 * It steers back exponentially with τ = 1.8 s.
			 * Going off-line takes a bit of damage + dirt. */
			if (lap == bump_at_lap &&
			    bump_applied_for_lap != lap &&
			    fabsf(bump_metres) > 0.01f) {
				int i;
				lateral_offset += bump_metres;
				bump_applied_for_lap = lap;
				/* Bump impacts: front + a side panel + dirt
				 * on all tyres. */
				if (damage[0] < 240) damage[0] += 15;
				if (bump_metres > 0) {
					if (damage[3] < 240) damage[3] += 10;
				} else {
					if (damage[2] < 240) damage[2] += 10;
				}
				for (i = 0; i < 5; i++)
					if (car_dirt[i] < 250)
						car_dirt[i] += 5;
				printf("[bot] BUMP at lap=%d: offset=%+.1f m, "
				    "damage=[%u,%u,%u,%u,%u] dirt+=5\n",
				    lap, lateral_offset, damage[0],
				    damage[1], damage[2], damage[3],
				    damage[4]);
			}

			/* Per-lap dirt creep — accumulate slowly with on-
			 * track time so late joiners' welcome trailer
			 * carries plausible weathering. */
			{
				int i;
				for (i = 0; i < 5; i++)
					if (car_dirt[i] < 250)
						car_dirt[i] += 1;
			}

			/* End-of-lap damage + dirt sync.  0x43 broadcasts
			 * to peers, 0x46 only updates server-side cache
			 * (per Kunos pcap behaviour) but is needed for
			 * subsequent welcome trailers. */
			{
				size_t n = pkt_damage_zones(pkt, damage);
				send_tcp_framed(tcp_fd, pkt, n);
				n = pkt_car_dirt(pkt, car_dirt);
				send_tcp_framed(tcp_fd, pkt, n);
			}
		}

		/* Lateral-offset recovery — exponential decay toward
		 * the racing line.  Steering correction is implicit:
		 * the offset shrinks each tick, the reported position
		 * tracks the shrinking offset, and the bot rejoins the
		 * line over a few seconds. */
		lateral_offset *=
		    expf(-dt / LATERAL_RECOVERY_TAU_S);
		if (fabsf(lateral_offset) < 0.05f)
			lateral_offset = 0;

		/* Tyre wear: only ticks while on track. */
		if (loc == LOC_TRACK)
			on_track_ms += TICK_MS;

		/*
		 * Sector splits.  Three sectors of equal norm_pos length
		 * (0..1/3, 1/3..2/3, 2/3..1).  When the bot crosses a
		 * boundary, emit 0x21 with the just-finished sector's
		 * split time and the cumulative lap time so far.
		 */
		{
			int new_sector = (u_pos < 1.0f / 3.0f) ? 0 :
			    (u_pos < 2.0f / 3.0f) ? 1 : 2;
			if (new_sector != last_sector) {
				if (lap_start_ms == 0)
					lap_start_ms = client_ts;
				if (sector_start_ms == 0)
					sector_start_ms = client_ts;
				int32_t split = (int32_t)
				    (client_ts - sector_start_ms);
				int32_t lap_t = (int32_t)
				    (client_ts - lap_start_ms);
				/*
				 * Kunos wire convention (FUN_1400142f0
				 * dispatch table):
				 *   0x20 per-sector split for sectors 0/1
				 *   0x21 lap-complete event at S/F (sec-2
				 *        -> sec-0 transition)
				 * accd's h_sector_split_single (0x21) is the
				 * lap-complete handler that bumps lap_count
				 * and fires 0xd0 / ratings / penalty serve
				 * countdown.  Mirror the kunos shape so
				 * P/Q laps get counted server-side.
				 */
				if (last_sector == 2 && new_sector == 0) {
					/*
					 * Lap complete: 0x21 with the just-
					 * finished lap's full time in the
					 * first u32.  On the very first S/F
					 * crossing lap_t is 0 (timers were
					 * initialized this same iteration);
					 * tag the lap as IsOutLap (bit 2)
					 * so accd treats it as invalid and
					 * skips the 0xd0 emit + best-lap
					 * tracking, matching the real
					 * client's behaviour exiting pit.
					 */
					uint16_t car_field = (lap <= 1)
					    ? 0x0004u : 0x0000u;
					uint8_t pkt2[32];
					size_t n = pkt_sector_split(pkt2,
					    lap_t, lap_t,
					    (uint8_t)last_sector,
					    car_field);
					send_tcp_framed(tcp_fd, pkt2, n);
					lap_start_ms = client_ts;
				} else {
					/* Sectors 0 / 1: 0x20 per-sector
					 * split for the just-finished sector. */
					uint8_t pkt3[16];
					size_t m = pkt_sector_bulk(pkt3,
					    split, (uint8_t)last_sector,
					    lap_t, 0);
					send_tcp_framed(tcp_fd, pkt3, m);
				}
				sector_start_ms = client_ts;
				last_sector = new_sector;
			}
		}

		/*
		 * Mandatory pit served: emit 0x54 once per pit visit,
		 * after the bot has fully exited the pitlane (location
		 * went Pitlane / PitExit / ... → Track again).
		 */
		if (pit_on_lap > 0 && lap == pit_on_lap &&
		    last_loc != LOC_TRACK)
			pit_served_this_visit = 1;
		if (pit_served_this_visit && loc == LOC_TRACK) {
			uint8_t pkt2[16];
			if (mandatory_pit) {
				size_t n = pkt_mandatory_pit_served(pkt2,
				    (uint16_t)car_id);
				send_tcp_framed(tcp_fd, pkt2, n);
				printf("[bot] mandatory pit served (0x54) at "
				    "lap=%d u=%.3f\n", lap, u_pos);
			}
			if (send_penalty_served) {
				uint64_t ts = (uint64_t)client_ts;
				size_t n = pkt_penalty_cleared(pkt2, ts);
				send_tcp_framed(tcp_fd, pkt2, n);
				printf("[bot] penalty served (0x42) at lap=%d "
				    "u=%.3f ts=%llu\n", lap, u_pos,
				    (unsigned long long)ts);
			}
			pit_served_this_visit = 0;
		}

		waypoint_at(u_pos, &px, &py, &pz);

		/* Apply current lateral offset along the right-hand
		 * normal to the local tangent.  Tangent ≈ next-here on
		 * the racing line; right-hand normal in 2D = (-tz, tx). */
		if (fabsf(lateral_offset) > 0.01f) {
			float u_next = u_pos + 0.001f;
			float nx, ny, nz;
			waypoint_at(u_next, &nx, &ny, &nz);
			float tx = nx - px, tz = nz - pz;
			float tm = sqrtf(tx*tx + tz*tz);
			if (tm > 1e-4f) {
				/* Right-hand normal in the (x, z) plane. */
				float nrx = -tz / tm;
				float nrz =  tx / tm;
				px += lateral_offset * nrx;
				pz += lateral_offset * nrz;
			}
		}

		/* Velocity vector: tangent direction × v_current.  Tangent
		 * comes from the position delta between consecutive ticks;
		 * magnitude is the physics-smoothed speed.  This decouples
		 * "where the bot is heading" from "how fast it's going" so
		 * a sudden waypoint snap doesn't spike vec_c. */
		if (have_last) {
			float dx = px - last_x;
			float dz = pz - last_z;
			float dy = py - last_y;
			float mag = sqrtf(dx*dx + dy*dy + dz*dz);
			if (mag > 1e-4f) {
				float k = v_current / mag;
				vx = dx * k;
				vy = dy * k;
				vz = dz * k;
			} else {
				vx = vy = vz = 0;
			}
		} else {
			vx = vy = vz = 0;
		}
		last_x = px; last_y = py; last_z = pz;
		have_last = 1;
		last_loc = loc;

		/* Drain incoming TCP so the kernel rcv buffer doesn't
		 * back-pressure server sends.  We don't parse anything,
		 * but a 0-byte recv means the peer closed — trigger
		 * reconnect.  Same for any non-EAGAIN error. */
		{
			int disconnected = 0;
			/* Deliberate flap (--flap-at T): force a TCP close
			 * once at the configured tick.  The recv loop below
			 * then sees rn==0 and runs the reconnect cascade. */
			if (flap_at_tick >= 0 && !flap_done &&
			    (int)tick >= flap_at_tick) {
				printf("[bot] deliberate flap (--flap-at %d) "
				    "at tick %u\n",
				    flap_at_tick, (unsigned)tick);
				shutdown(tcp_fd, SHUT_RDWR);
				flap_done = 1;
			}
			while ((rn = recv(tcp_fd, rxbuf, sizeof rxbuf, 0)) > 0)
				;
			if (rn == 0) {
				disconnected = 1;
			} else if (rn < 0 && errno != EAGAIN &&
			    errno != EWOULDBLOCK && errno != EINTR) {
				disconnected = 1;
			}
			if (disconnected) {
				printf("[bot] disconnected (tcp), reconnecting "
				    "after %ums...\n", reconnect_backoff_ms);
				close(tcp_fd);
				close(udp_fd);
				{
					struct timespec ts = {
					    reconnect_backoff_ms / 1000,
					    (long)(reconnect_backoff_ms % 1000)
					        * 1000000L
					};
					nanosleep(&ts, NULL);
				}
				while (!g_stop) {
					size_t wl;
					if (connect_session(host, tcp_port,
					    name, "Driver", "BOT", steam,
					    race_number, driver_cat,
					    client_version, password,
					    &tcp_fd, &udp_fd,
					    &udp_peer, &conn_id, &car_id,
					    &wl) == 0) {
						reconnect_count++;
						printf("[bot] reconnected "
						    "#%d: conn=%u car=%u "
						    "(trailer %zu B), "
						    "resuming at u=%.3f "
						    "lap=%d\n",
						    reconnect_count, conn_id,
						    (unsigned)car_id, wl,
						    u_pos, lap);
						/* First UDP keepalive
						 * to register the peer
						 * with the new conn. */
						{
						    size_t kn =
							pkt_keepalive(pkt,
							    conn_id);
						    sendto(udp_fd, pkt, kn,
							0, (struct sockaddr *)
							&udp_peer,
							sizeof udp_peer);
						}
						reconnect_backoff_ms = 1000;
						break;
					}
					reconnect_backoff_ms =
					    reconnect_backoff_ms < 10000
					    ? reconnect_backoff_ms * 2
					    : 10000;
					printf("[bot] reconnect failed, "
					    "retry in %ums\n",
					    reconnect_backoff_ms);
					{
					struct timespec ts = {
					    reconnect_backoff_ms / 1000,
					    (long)(reconnect_backoff_ms % 1000)
					        * 1000000L
					};
					nanosleep(&ts, NULL);
				}
				}
				if (g_stop) break;
				/* Don't drain more bytes this tick — the
				 * fresh tcp_fd has nothing yet. */
				continue;
			}
		}

		/* Drain incoming UDP, answer keepalives. */ /* DBG */
		while ((rn = recv(udp_fd, rxbuf, sizeof rxbuf, 0)) > 0) {
			if (rn >= 7 && rxbuf[0] == 0x14) {
				uint32_t srv_ts =
				    rxbuf[1] | (rxbuf[2] << 8) |
				    (rxbuf[3] << 16) | (rxbuf[4] << 24);
				size_t n = pkt_pong(pkt, conn_id, srv_ts,
				    client_ts);
				sendto(udp_fd, pkt, n, 0,
				    (struct sockaddr *)&udp_peer,
				    sizeof udp_peer);
			}
		}

		if (tick % 30 == 0) {
			size_t n = pkt_keepalive(pkt, conn_id);
			sendto(udp_fd, pkt, n, 0,
			    (struct sockaddr *)&udp_peer, sizeof udp_peer);
		}

		/* 0x32 location updates go over TCP (framed); only 0x1e
		 * car_update + keepalives + pong travel on UDP. */
		if (tick % 5 == 0) {
			size_t n = pkt_location(pkt, (uint16_t)car_id, loc);
			send_tcp_framed(tcp_fd, pkt, n);
		}

		{
			struct CarInputs ci;
			float yaw_now = atan2f(vz, vx);
			float yaw_delta = have_yaw ? (yaw_now - prev_yaw) : 0.0f;
			int dsum, k;

			/* unwrap yaw_delta to (-pi, pi] so a wrap-around
			 * tick doesn't fake a hard steering input. */
			while (yaw_delta >  (float)M_PI) yaw_delta -= 2.0f * (float)M_PI;
			while (yaw_delta < -(float)M_PI) yaw_delta += 2.0f * (float)M_PI;

			ci.zero      = zero_inputs;
			ci.v_current = v_current;
			ci.v_target  = v_target;
			ci.yaw_delta = yaw_delta;
			/*
			 * Crude gear ladder: 1 at idle, +1 per ~12 m/s up to
			 * 7th.  Matches the bot's V_RACE = 85 m/s ceiling.
			 */
			ci.gear = (uint8_t)(1 + (int)(v_current / 12.0f));
			if (ci.gear > 7) ci.gear = 7;
			/*
			 * RPM: 1500 idle, climbing to ~7500 at V_RACE.
			 * Matches the AC2 HUD's typical band for a GT3 car.
			 */
			{
				int rpm_calc = 1500 +
				    (int)((v_current / V_RACE) * 6000.0f);
				if (rpm_calc < 0) rpm_calc = 0;
				if (rpm_calc > 65000) rpm_calc = 65000;
				ci.rpm = (uint16_t)rpm_calc;
			}
			/*
			 * Fuel: linear decay across 5 laps from full (255).
			 * Per-lap drain ≈ 51 bytes; bot rarely runs more
			 * than 3-4 laps in integration tests.
			 */
			{
				int f = 255 - lap * 51;
				if (f < 0) f = 0;
				ci.fuel = (uint8_t)f;
			}
			/* Damage: sum of the 5 zone bytes, clamped. */
			dsum = 0;
			for (k = 0; k < 5; k++)
				dsum += damage[k];
			if (dsum > 255) dsum = 255;
			ci.damage = (uint8_t)dsum;

			size_t n = pkt_car_update(pkt, conn_id,
			    (uint16_t)car_id, seq++, client_ts,
			    px, py, pz, vx, vy, vz, u_pos, &ci);
			sendto(udp_fd, pkt, n, 0,
			    (struct sockaddr *)&udp_peer, sizeof udp_peer);

			prev_yaw = yaw_now;
			have_yaw = 1;
		}

		if (tick % 60 == 0) {
			float vm = sqrtf(vx*vx + vy*vy + vz*vz);
			const char *phase = (lap == 0) ? "FORM" : "RACE";
			const char *lname =
			    (loc == LOC_PITENTRY) ? "PitEntry" :
			    (loc == LOC_PITLANE)  ? "Pitlane"  :
			    (loc == LOC_PITEXIT)  ? "PitExit"  : "Track";
			printf("[bot] t=%us lap=%d %s u=%.3f v=%.1f m/s "
			    "(%.0f km/h) loc=%s pos=(%.0f,%.0f)\n",
			    (unsigned)(client_ts / 1000), lap, phase, u_pos,
			    vm, vm * 3.6f, lname, px, pz);
		}

		/* Stagger multiple --report-penalty entries 20 ticks apart
		 * (~0.66 s) starting at penalty_start_tick (default 200,
		 * configurable via --penalty-start-tick so tests that need
		 * the report to land during a specific session phase can
		 * shift the burst later in the bot's lifetime). */
		if (rp_sent_n < rp_n &&
		    tick == penalty_start_tick + (uint32_t)(rp_sent_n * 20)) {
			int i = rp_sent_n;
			size_t n = pkt_report_penalty(pkt,
			    (uint8_t)rp_cat[i], (uint8_t)rp_kind[i],
			    (uint64_t)client_ts, rp_value[i]);
			send_tcp_framed(tcp_fd, pkt, n);
			printf("[bot] sent 0x41 #%d cat=%d kind=%d value=%d "
			    "ts=%u\n", i, rp_cat[i], rp_kind[i], rp_value[i],
			    (unsigned)client_ts);
			rp_sent_n++;
		}

		/* Voluntary or forced 0x4f driver-stint reset, fired once
		 * at the configured tick.  Body = u8 force + u64 ts. */
		if (stint_reset_tick >= 0 && !stint_reset_sent &&
		    (int)tick >= stint_reset_tick) {
			uint8_t pkt2[16];
			size_t n = pkt_driver_stint_reset(pkt2,
			    (uint8_t)stint_reset_force,
			    (uint64_t)client_ts);
			send_tcp_framed(tcp_fd, pkt2, n);
			printf("[bot] sent 0x4f stint reset force=%d ts=%u\n",
			    stint_reset_force, (unsigned)client_ts);
			stint_reset_sent = 1;
		}

		/* 0x55 garage load-setup, fired once at the configured tick.
		 * Server replies with 0x56 carrying the car's lap history
		 * for the requested session_type. */
		if (load_setup_tick >= 0 && !load_setup_sent &&
		    (int)tick >= load_setup_tick) {
			uint8_t pkt2[16];
			size_t n = pkt_load_setup(pkt2,
			    (uint8_t)load_setup_sess,
			    (uint16_t)car_id, 0);
			send_tcp_framed(tcp_fd, pkt2, n);
			printf("[bot] sent 0x55 load_setup sess_type=%d "
			    "car_id=%u\n", load_setup_sess,
			    (unsigned)car_id);
			load_setup_sent = 1;
		}

		/* 0x43 damage zones update, fired once at the configured
		 * tick.  Server broadcasts 0x44 over UDP. */
		if (damage_tick >= 0 && !damage_sent &&
		    (int)tick >= damage_tick) {
			uint8_t pkt2[16];
			size_t n = pkt_damage_zones(pkt2, damage_bytes);
			send_tcp_framed(tcp_fd, pkt2, n);
			printf("[bot] sent 0x43 damage [%u,%u,%u,%u,%u]\n",
			    damage_bytes[0], damage_bytes[1],
			    damage_bytes[2], damage_bytes[3],
			    damage_bytes[4]);
			damage_sent = 1;
		}

		/* 0x19 SA contact, fired once.  Server relays as 0x1b. */
		if (sa_tick >= 0 && !sa_sent && (int)tick >= sa_tick) {
			uint8_t pkt2[16];
			size_t n = pkt_sa_contact(pkt2, (uint16_t)car_id,
			    sa_target_race, (int32_t)client_ts, sa_quality);
			send_tcp_framed(tcp_fd, pkt2, n);
			printf("[bot] sent 0x19 SA contact target=%u quality=%u\n",
			    (unsigned)sa_target_race, (unsigned)sa_quality);
			sa_sent = 1;
		}

		/* 0x3d out-of-track, fired once with force=0.  Relayed 0x3c. */
		if (oot_tick >= 0 && !oot_sent && (int)tick >= oot_tick) {
			uint8_t pkt2[8];
			size_t n = pkt_out_of_track(pkt2, 0, (int32_t)client_ts);
			send_tcp_framed(tcp_fd, pkt2, n);
			printf("[bot] sent 0x3d out-of-track\n");
			oot_sent = 1;
		}

		/* 0x5e UDP time event, fired once.  Source=target=self with
		 * chat=1 so accd echoes a "Latency error" 0x2b to ourselves. */
		if (te_tick >= 0 && !te_sent && (int)tick >= te_tick) {
			uint8_t pkt2[24];
			size_t n = pkt_time_event(pkt2, conn_id, conn_id, 42, 1);
			sendto(udp_fd, pkt2, n, 0,
			    (struct sockaddr *)&udp_peer, sizeof udp_peer);
			printf("[bot] sent 0x5e time event\n");
			te_sent = 1;
		}

		/* 0x4a swap-state-request, fired once at the configured tick.
		 * Server applies + broadcasts 0x47 with the resulting state. */
		if (swap_req_tick >= 0 && !swap_req_sent &&
		    (int)tick >= swap_req_tick) {
			uint8_t pkt2[16];
			size_t n = pkt_swap_state_request(pkt2,
			    (uint16_t)car_id, swap_req_sub, swap_req_state);
			send_tcp_framed(tcp_fd, pkt2, n);
			printf("[bot] sent 0x4a swap_request sub=%u state=%u\n",
			    (unsigned)swap_req_sub, (unsigned)swap_req_state);
			swap_req_sent = 1;
		}

		/* 0x47 driver-swap-state update, fired once at the configured
		 * tick.  Server broadcasts 0x47 with the same body shape. */
		if (swap_state_tick >= 0 && !swap_state_sent &&
		    swap_state_count > 0 &&
		    (int)tick >= swap_state_tick) {
			uint8_t pkt2[32];
			size_t n = pkt_update_swap_state(pkt2,
			    (uint16_t)car_id, swap_state_bytes,
			    swap_state_count);
			send_tcp_framed(tcp_fd, pkt2, n);
			printf("[bot] sent 0x47 swap_state car_id=%u "
			    "dcnt=%u\n", (unsigned)car_id,
			    (unsigned)swap_state_count);
			swap_state_sent = 1;
		}

		/* Stagger --chat messages 20 ticks apart starting at
		 * chat_start_tick (default 60 = 2 s, so /admin <pw> fires
		 * well before any --report-penalty burst at tick 200). */
		if (chat_sent_n < chat_n &&
		    tick == chat_start_tick + (uint32_t)(chat_sent_n * 20)) {
			int i = chat_sent_n;
			size_t n = pkt_chat(pkt, name ? name : "bot",
			    chat_msgs[i], client_ts);
			send_tcp_framed(tcp_fd, pkt, n);
			printf("[bot] sent chat #%d: %s\n", i, chat_msgs[i]);
			chat_sent_n++;
		}

		tick++;
		nanosleep(&slp, NULL);
	}

	close(udp_fd);
	close(tcp_fd);
	printf("[bot] stopped after %d laps, %u ticks\n", lap,
	    (unsigned)tick);
	return 0;
}
