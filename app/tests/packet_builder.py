"""Synthetic GB 46750-2025 packet builder.

Mirrors the ESP32-S3 encoder in `main/protocol/rid_messages.cpp` so the
decoder can be tested against the exact wire format the firmware produces.
"""
from __future__ import annotations

import struct

DATA_TYPE = 0xFF
VERSION = 0x20

# dataId bit masks
DID_REL_HEIGHT = 0x10
DID_VERT_SPEED = 0x08
DID_BARO_ALT = 0x02


def _le32(v: int) -> bytes:
    return struct.pack("<i", v)


def _le16(v: int) -> bytes:
    return struct.pack("<H", v)


def _encode_alt1000(alt: float) -> bytes:
    if alt != alt:  # NaN → unknown
        return _le16(0)
    return _le16(int(round((alt + 1000.0) * 2.0)))


def _encode_rel_height(h: float) -> bytes:
    return _le16(int(round((h + 9000.0) * 2.0)))


def _encode_heading(deg: float) -> bytes:
    return _le16(int(round(deg * 10.0)))


def _encode_speed(mps: float) -> bytes:
    return _le16(int(round(mps * 10.0)))


def _encode_vspeed(mps: float) -> bytes:
    dir_bit = 0x80 if mps < 0 else 0
    return bytes([dir_bit | int(round(abs(mps) * 2.0))])


def _encode_pos(lat: float, lon: float) -> bytes:
    return _le32(int(lat * 1e7)) + _le32(int(lon * 1e7))


def _encode_ts(ms: int) -> bytes:
    return (ms & 0xFFFFFFFFFFFFFFFF).to_bytes(6, "little")


def build_packet(
    uas_id: str = "CPNYMDL001234567890A",
    realname: str = "12345678",
    op_category: int = 1,
    ua_class: int = 1,
    op_loc_type: int = 0,
    op_lat: float = 31.2304,
    op_lon: float = 121.4737,
    op_alt: float = 50.0,
    ua_lat: float = 31.2305,
    ua_lon: float = 121.4738,
    heading: float = 45.6,
    speed: float = 3.5,
    rel_height: float | None = 120.0,
    vspeed: float | None = -1.5,
    geo_alt: float = 150.0,
    baro_alt: float | None = 149.0,
    op_status: int = 2,
    coord_sys: int = 0,
    horiz_acc: int = 10,
    vert_acc: int = 5,
    speed_acc: int = 3,
    timestamp_ms: int = 1700000000000,
    ts_acc: int = 5,
    version: int = VERSION,
    data_type: int = DATA_TYPE,
    ua_pos_unknown: bool = False,
) -> bytes:
    c = bytearray()
    uas = uas_id.encode("ascii")[:20].ljust(20, b"\x00")
    rn = realname.encode("ascii")[:8].ljust(8, b"\x00")
    c += uas + rn
    c += bytes([op_category, ua_class, op_loc_type])
    c += _encode_pos(op_lat, op_lon)
    c += _encode_alt1000(op_alt)
    c += (_le32(-1) + _le32(-1) if ua_pos_unknown else _encode_pos(ua_lat, ua_lon))
    c += _encode_heading(heading)
    c += _encode_speed(speed)
    if rel_height is not None:
        c += _encode_rel_height(rel_height)
    if vspeed is not None:
        c += _encode_vspeed(vspeed)
    c += _encode_alt1000(geo_alt)
    if baro_alt is not None:
        c += _encode_alt1000(baro_alt)
    c += bytes([op_status, coord_sys, horiz_acc, vert_acc, speed_acc])
    c += _encode_ts(timestamp_ms)
    c += bytes([ts_acc])

    data_id0 = 0xFF  # all bits
    data_id1 = 0xE5  # M bits (pos/heading/speed/geo_alt/ext)
    if rel_height is not None:
        data_id1 |= DID_REL_HEIGHT
    if vspeed is not None:
        data_id1 |= DID_VERT_SPEED
    if baro_alt is not None:
        data_id1 |= DID_BARO_ALT
    data_id2 = 0xFE  # all bits (status/coord/acc/timestamp/ts_acc)

    body = bytes([data_type, version, len(c)]) + bytes([data_id0, data_id1, data_id2]) + bytes(c)
    return body
