"""BLE scanner extraction tests (mirror firmware AD layout)."""
from __future__ import annotations

from packet_builder import build_packet

from rid.ble_scanner import (
    CONFIG_MAGIC,
    CONFIG_UUID_16BIT,
    SERVICE_UUID_16BIT,
    _match_uuid,
    extract_gb_from_adv,
    extract_packet,
    is_config_target,
    is_target,
)

# firmware writes RID_SERVICE_UUID (0xFFFF) little-endian:
#   *p++ = RID_SERVICE_UUID & 0xFF      -> 0xFF
#   *p++ = (RID_SERVICE_UUID >> 8) & 0xFF -> 0xFF
UUID_LE = b"\xff\xff"


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
    assert _match_uuid("ffff", SERVICE_UUID_16BIT)
    assert _match_uuid("0000ffff", SERVICE_UUID_16BIT)
    assert _match_uuid("0000ffff-0000-1000-8000-00805f9b34fb", SERVICE_UUID_16BIT)
    assert _match_uuid(0xFFFF, SERVICE_UUID_16BIT)
    assert _match_uuid(65535, SERVICE_UUID_16BIT)
    assert not _match_uuid("1234", SERVICE_UUID_16BIT)
    assert not _match_uuid("00001234-0000-1000-8000-00805f9b34fb", SERVICE_UUID_16BIT)
    assert not _match_uuid(0x1234, SERVICE_UUID_16BIT)


def test_extract_from_service_data_dict():
    raw = build_packet()
    adv = _fake_adv(sd={"0000ffff-0000-1000-8000-00805f9b34fb": raw})
    assert extract_packet(adv) == raw


def test_extract_from_raw_ad_bytes():
    """Raw AD bytes: flags + name + service data (pure GB packet, no header)."""
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
    """Fragmented Service Data across two AD structures."""
    raw = build_packet()
    ad = (
        _ad_struct(0x16, UUID_LE + raw[:40])
        + _ad_struct(0x16, UUID_LE + raw[40:])
    )
    assert extract_packet(_fake_adv(data=ad)) == raw


def test_is_target_by_name_and_uuid():
    assert is_target("GBI_RID_001", _fake_adv())
    assert is_target("", _fake_adv(uuids=["0000ffff-0000-1000-8000-00805f9b34fb"]))
    assert is_target("", _fake_adv(sd={"ffff": build_packet()}))
    assert not is_target("OTHER_DEV", _fake_adv(uuids=["0000aaaa-0000-1000-8000-00805f9b34fb"]))
    assert not is_target("", _fake_adv())


def test_extract_returns_none_when_absent():
    adv = _fake_adv(sd={"0000aaaa-0000-1000-8000-00805f9b34fb": b"\x01\x02"})
    assert extract_packet(adv) is None
    assert extract_packet(_fake_adv(data=b"\x02\x01\x06")) is None


# --- is_config_target: ground connectable adv recognized by 0xFFF0 magic ---

# firmware writes RID_GATT_SVC_UUID16 (0xFFF0) little-endian in Service Data
CONFIG_UUID_LE = b"\xf0\xff"


def _config_ad(name=b"GBI_RID_001") -> bytes:
    """地面可连接广播 AD 字节：Flags + 名称 + Service UUID 列表(0xFFF0) + Service Data 魔数。"""
    return (
        _ad_struct(0x01, b"\x06")
        + _ad_struct(0x09, name)
        + _ad_struct(0x03, CONFIG_UUID_LE)
        + _ad_struct(0x16, CONFIG_UUID_LE + CONFIG_MAGIC)
    )


def test_match_config_uuid_forms():
    assert _match_uuid("fff0", CONFIG_UUID_16BIT)
    assert _match_uuid("0000fff0", CONFIG_UUID_16BIT)
    assert _match_uuid("0000fff0-0000-1000-8000-00805f9b34fb", CONFIG_UUID_16BIT)
    assert _match_uuid(0xFFF0, CONFIG_UUID_16BIT)
    assert _match_uuid(65520, CONFIG_UUID_16BIT)
    assert not _match_uuid("ffff", CONFIG_UUID_16BIT)
    assert not _match_uuid(0xFFFF, CONFIG_UUID_16BIT)
    assert not _match_uuid("1234", CONFIG_UUID_16BIT)


def test_is_config_target_via_service_data_dict():
    # bleak 规范化 service_data：key 可能是 str（128-bit）或 int
    adv = _fake_adv(sd={"0000fff0-0000-1000-8000-00805f9b34fb": CONFIG_MAGIC})
    assert is_config_target(adv)
    adv = _fake_adv(sd={0xFFF0: CONFIG_MAGIC})
    assert is_config_target(adv)


def test_is_config_target_via_raw_ad_bytes():
    assert is_config_target(_fake_adv(data=_config_ad()))


def test_is_config_target_via_winrt_platform_data():
    assert is_config_target(_fake_adv(platform_data=("sender", _config_ad())))


def test_is_config_target_false_without_magic():
    # 只有 0xFFF0 Service UUID 列表、无魔数载荷 → 不算本模块（旧固件 / 仿冒）
    adv = _fake_adv(
        uuids=["0000fff0-0000-1000-8000-00805f9b34fb"],
        data=_ad_struct(0x16, CONFIG_UUID_LE + b"\x00\x00\x00\x00\x00\x00"),
    )
    assert not is_config_target(adv)
    # 只有 0xFFF0 UUID 列表，无 Service Data
    assert not is_config_target(_fake_adv(uuids=["0000fff0-0000-1000-8000-00805f9b34fb"]))
    # 纯名称匹配不够
    assert not is_config_target(_fake_adv(name="GBI_RID_001"))
    # 空广播
    assert not is_config_target(_fake_adv())


def test_is_config_target_ignores_air_broadcast():
    # 空中态广播（Service Data 0xFFFF 纯 GB 包）不携带魔数 → 不是配置目标
    raw = build_packet()
    adv = _fake_adv(sd={"ffff": raw})
    assert is_target("", adv)
    assert not is_config_target(adv)
    # raw 字节形式的空中广播同样不算配置目标
    adv_raw = _fake_adv(data=_ad_struct(0x16, UUID_LE + raw))
    assert not is_config_target(adv_raw)


# --- extract_gb_from_adv: normalize pasted hex for manual decode ---


def test_extract_gb_from_bare_packet_passthrough():
    raw = build_packet()
    assert raw[0] == 0xFF
    assert extract_gb_from_adv(raw) == raw


def test_extract_gb_from_full_ad_frame():
    """nRF Connect 'Raw' field: flags + name + service data."""
    raw = build_packet()
    ad = (
        _ad_struct(0x01, b"\x06")
        + _ad_struct(0x09, b"ESP32C5_RIDQ")
        + _ad_struct(0x16, UUID_LE + raw)
    )
    assert extract_gb_from_adv(ad) == raw


def test_extract_gb_from_fragmented_ad():
    """Fragmented AD spanning two Service Data structures."""
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
