"""BLE scanning for the RID module, built on bleak.

The firmware (`main/broadcaster/ble_rid_broadcaster.cpp`) puts the serialized
GB 46750 packet inside the advertisement as **Service Data (AD type 0x16)
under the 16-bit UUID 0xFFFF**, plus AD Flags and the local name
"GBI_RID_001". Broadcasting uses BLE 5 extended advertising (1M primary PHY).

Packet extraction tries, in order:
  1. `advertisement_data.service_data` (bleak-normalized dict)
  2. raw advertisement bytes (`advertisement_data.data`, if the installed
     bleak version exposes it) parsed for AD type 0x16 / UUID 0xFFFF
"""
from __future__ import annotations

import struct
from typing import Any

SERVICE_UUID_16BIT = 0xFFFF
EXPECTED_NAME = "GBI_RID_001"

# 地面可连接广播的识别魔数 (Service Data UUID 0xFFF0 的载荷)
# 与固件 main/gatt/rid_config.h 的 RID_CONFIG_MAGIC 一致: 5 字节 ASCII "GBRID" + 1 字节版本 0x01
CONFIG_UUID_16BIT = 0xFFF0
CONFIG_MAGIC = b"GBRID\x01"


def _match_uuid(key: str | int, uuid16: int) -> bool:
    """匹配 16-bit 服务 UUID（bleak 后端 key 可能是 int 或 str）。"""
    if isinstance(key, int):  # bleak may expose uuid as an int on some backends
        return key == uuid16
    k = str(key).strip().lower()
    h = f"{uuid16:04x}"
    if k in (h, f"0000{h}"):
        return True
    try:
        return int(k, 16) == uuid16
    except ValueError:
        pass
    # canonical 128-bit form: 0000ffff-0000-1000-8000-00805f9b34fb
    if len(k) == 36 and k.endswith("-0000-1000-8000-00805f9b34fb"):
        return k[:8].lstrip("0") == h
    return False


def is_config_target(adv: Any) -> bool:
    """True 当且仅当该广播是本模块的地面可配置态：Service Data (0xFFF0) 载荷精确匹配魔数。

    不依赖广播名；即使其他设备广播 0xFFF0 服务（UUID 列表或 Service Data）也不会被误认。
    """
    sd = getattr(adv, "service_data", None)
    if sd:
        for key, data in sd.items():
            if _match_uuid(key, CONFIG_UUID_16BIT) and data and bytes(data) == CONFIG_MAGIC:
                return True

    raw = getattr(adv, "data", None)
    if not raw:
        pd = getattr(adv, "platform_data", None)
        if (
            isinstance(pd, tuple)
            and len(pd) >= 2
            and isinstance(pd[1], (bytes, bytearray, memoryview))
        ):
            raw = pd[1]
    if raw:
        if _parse_ad_service_data(bytes(raw)).get(CONFIG_UUID_16BIT) == CONFIG_MAGIC:
            return True
    return False


def _parse_ad_service_data(raw: bytes) -> dict[int, bytes]:
    """Parse raw advertisement bytes for AD type 0x16 (Service Data, 16-bit UUID)."""
    out: dict[int, bytes] = {}
    i, n = 0, len(raw)
    while i < n:
        length = raw[i]
        if length == 0 or i + 1 + length > n:
            break
        typ = raw[i + 1]
        data = raw[i + 2:i + 1 + length]
        if typ == 0x16 and len(data) >= 2:
            uuid16 = struct.unpack_from("<H", data, 0)[0]
            out[uuid16] = out.get(uuid16, b"") + data[2:]
        i += 1 + length
    return out


def is_target(device_name: str, adv: Any) -> bool:
    """True if the advertisement is (probably) from our RID module."""
    name = (device_name or "").strip()
    if name == EXPECTED_NAME:
        return True
    if hasattr(adv, "service_data"):
        if any(_match_uuid(k, SERVICE_UUID_16BIT) for k in adv.service_data):
            return True
    if hasattr(adv, "service_uuids"):
        if any(_match_uuid(str(u), SERVICE_UUID_16BIT) for u in adv.service_uuids):
            return True
    return False


def extract_packet(adv: Any) -> bytes | None:
    """Return the raw GB 46750 packet bytes, or None if not present."""
    # 1. bleak-normalized service_data dict
    sd = getattr(adv, "service_data", None)
    if sd:
        for key, data in sd.items():
            if _match_uuid(str(key), SERVICE_UUID_16BIT) and data:
                return bytes(data)

    # 2. raw AD bytes: `adv.data` (older bleak) or winrt `platform_data`
    #    which is a (sender, raw_bytes) tuple in bleak 3.x.
    raw = getattr(adv, "data", None)
    if not raw:
        pd = getattr(adv, "platform_data", None)
        if (
            isinstance(pd, tuple)
            and len(pd) >= 2
            and isinstance(pd[1], (bytes, bytearray, memoryview))
        ):
            raw = pd[1]
    if raw:
        found = _parse_ad_service_data(bytes(raw)).get(SERVICE_UUID_16BIT)
        if found:
            return bytes(found)
    return None


def extract_gb_from_adv(raw: bytes) -> bytes:
    """Normalize pasted bytes into the raw GB 46750 packet.

    Accepts either the bare packet (starts with dataType 0xFF, as produced by
    the firmware serializer) or a full BLE advertising frame copied from a
    sniffer (nRF Connect "Raw" field), pulling the packet out of the Service
    Data AD.
    If no GB packet is found, returns `raw` unchanged so the caller's
    error reporting still shows the actual header bytes.
    """
    if not raw or raw[0] == 0xFF:
        return raw
    for payload in _parse_ad_service_data(raw).values():
        if payload.startswith(b"\xff"):
            return payload
    return raw


def format_mac(mac: str) -> str:
    return mac.upper()
