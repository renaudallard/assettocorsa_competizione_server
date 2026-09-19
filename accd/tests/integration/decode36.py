"""Decode ACP_LEADERBOARD (0x36) records.

Mirrors accd's write_session_leaderboard_section / write_car_
leaderboard_record so a test can assert on fields instead of counting
byte offsets by hand.  Frame layout:

    u8  0x36
    u32 session best lap        (INT32_MAX when unset)
    u8  3
    u32 session best sector x3
    u8  cvar8
    u16 car count
    <car record> x count
    u8  cvar8 echo + u8 0

and a car record:

    u16 car id, u16 race number, u8 car model, u8 cup, u16 status,
    u8 active-penalty flag [+ u16 wire + f32 remaining],
    [u8 mandatory-pit remaining, only when cvar8],
    u8 pq count + i32 x count,
    u8 driver count + per driver (4x str_a, u8 category, u16 nation),
    u16 current driver index, u32 best lap, u32 last lap,
    u16 lap count, u32 split time, u8 split id,
    u8 wide flag, u8 n + n x (u32|u16), u8 m + m x (u32|u16),
    u8 tail0, u8 tail1

str_a is a u8 codepoint count followed by that many u32 codepoints.

The last two fields before the lap lists are context dependent: a live
0x36 carries the session-relative time of the car's last split and which
split it was, a post-race 0x3e carries the elapsed race time and the
rating.  `split_ms` / `split_id` are named for the live case.
"""
import struct

SENTINEL = 0x7FFFFFFF
SPLIT_ID_NONE = 0xFF


class _R:
    def __init__(self, b):
        self.b, self.i = b, 0

    def u8(self):
        v = self.b[self.i]
        self.i += 1
        return v

    def u16(self):
        v = struct.unpack_from('<H', self.b, self.i)[0]
        self.i += 2
        return v

    def u32(self):
        v = struct.unpack_from('<I', self.b, self.i)[0]
        self.i += 4
        return v

    def str_a(self):
        # Read the count first: `self.i += 4 * self.u8()` would load
        # self.i before u8() advances it and lose that byte.
        n = self.u8()
        self.i += 4 * n


def decode(frame):
    """Return a dict of frame fields, or raise on a malformed frame.

    Each car carries `split_ms_off`, the absolute offset of its split
    time inside the frame, so a caller can normalise that field before
    a byte comparison.
    """
    r = _R(frame)
    if r.u8() != 0x36:
        raise ValueError('not a 0x36 frame')
    out = {'session_best_lap': r.u32()}
    if r.u8() != 3:
        raise ValueError('unexpected session best sector count')
    out['session_best_sectors'] = [r.u32() for _ in range(3)]
    cvar8 = r.u8()
    out['cvar8'] = cvar8
    cars = []
    for _ in range(r.u16()):
        car = {
            'car_id': r.u16(),
            'race_number': r.u16(),
            'car_model': r.u8(),
            'cup': r.u8(),
            'status': r.u16(),
        }
        if r.u8() == 1:
            r.u16()
            r.u32()
        if cvar8:
            r.u8()
        for _ in range(r.u8()):
            r.u32()
        for _ in range(r.u8()):
            r.str_a(), r.str_a(), r.str_a(), r.str_a()
            r.u8()
            r.u16()
        car['driver_index'] = r.u16()
        car['best_lap'] = r.u32()
        car['last_lap'] = r.u32()
        car['lap_count'] = r.u16()
        car['split_ms_off'] = r.i
        car['split_ms'] = r.u32()
        car['split_id'] = r.u8()
        wide = r.u8()
        for _ in range(2):
            n = r.u8()
            r.i += (4 if wide else 2) * n
        car['tail'] = (r.u8(), r.u8())
        cars.append(car)
    out['cars'] = cars
    out['trailer'] = (r.u8(), r.u8())
    if r.i != len(frame):
        raise ValueError(f'consumed {r.i} of {len(frame)} bytes')
    return out


def normalise_split_times(frame, decoded):
    """Blank every car's split time so two runs can be compared.

    The field holds the wall-clock moment a split happened, measured by
    whichever server saw it, so two independent runs never agree on it
    to the millisecond.
    """
    out = bytearray(frame)
    for car in decoded['cars']:
        if car['split_ms'] == SENTINEL:
            continue
        out[car['split_ms_off']:car['split_ms_off'] + 4] = b'\x00\x00\x00\x00'
    return bytes(out)
