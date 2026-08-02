"""GB 46750-2025 packet decoder.

Matches the ESP32-S3 firmware serialization in
`main/protocol/rid_messages.cpp`:

    wire[0] = dataType    (0xFF)
    wire[1] = version     (0x20 = V1.0)
    wire[2] = dataLength  (content bytes)
    wire[3..5] = dataId   (3 bytes, bit flags)
    wire[6..]  = content

Decoding is defensive: a truncated or malformed packet yields what could be
recovered plus a `structure_error` message, instead of raising.
"""
from __future__ import annotations

import struct
from typing import Any

from .models import DecodedPacket

GB46750_DATA_TYPE = 0xFF
GB46750_VERSION = 0x20  # V1.0

# dataId bit masks (Byte 1) — optional-field presence
DID_REL_HEIGHT = 0x10  # 011 相对高度
DID_VERT_SPEED = 0x08  # 012 垂直速度
DID_BARO_ALT = 0x02    # 014 气压高度

# Sentinel values the encoder writes for "unknown / unavailable"
SENT_LATLON = 0xFFFFFFFF  # positions (int32 -1)
SENT_SPEED_HEADING = 0xFFFF  # speed / heading
SENT_VSPEED = 0xFF  # vertical speed

OP_STATUS = {
    0: "未报告",
    1: "地面",
    2: "空中",
    3: "紧急状态",
    4: "识别发送功能失效(非紧急)",
    5: "识别发送功能失效(紧急)",
}
HORIZ_ACC = {
    0: ">=18.52km / 未知", 1: "<18.52km", 2: "<7.41km", 3: "<3.70km", 4: "<1852m",
    5: "<926m", 6: "<556m", 7: "<185m", 8: "<92.6m", 9: "<30m", 10: "<10m",
    11: "<3m", 12: "<1m",
}
VERT_ACC = {0: ">=150m / 未知", 1: "<150m", 2: "<45m", 3: "<25m", 4: "<10m", 5: "<3m", 6: "<1m"}
SPEED_ACC = {0: ">=10m/s / 未知", 1: "<10m/s", 2: "<3m/s", 3: "<1m/s", 4: "<0.3m/s"}
TS_ACC = {
    0: ">0.5s / 未知", 1: "<=0.5s", 2: "<=0.4s", 3: "<=0.3s", 4: "<=0.2s", 5: "<=0.1s",
    6: "<=50ms", 7: "<=20ms", 8: "<=10ms",
}


def _fmt_coord(v: float) -> str:
    return "未知" if v != v or abs(v) > 360 else f"{v:.7f}"


def _fmt_alt(v: float) -> str:
    return "未知" if v != v else f"{v:.1f}"


def _fmt_speed(v: float) -> str:
    return "未知" if v != v else f"{v:.1f}"


def parse_hex(text: str) -> bytes:
    """Parse a whitespace/colon/comma separated hex string into bytes.

    Accepts forms like:
      "FF 20 46 ..."
      "ff20 46, 0x1B ..."
    """
    cleaned = text.replace("0x", "").replace("0X", "")
    parts = [p for p in cleaned.replace(",", " ").replace(":", " ").split() if p]
    if not parts:
        return b""
    return bytes(int(p, 16) for p in parts)


def decode_gb_packet(data: bytes, **meta: Any) -> DecodedPacket:
    """Decode a serialized GB 46750 packet into a DecodedPacket."""
    pkt = DecodedPacket(raw=bytes(data))
    pkt.address = meta.get("address", "")
    pkt.rssi = meta.get("rssi", 0)
    pkt.received_at_ms = meta.get("received_at_ms", 0)
    pkt.source = meta.get("source", "ble")

    if len(data) < 6:
        pkt.structure_error = f"数据包过短 ({len(data)}B < 6B 头部)"
        return pkt

    pkt.data_type = data[0]
    pkt.version = data[1]
    pkt.declared_len = data[2]
    pkt.data_id = data[3:6]
    content = data[6:]
    pkt.content_len = len(content)

    if pkt.data_type != GB46750_DATA_TYPE:
        pkt.structure_error = f"dataType 错误: 0x{pkt.data_type:02X} (应为 0xFF)"
    elif pkt.version != GB46750_VERSION:
        pkt.structure_error = (
            f"版本错误: 0x{pkt.version:02X} (应为 0x20=V1.0，收到的可能是旧版或错包)"
        )
    elif pkt.declared_len != pkt.content_len:
        pkt.structure_error = (
            f"dataLength 不匹配: 声明 {pkt.declared_len}B, 实际 {pkt.content_len}B"
        )

    # Even with a structure error, still try to parse content for visibility.
    _parse_content(pkt, content, pkt.data_id[1] if len(pkt.data_id) >= 2 else 0)
    return pkt


def _parse_content(pkt: DecodedPacket, c: bytes, data_id_1: int) -> None:
    """Parse content bytes in firmware field order (see rid_messages.cpp)."""
    pos = 0
    n = len(c)

    def take(size: int) -> bytes:
        nonlocal pos
        end = pos + size
        if end > n:
            end = n
        chunk = c[pos:end]
        pos = end
        return chunk

    def need(size: int) -> bool:
        return pos + size <= n

    # 001 唯一产品识别码 (20 ASCII)
    pkt.uas_id = take(20).rstrip(b"\x00").decode("ascii", errors="replace")

    # 002 实名登记标志 (8 ASCII)
    pkt.realname = take(8).rstrip(b"\x00").decode("ascii", errors="replace")

    # 003-005 one-byte enums
    if need(1):
        pkt.op_category = c[pos]; pos += 1
    if need(1):
        pkt.ua_class = c[pos]; pos += 1
    if need(1):
        pkt.op_loc_type = c[pos]; pos += 1

    # 006 遥控站位置 (int32 LE x2, deg*1e7)
    if need(8):
        lat_i, lon_i = struct.unpack_from("<ii", c, pos); pos += 8
        if lat_i == -1 or lon_i == -1:
            pkt.op_lat = pkt.op_lon = float("nan")
        else:
            pkt.op_lat, pkt.op_lon = lat_i / 1e7, lon_i / 1e7

    # 007 遥控站高度 (uint16 LE, (val+1000)*2)
    if need(2):
        v = struct.unpack_from("<H", c, pos)[0]; pos += 2
        pkt.op_alt = v / 2.0 - 1000.0 if v else float("nan")

    # 008 无人机位置
    if need(8):
        lat_i, lon_i = struct.unpack_from("<ii", c, pos); pos += 8
        if lat_i == -1 or lon_i == -1:
            pkt.ua_lat = pkt.ua_lon = float("nan")
        else:
            pkt.ua_lat, pkt.ua_lon = lat_i / 1e7, lon_i / 1e7

    # 009 航迹角 (uint16 LE, *0.1 deg), 010 地速 (uint16 LE, *0.1 m/s)
    if need(2):
        v = struct.unpack_from("<H", c, pos)[0]; pos += 2
        pkt.heading = v / 10.0 if v != SENT_SPEED_HEADING else float("nan")
    if need(2):
        v = struct.unpack_from("<H", c, pos)[0]; pos += 2
        pkt.speed = v / 10.0 if v != SENT_SPEED_HEADING else float("nan")

    # 011 相对高度 (O, uint16 LE, (val+9000)*2)
    if data_id_1 & DID_REL_HEIGHT:
        if need(2):
            v = struct.unpack_from("<H", c, pos)[0]; pos += 2
            pkt.rel_height = v / 2.0 - 9000.0
            pkt.has_rel_height = True

    # 012 垂直速度 (O, 1 byte, bit7=dir, bits6-0=*0.5 m/s)
    if data_id_1 & DID_VERT_SPEED:
        if need(1):
            b = c[pos]; pos += 1
            pkt.has_vspeed = True
            if b == SENT_VSPEED:
                pkt.vspeed = float("nan")
            else:
                val = b & 0x7F
                pkt.vspeed = (-val / 2.0) if (b & 0x80) else (val / 2.0)

    # 013 大地高度 (uint16 LE, (val+1000)*2)
    if need(2):
        v = struct.unpack_from("<H", c, pos)[0]; pos += 2
        pkt.geo_alt = v / 2.0 - 1000.0 if v else float("nan")

    # 014 气压高度 (O, uint16 LE, (val+1000)*2)
    if data_id_1 & DID_BARO_ALT:
        if need(2):
            v = struct.unpack_from("<H", c, pos)[0]; pos += 2
            pkt.baro_alt = v / 2.0 - 1000.0 if v else float("nan")
            pkt.has_baro_alt = True

    # 015 运行状态, 016 坐标系
    if need(1):
        pkt.op_status = c[pos]; pos += 1
    if need(1):
        pkt.coord_sys = c[pos]; pos += 1

    # 017-019 精度
    if need(1):
        pkt.horiz_acc = c[pos]; pos += 1
    if need(1):
        pkt.vert_acc = c[pos]; pos += 1
    if need(1):
        pkt.speed_acc = c[pos]; pos += 1

    # 020 时间戳 (uint48 LE, ms)
    if need(6):
        ts_bytes = c[pos:pos + 6] + b"\x00\x00"; pos += 6
        pkt.timestamp_ms = struct.unpack_from("<Q", ts_bytes, 0)[0]

    # 021 时间戳精度
    if need(1):
        pkt.ts_acc = c[pos]; pos += 1

    pkt.content_len = pos

    # --- human-readable summary ---
    pkt.fmt = {
        "唯一产品识别码": pkt.uas_id or "(空)",
        "实名登记号": pkt.realname or "(空)",
        "运行类别": str(pkt.op_category) if pkt.op_category >= 0 else "-",
        "无人机分类": str(pkt.ua_class) if pkt.ua_class >= 0 else "-",
        "遥控站位置类型": str(pkt.op_loc_type) if pkt.op_loc_type >= 0 else "-",
        "遥控站位置": f"{_fmt_coord(pkt.op_lat)}, {_fmt_coord(pkt.op_lon)}",
        "遥控站高度_m": _fmt_alt(pkt.op_alt),
        "无人机位置": f"{_fmt_coord(pkt.ua_lat)}, {_fmt_coord(pkt.ua_lon)}",
        "航迹角_deg": _fmt_speed(pkt.heading),
        "地速_mps": _fmt_speed(pkt.speed),
        "相对高度_m": _fmt_alt(pkt.rel_height) if pkt.has_rel_height else "未广播",
        "垂直速度_mps": _fmt_speed(pkt.vspeed) if pkt.has_vspeed else "未广播",
        "大地高度_m": _fmt_alt(pkt.geo_alt),
        "气压高度_m": _fmt_alt(pkt.baro_alt) if pkt.has_baro_alt else "未广播",
        "运行状态": OP_STATUS.get(pkt.op_status, f"无效({pkt.op_status})")
        if pkt.op_status >= 0 else "-",
        "坐标系": {0: "WGS-84", 1: "CGCS2000"}.get(pkt.coord_sys, f"无效({pkt.coord_sys})")
        if pkt.coord_sys >= 0 else "-",
        "水平精度": HORIZ_ACC.get(pkt.horiz_acc, f"无效({pkt.horiz_acc})")
        if pkt.horiz_acc >= 0 else "-",
        "垂直精度": VERT_ACC.get(pkt.vert_acc, f"无效({pkt.vert_acc})")
        if pkt.vert_acc >= 0 else "-",
        "速度精度": SPEED_ACC.get(pkt.speed_acc, f"无效({pkt.speed_acc})")
        if pkt.speed_acc >= 0 else "-",
        "时间戳": pkt.timestamp_utc,
        "时间戳精度": TS_ACC.get(pkt.ts_acc, f"无效({pkt.ts_acc})")
        if pkt.ts_acc >= 0 else "-",
    }
