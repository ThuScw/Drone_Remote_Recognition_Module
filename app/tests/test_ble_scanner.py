"""BLE scanner extraction tests (mirror firmware AD layout)."""
from __future__ import annotations

from packet_builder import build_packet

from rid.ble_scanner import _match_uuid, extract_gb_from_adv, extract_packet, is_target

# firmware writes RID_SERVICE_UUID (0x0D50) little-endian:
#   *p++ = RID_SERVICE_UUID & 0xFF      -> 0x50
#   *p++ = (RID_SERVICE_UUID >> 8) & 0xFF -> 0x0D
UUID_LE = b"\x50\x0d"


def _ad_struct(typ: int, payload: bytes) -> bytes:
    return bytes([len(payload) + 1, typ]) + payload


def _fake_adv(sd=None, data=None, uuids=None, name="", platform_data=None):
    class FakeAdv:
        pass

    adv = FakeAdv()
    adv.service_data = sd or {}
    adv.data = data
    adv.service_uuids = uuids or []
    adv.local_name = name
    adv.platform_data = platform_data
    return adv


def test_match_uuid_forms():
    assert _match_uuid("0d50")
    assert _match_uuid("00000d50")
    assert _match_uuid("00000d50-0000-1000-8000-00805f9b34fb")
    assert _match_uuid(0x0D50)
    assert _match_uuid(3408)
    assert not _match_uuid("1234")
    assert not _match_uuid("00001234-0000-1000-8000-00805f9b34fb")
    assert not _match_uuid(0x1234)


def test_extract_from_service_data_dict():
    raw = build_packet()
    adv = _fake_adv(sd={"00000d50-0000-1000-8000-00805f9b34fb": raw})
    assert extract_packet(adv) == raw


def test_extract_from_raw_ad_bytes():
    raw = build_packet()
    ad = (
        _ad_struct(0x01, b"\x06")
        + _ad_struct(0x09, b"GBI_RID_001")
        + _ad_struct(0x16, UUID_LE + raw)
    )
    assert extract_packet(_fake_adv(data=ad)) == raw


def test_extract_from_winrt_platform_data():
    """bleak 3.x winrt: platform_data = (sender, raw_ad_bytes)."""
    raw = build_packet()
    ad = (
        _ad_struct(0x01, b"\x06")
        + _ad_struct(0x09, b"GBI_RID_001")
        + _ad_struct(0x16, UUID_LE + raw)
    )
    assert extract_packet(_fake_adv(platform_data=("sender", ad))) == raw


def test_extract_from_fragmented_raw_ad():
    raw = build_packet()
    ad = (
        _ad_struct(0x16, UUID_LE + raw[:40])
        + _ad_struct(0x16, UUID_LE + raw[40:])
    )
    assert extract_packet(_fake_adv(data=ad)) == raw


def test_is_target_by_name_and_uuid():
    assert is_target("GBI_RID_001", _fake_adv())
    assert is_target("", _fake_adv(uuids=["00000d50-0000-1000-8000-00805f9b34fb"]))
    assert is_target("", _fake_adv(sd={"0d50": build_packet()}))
    assert not is_target("OTHER_DEV", _fake_adv(uuids=["0000aaaa-0000-1000-8000-00805f9b34fb"]))
    assert not is_target("", _fake_adv())


def test_extract_returns_none_when_absent():
    adv = _fake_adv(sd={"0000aaaa-0000-1000-8000-00805f9b34fb": b"\x01\x02"})
    assert extract_packet(adv) is None
    assert extract_packet(_fake_adv(data=b"\x02\x01\x06")) is None


# --- extract_gb_from_adv: normalize pasted hex for manual decode ---


def test_extract_gb_from_bare_packet_passthrough():
    raw = build_packet()
    assert raw[0] == 0xFF
    assert extract_gb_from_adv(raw) == raw


def test_extract_gb_from_full_ad_frame():
    """nRF Connect 'Raw' field: flags + name + service data wrapping the GB packet."""
    raw = build_packet()
    ad = (
        _ad_struct(0x01, b"\x06")
        + _ad_struct(0x09, b"ESP32C5_RIDQ")
        + _ad_struct(0x16, UUID_LE + raw)
    )
    assert extract_gb_from_adv(ad) == raw


def test_extract_gb_from_fragmented_ad():
    raw = build_packet()
    ad = (
        _ad_struct(0x16, UUID_LE + raw[:40])
        + _ad_struct(0x16, UUID_LE + raw[40:])
    )
    assert extract_gb_from_adv(ad) == raw


def test_extract_gb_from_empty_and_unrelated_passthrough():
    assert extract_gb_from_adv(b"") == b""
    assert extract_gb_from_adv(b"\x02\x01\x06") == b"\x02\x01\x06"


def test_extract_gb_from_non_ff_service_data_passthrough():
    # service data present but payload doesn't start with dataType 0xFF
    ad = _ad_struct(0x16, b"\x34\x12" + b"\x01\x02\x03")
    assert extract_gb_from_adv(ad) == ad
