"""BLE scanning for the RID module, built on bleak.

The firmware (`main/broadcaster/ble_rid_broadcaster.cpp`) puts the serialized
GB 46750 packet inside the advertisement as **Service Data (AD type 0x16)
under the 16-bit UUID 0x0D50**, plus AD Flags and the local name
"ESP32S3_RID". Broadcasting uses BLE 5 extended advertising (1M primary PHY).

Packet extraction tries, in order:
  1. `advertisement_data.service_data` (bleak-normalized dict)
  2. raw advertisement bytes (`advertisement_data.data`, if the installed
     bleak version exposes it) parsed for AD type 0x16 / UUID 0x0D50
"""
from __future__ import annotations

import struct
from typing import Any

SERVICE_UUID_16BIT = 0x0D50
EXPECTED_NAME = "ESP32S3_RID"


def _match_uuid(key: str | int) -> bool:
    if isinstance(key, int):  # bleak may expose uuid as an int on some backends
        return key == SERVICE_UUID_16BIT
    k = key.strip().lower()
    if k in ("0d50", "00000d50", "d50"):
        return True
    try:
        return int(k, 16) == SERVICE_UUID_16BIT
    except ValueError:
        pass
    # canonical 128-bit form: 00000d50-0000-1000-8000-00805f9b34fb
    if len(k) == 36 and k.endswith("-0000-1000-8000-00805f9b34fb"):
        return k[:8].lstrip("0") == "d50"
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
        if any(_match_uuid(k) for k in adv.service_data):
            return True
    if hasattr(adv, "service_uuids"):
        if any(_match_uuid(str(u)) for u in adv.service_uuids):
            return True
    return False


def extract_packet(adv: Any) -> bytes | None:
    """Return the raw GB 46750 packet bytes, or None if not present."""
    # 1. bleak-normalized service_data dict
    sd = getattr(adv, "service_data", None)
    if sd:
        for key, data in sd.items():
            if _match_uuid(str(key)) and data:
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


def format_mac(mac: str) -> str:
    return mac.upper()
