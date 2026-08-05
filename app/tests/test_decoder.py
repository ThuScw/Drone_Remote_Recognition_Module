"""Decoder tests against the firmware wire format."""
from __future__ import annotations

import math

from packet_builder import DATA_TYPE, VERSION, build_packet

from rid.decoder import decode_gb_packet, parse_hex
from rid.models import DecodedPacket


def test_full_packet_decodes():
    raw = build_packet()
    pkt = decode_gb_packet(raw, address="AA:BB:CC:DD:EE:FF", rssi=-55)

    assert isinstance(pkt, DecodedPacket)
    assert pkt.structure_error == ""
    assert pkt.address == "AA:BB:CC:DD:EE:FF"
    assert pkt.rssi == -55
    assert pkt.data_type == DATA_TYPE
    assert pkt.version == VERSION
    assert pkt.declared_len == len(raw) - 6
    assert pkt.content_len == len(raw) - 6

    assert pkt.uas_id == "CPNYMDL001234567890A"
    assert pkt.realname == "12345678"
    assert pkt.op_category == 1
    assert pkt.ua_class == 1
    assert pkt.op_loc_type == 0
    assert math.isclose(pkt.op_lat, 31.2304, abs_tol=1e-7)
    assert math.isclose(pkt.op_lon, 121.4737, abs_tol=1e-7)
    assert math.isclose(pkt.op_alt, 50.0, abs_tol=0.1)
    assert math.isclose(pkt.ua_lat, 31.2305, abs_tol=1e-7)
    assert math.isclose(pkt.ua_lon, 121.4738, abs_tol=1e-7)
    assert math.isclose(pkt.heading, 45.6, abs_tol=0.1)
    assert math.isclose(pkt.speed, 3.5, abs_tol=0.1)
    assert pkt.has_rel_height and math.isclose(pkt.rel_height, 120.0, abs_tol=0.1)
    assert pkt.has_vspeed and math.isclose(pkt.vspeed, -1.5, abs_tol=0.1)
    assert math.isclose(pkt.geo_alt, 150.0, abs_tol=0.1)
    assert pkt.has_baro_alt and math.isclose(pkt.baro_alt, 149.0, abs_tol=0.1)
    assert pkt.op_status == 2
    assert pkt.coord_sys == 0
    assert pkt.horiz_acc == 10 and pkt.vert_acc == 5 and pkt.speed_acc == 3
    assert pkt.timestamp_ms == 1700000000000
    assert pkt.ts_acc == 5


def test_optional_fields_absent():
    raw = build_packet(rel_height=None, vspeed=None, baro_alt=None)
    pkt = decode_gb_packet(raw)

    assert pkt.structure_error == ""
    assert not pkt.has_rel_height
    assert not pkt.has_vspeed
    assert not pkt.has_baro_alt
    assert pkt.fmt["相对高度_m"] == "未广播"
    # content parsing must not desync: following fields still correct
    assert pkt.op_status == 2
    assert math.isclose(pkt.geo_alt, 150.0, abs_tol=0.1)
    assert pkt.timestamp_ms == 1700000000000


def test_version_wrong_is_flagged():
    raw = build_packet(version=0x01)
    pkt = decode_gb_packet(raw)
    assert "版本错误" in pkt.structure_error


def test_truncated_packet_does_not_crash():
    raw = build_packet()
    pkt = decode_gb_packet(raw[:20])
    # partial parse, no exception, some structure flag
    assert isinstance(pkt, DecodedPacket)
    assert pkt.uas_id != ""  # header bytes made it
    pkt2 = decode_gb_packet(raw[:3])
    assert "过短" in pkt2.structure_error
    assert pkt2.raw == raw[:3]


def test_unknown_sentinels_decoded_as_nan():
    pkt = decode_gb_packet(build_packet(ua_pos_unknown=True))
    assert math.isnan(pkt.ua_lat) and math.isnan(pkt.ua_lon)
    assert pkt.fmt["无人机位置"] == "未知, 未知"


def test_parse_hex_accepts_common_formats():
    assert parse_hex("FF 20 40") == b"\xff\x20\x40"
    assert parse_hex("ff 20,40") == b"\xff\x20\x40"
    assert parse_hex("0xFF 0x20") == b"\xff\x20"
    assert parse_hex("") == b""
    # Continuous run without separators (nRF Connect copy format)
    assert parse_hex("FF2040") == b"\xff\x20\x40"
    assert parse_hex("ff2040") == b"\xff\x20\x40"
    assert parse_hex("0x0201060C09") == b"\x02\x01\x06\x0c\x09"
    # Mixed separators + continuous token
    assert parse_hex("FF20 46,0x1B") == b"\xff\x20\x46\x1b"


def test_parse_hex_rejects_bad_input():
    import pytest
    # Odd-length continuous run is ambiguous
    with pytest.raises(ValueError):
        parse_hex("FF2")
    # Non-hex characters
    with pytest.raises(ValueError):
        parse_hex("FF G0")
