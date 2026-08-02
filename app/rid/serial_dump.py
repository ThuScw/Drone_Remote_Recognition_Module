"""Extract internal flight-log records from the ESP32-S3 module over UART.

Protocol (firmware: main/console/console_cmd.cpp):
  PC → ESP32:  "DUMP\\r\\n"
  ESP32 → PC:  "+OK <N>\\r\\n"  (N = available records) or "+EMPTY\\r\\n"
  ESP32 → PC:  <N × 96 binary bytes>
  ESP32 → PC:  "+DONE\\r\\n"

Each 96-byte record stores one serialized GB 46750 packet, so records are
decoded with the same decoder as live BLE packets.
"""
from __future__ import annotations

import csv
import struct
import time
from datetime import datetime, timezone
from typing import Callable

from .decoder import decode_gb_packet
from .models import DecodedPacket

RECORD_SIZE = 96
PAYLOAD_OFFSET = 16
MAX_PAYLOAD = 80
BAUDRATE = 115200

CSV_HEADER = [
    "记录序号", "Flash时间戳_ms", "Flash时间戳_UTC",
    "唯一产品识别码", "实名登记号",
    "运行类别", "无人机分类", "遥控站位置类型",
    "遥控站纬度", "遥控站经度", "遥控站高度_m",
    "无人机纬度", "无人机经度", "航迹角_deg", "地速_mps",
    "相对高度_m", "垂直速度_mps", "大地高度_m", "气压高度_m",
    "运行状态", "坐标系",
    "水平精度", "垂直精度", "速度精度",
    "数据时间戳", "时间戳精度",
    "CRC有效",
]


def crc16_ccitt(data: bytes) -> int:
    """CRC-16/CCITT, identical to ESP32 FlightLog::crc16."""
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def verify_record(buf: bytes) -> bool:
    if len(buf) < RECORD_SIZE:
        return False
    magic = struct.unpack_from("<I", buf, 0)[0]
    if magic != 0x5249444C:  # "RIDL"
        return False
    stored_crc = struct.unpack_from("<H", buf, 4)[0]
    return stored_crc == crc16_ccitt(buf[6:RECORD_SIZE])


def list_ports() -> list[tuple[str, str]]:
    try:
        import serial.tools.list_ports
    except ImportError:
        return []
    return [(p.device, p.description or "") for p in serial.tools.list_ports.comports()]


def parse_record(buf: bytes) -> tuple[int, int, bytes]:
    """Return (flash_timestamp_ms, data_len, payload_bytes)."""
    ts = struct.unpack_from("<Q", buf, 6)[0]
    data_len = buf[14] | (buf[15] << 8)
    if data_len > MAX_PAYLOAD:
        data_len = MAX_PAYLOAD
    payload = buf[PAYLOAD_OFFSET:PAYLOAD_OFFSET + data_len]
    return ts, data_len, payload


def _read_exact(ser, n: int, timeout: float) -> bytes:
    deadline = time.monotonic() + timeout
    buf = bytearray()
    while len(buf) < n and time.monotonic() < deadline:
        chunk = ser.read(n - len(buf))
        if not chunk:
            break
        buf.extend(chunk)
    return bytes(buf)


def dump_flight_log(
    port: str,
    baudrate: int = BAUDRATE,
    timeout: float = 5.0,
    progress: Callable[[str, int, int], None] | None = None,
) -> list[DecodedPacket] | None:
    """Connect, run DUMP, and return decoded records.

    `progress(msg, done, total)` is called with human-readable status and a
    (done, total) pair (-1,-1 when total is not yet known). Returns None if the
    device did not respond or reported empty.
    """
    try:
        import serial
    except ImportError as e:
        raise RuntimeError("缺少 pyserial，请先运行: pip install pyserial") from e

    if progress:
        progress("打开串口...", -1, -1)

    with serial.Serial(port, baudrate, timeout=timeout) as ser:
        ser.reset_input_buffer()
        ser.write(b"DUMP\r\n")
        ser.flush()

        response = ""
        for _ in range(20):
            line = ser.readline().decode("ascii", errors="replace").strip()
            if line.startswith("+OK ") or line.startswith("+EMPTY"):
                response = line
                break

        if not response:
            raise RuntimeError("设备无响应。请确认串口正确、模块已上电且控制台命令已启用")
        if progress:
            progress(f"设备响应: {response}", -1, -1)

        if response.startswith("+EMPTY"):
            return []

        try:
            num_records = int(response.split()[1])
        except (IndexError, ValueError):
            raise RuntimeError(f"无法解析记录数: {response!r}")

        records: list[DecodedPacket] = []
        for i in range(num_records):
            buf = _read_exact(ser, RECORD_SIZE, timeout=timeout)
            if len(buf) < RECORD_SIZE:
                raise RuntimeError(f"记录 {i} 读取超时：仅收到 {len(buf)}/{RECORD_SIZE} 字节")
            ts_ms, _data_len, payload = parse_record(buf)
            pkt = decode_gb_packet(payload, source="serial")
            pkt.raw = buf  # keep full record for CRC check in CSV
            records.append(pkt)
            if progress and (i % 100 == 0 or i == num_records - 1):
                progress(f"接收记录 {i + 1}/{num_records}", i + 1, num_records)

        # drain trailing log noise until +DONE
        for _ in range(10):
            done = ser.readline().decode("ascii", errors="replace").strip()
            if done.startswith("+DONE"):
                break

    return records


def records_to_csv(records: list[DecodedPacket], output_path: str) -> int:
    """Write decoded records to a UTF-8 BOM CSV. Returns number of rows written."""
    written = 0
    with open(output_path, "w", newline="", encoding="utf-8-sig") as f:
        writer = csv.writer(f)
        writer.writerow(CSV_HEADER)
        for i, pkt in enumerate(records):
            flash_ts_ms = 0
            if len(pkt.raw) >= 14:
                flash_ts_ms = struct.unpack_from("<Q", pkt.raw, 6)[0]
            ts_utc = (
                datetime.fromtimestamp(flash_ts_ms / 1000.0, tz=timezone.utc).strftime(
                    "%Y-%m-%dT%H:%M:%S.%f"
                )[:-3] + "Z"
                if flash_ts_ms
                else ""
            )
            crc_ok = verify_record(pkt.raw)
            fld = pkt.fmt
            row = [
                i + 1,
                flash_ts_ms,
                ts_utc,
                fld.get("唯一产品识别码", ""),
                fld.get("实名登记号", ""),
                fld.get("运行类别", ""),
                fld.get("无人机分类", ""),
                fld.get("遥控站位置类型", ""),
                fld.get("遥控站位置", ""),
                fld.get("遥控站高度_m", ""),
                fld.get("无人机位置", ""),
                fld.get("航迹角_deg", ""),
                fld.get("地速_mps", ""),
                fld.get("相对高度_m", ""),
                fld.get("垂直速度_mps", ""),
                fld.get("大地高度_m", ""),
                fld.get("气压高度_m", ""),
                fld.get("运行状态", ""),
                fld.get("坐标系", ""),
                fld.get("水平精度", ""),
                fld.get("垂直精度", ""),
                fld.get("速度精度", ""),
                fld.get("时间戳", ""),
                fld.get("时间戳精度", ""),
                "Y" if crc_ok else "N",
            ]
            writer.writerow(row)
            written += 1
    return written
