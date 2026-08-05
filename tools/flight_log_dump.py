#!/usr/bin/env python3
"""
ESP32-S3 RID Flight Log Dump Tool
==================================
PC-side tool to extract flight data from the ESP32-S3 RID module over UART.

Protocol:
  PC → ESP32:  "DUMP\r\n"
  ESP32 → PC:  "+OK <N>\r\n"  (N = available records) or "+EMPTY\r\n"
  ESP32 → PC:  <N × 96 binary bytes>
  ESP32 → PC:  "+DONE\r\n"

Output: CSV file with all 21 GB 46750-2025 fields decoded.

Usage:
  python flight_log_dump.py                  # auto-list COM ports, interactive
  python flight_log_dump.py COM3             # direct port
  python flight_log_dump.py COM3 -o out.csv  # specify output file
"""

import sys
import struct
import csv
import os
from datetime import datetime, timezone

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    print("Missing pyserial. Install: pip install pyserial")
    sys.exit(1)

# ---------------------------------------------------------------------------
# Record layout (96 bytes on ESP32-S3 Flash)
#   [0..3]   Magic "RIDL" LE
#   [4..5]   CRC16-CCITT over bytes 6..95
#   [6..13]  Timestamp uint64 LE (ms)
#   [14..15] DataLen uint16 LE
#   [16..95] GB46750 payload (≤80 bytes, zero-padded)
# ---------------------------------------------------------------------------

RECORD_SIZE = 96
PAYLOAD_OFFSET = 16
MAX_PAYLOAD = 80

# dataId bit masks for O-fields (Byte 1)
DID_REL_HEIGHT = 0x10
DID_VERT_SPEED = 0x08
DID_BARO_ALT   = 0x02


def crc16_ccitt(data: bytes) -> int:
    """CRC-16/CCITT (same as ESP32 FlightLog::crc16)."""
    crc = 0xFFFF
    for b in data:
        crc ^= (b << 8)
        for _ in range(8):
            if crc & 0x8000:
                crc = (crc << 1) ^ 0x1021
            else:
                crc <<= 1
        crc &= 0xFFFF
    return crc


def verify_record(buf: bytes) -> bool:
    """Check magic and CRC of a 96-byte record."""
    magic = struct.unpack_from('<I', buf, 0)[0]
    if magic != 0x5249444C:  # "RIDL"
        return False
    stored_crc = struct.unpack_from('<H', buf, 4)[0]
    calc_crc = crc16_ccitt(buf[6:96])
    return stored_crc == calc_crc


def parse_record(buf: bytes):
    """Parse a 96-byte record into (timestamp_ms, data_len, payload_bytes)."""
    ts = struct.unpack_from('<Q', buf, 6)[0]
    data_len = buf[14] | (buf[15] << 8)
    if data_len > MAX_PAYLOAD:
        print(f"  Warning: data_len={data_len} exceeds max, clamping to {MAX_PAYLOAD}")
        data_len = MAX_PAYLOAD
    payload = buf[PAYLOAD_OFFSET:PAYLOAD_OFFSET + data_len]
    return ts, data_len, payload


# ---------------------------------------------------------------------------
# GB 46750-2025 payload decoder
# ---------------------------------------------------------------------------

OP_STATUS = {0: "未报告", 1: "地面", 2: "空中", 3: "紧急", 4: "失效(非紧急)", 5: "失效(紧急)"}
HORIZ_ACC = {0: ">=18.52km/未知", 1: "<18.52km", 2: "<7.41km", 3: "<3.70km", 4: "<1852m",
             5: "<926m", 6: "<556m", 7: "<185m", 8: "<92.6m", 9: "<30m", 10: "<10m",
             11: "<3m", 12: "<1m"}
VERT_ACC = {0: ">=150m/未知", 1: "<150m", 2: "<45m", 3: "<25m", 4: "<10m", 5: "<3m", 6: "<1m"}
SPEED_ACC = {0: ">=10m/s/未知", 1: "<10m/s", 2: "<3m/s", 3: "<1m/s", 4: "<0.3m/s"}
TS_ACC = {0: ">0.5s/未知", 1: "<=0.5s", 2: "<=0.4s", 3: "<=0.3s", 4: "<=0.2s", 5: "<=0.1s",
          6: "<=50ms", 7: "<=20ms", 8: "<=10ms"}


def decode_payload(payload: bytes):
    """Decode a GB46750 serialized payload into a dict of field values.

    Payload layout (serialized GB46750 packet):
      [0]     dataType    = 0xFF
      [1]     version     = 0x01
      [2]     dataLength  = content length
      [3..5]  dataId[0..2]
      [6..]   content     = actual data fields
    """
    if len(payload) < 6:
        return {}

    data_length = payload[2]          # declared content length
    end_pos = 6 + data_length         # where content actually ends
    data_id_1 = payload[4]            # dataId[1] — O-field presence bits
    d = {}
    pos = 6  # skip header

    # 001 唯一产品识别码 (20 bytes ASCII)
    uas_id = payload[pos:pos + 20].rstrip(b'\x00').decode('ascii', errors='replace')
    d['唯一产品识别码'] = uas_id
    pos += 20

    # 002 实名登记标志 (8 bytes ASCII)
    realname = payload[pos:pos + 8].rstrip(b'\x00').decode('ascii', errors='replace')
    d['实名登记号'] = realname
    pos += 8

    # 003 运行类别
    d['运行类别'] = payload[pos]; pos += 1

    # 004 无人机分类
    d['无人机分类'] = payload[pos]; pos += 1

    # 005 遥控站位置类型
    d['遥控站位置类型'] = payload[pos]; pos += 1

    # 006 遥控站位置 (int32 LE × 2, deg * 1e7)
    op_lat = struct.unpack_from('<i', payload, pos)[0] / 1e7; pos += 4
    op_lon = struct.unpack_from('<i', payload, pos)[0] / 1e7; pos += 4
    d['遥控站纬度'] = op_lat
    d['遥控站经度'] = op_lon

    # 007 遥控站高度 (uint16 LE, (val+1000)*2, 0.5m res)
    d['遥控站高度_m'] = struct.unpack_from('<H', payload, pos)[0] / 2.0 - 1000.0; pos += 2

    # 008 无人机位置
    ua_lat = struct.unpack_from('<i', payload, pos)[0] / 1e7; pos += 4
    ua_lon = struct.unpack_from('<i', payload, pos)[0] / 1e7; pos += 4
    d['无人机纬度'] = ua_lat
    d['无人机经度'] = ua_lon

    # 009 航迹角 (uint16 LE, val*10, 0.1° res)
    d['航迹角_deg'] = struct.unpack_from('<H', payload, pos)[0] / 10.0; pos += 2

    # 010 地速 (uint16 LE, val*10, 0.1 m/s res)
    d['地速_mps'] = struct.unpack_from('<H', payload, pos)[0] / 10.0; pos += 2

    # 011 相对高度 (uint16 LE, (val+9000)*2, 0.5m res) — O field
    if data_id_1 & DID_REL_HEIGHT:
        d['相对高度_m'] = struct.unpack_from('<H', payload, pos)[0] / 2.0 - 9000.0
        pos += 2
    else:
        d['相对高度_m'] = ''

    # 012 垂直速度 (1 byte) — O field
    if data_id_1 & DID_VERT_SPEED:
        b = payload[pos]; pos += 1
        val = b & 0x7F
        d['垂直速度_mps'] = -val / 2.0 if (b & 0x80) else val / 2.0
    else:
        d['垂直速度_mps'] = ''

    # 013 大地高度 (uint16 LE, (val+1000)*2, 0.5m res)
    d['大地高度_m'] = struct.unpack_from('<H', payload, pos)[0] / 2.0 - 1000.0; pos += 2

    # 014 气压高度 (uint16 LE) — O field
    if data_id_1 & DID_BARO_ALT:
        d['气压高度_m'] = struct.unpack_from('<H', payload, pos)[0] / 2.0 - 1000.0
        pos += 2
    else:
        d['气压高度_m'] = ''

    # 015 运行状态
    d['运行状态'] = OP_STATUS.get(payload[pos], str(payload[pos])); pos += 1

    # 016 坐标系类型
    d['坐标系'] = 'WGS-84' if payload[pos] == 0 else 'CGCS2000' if payload[pos] == 1 else str(payload[pos]); pos += 1

    # 017-019 精度
    d['水平精度'] = HORIZ_ACC.get(payload[pos], str(payload[pos])); pos += 1
    d['垂直精度'] = VERT_ACC.get(payload[pos], str(payload[pos])); pos += 1
    d['速度精度'] = SPEED_ACC.get(payload[pos], str(payload[pos])); pos += 1

    # 020 时间戳 (uint48 LE, ms)
    ts_bytes = payload[pos:pos + 6] + b'\x00\x00'
    ts = struct.unpack_from('<Q', ts_bytes, 0)[0]
    pos += 6
    d['数据时间戳'] = ts

    # 021 时间戳精度
    d['时间戳精度'] = TS_ACC.get(payload[pos], str(payload[pos])); pos += 1

    return d


# ---------------------------------------------------------------------------
# Serial protocol
# ---------------------------------------------------------------------------

def list_ports():
    ports = serial.tools.list_ports.comports()
    available = list(ports)
    if not available:
        print("No serial ports found.")
        return []
    print("Available serial ports:")
    for i, p in enumerate(available):
        print(f"  [{i}] {p.device} — {p.description}")
    return available


def _read_exact(ser, n: int, timeout: float = 5.0) -> bytes:
    """Read exactly n bytes from serial, or fewer on timeout."""
    import time
    buf = bytearray()
    deadline = time.monotonic() + timeout
    while len(buf) < n and time.monotonic() < deadline:
        chunk = ser.read(n - len(buf))
        if not chunk:
            break
        buf.extend(chunk)
    return bytes(buf)


def dump_flight_log(port: str, baudrate: int = 115200, timeout: float = 5.0):
    """Connect to ESP32-S3, send DUMP, receive all records."""
    with serial.Serial(port, baudrate, timeout=timeout) as ser:
        ser.reset_input_buffer()

        ser.write(b"DUMP\r\n")
        ser.flush()

        response = ""
        for attempt in range(10):
            if attempt == 3:
                print("  Still waiting for ESP32 response...")
            line = ser.readline().decode('ascii', errors='replace').strip()
            if line.startswith("+OK ") or line.startswith("+EMPTY"):
                response = line
                break
            if line:
                print(f"  (skipped log line: {line[:80]})")

        if not response:
            print("No valid response from ESP32. Is the console command module loaded?")
            return None, None

        print(f"ESP32: {response}")

        if response.startswith("+EMPTY"):
            print("No flight records stored on device.")
            return None, None

        try:
            num_records = int(response.split()[1])
        except (IndexError, ValueError):
            print(f"Failed to parse record count from: {response}")
            return None, None

        print(f"Receiving {num_records} records ({num_records * RECORD_SIZE} bytes)...")

        all_records = []
        bad_crc = 0
        for i in range(num_records):
            buf = _read_exact(ser, RECORD_SIZE, timeout=timeout)
            if len(buf) < RECORD_SIZE:
                print(f"  Timeout at record {i}: got {len(buf)} of {RECORD_SIZE} bytes")
                break
            if not verify_record(buf):
                bad_crc += 1
            all_records.append(buf)
            if (i + 1) % 500 == 0:
                print(f"  {i + 1}/{num_records}...")

        print(f"  Done: {len(all_records)} records received, {bad_crc} CRC mismatches")

        # Read done marker (skip log noise). 用子串匹配容忍二进制记录流的
        # 前导垃圾字节 (如记录错位时 +DONE 前残留的字节)。
        for _ in range(5):
            done = ser.readline().decode('ascii', errors='replace').strip()
            if "+DONE" in done:
                print(f"ESP32: {done}")
                break
            elif done:
                print(f"  (skipped: {done[:80]})")

        return all_records, bad_crc


# ---------------------------------------------------------------------------
# CSV output
# ---------------------------------------------------------------------------

CSV_HEADER = [
    '记录序号', 'Flash时间戳_ms', 'Flash时间戳_UTC',
    '唯一产品识别码', '实名登记号',
    '运行类别', '无人机分类', '遥控站位置类型',
    '遥控站纬度', '遥控站经度', '遥控站高度_m',
    '无人机纬度', '无人机经度', '航迹角_deg', '地速_mps',
    '相对高度_m', '垂直速度_mps', '大地高度_m', '气压高度_m',
    '运行状态', '坐标系',
    '水平精度', '垂直精度', '速度精度',
    '数据时间戳', '时间戳精度',
    'CRC有效',
]


def records_to_csv(records, output_path):
    written = 0
    with open(output_path, 'w', newline='', encoding='utf-8-sig') as f:
        writer = csv.writer(f)
        writer.writerow(CSV_HEADER)

        for i, buf in enumerate(records):
            crc_ok = verify_record(buf)
            ts_ms, data_len, payload = parse_record(buf)

            try:
                fields = decode_payload(payload)
            except Exception as e:
                print(f"  Warning: decode error at record {i}: {type(e).__name__}: {e}")
                continue

            # 记录被污染 (CRC 失败) 时 ts 可能是巨大垃圾值, fromtimestamp 会抛
            # OverflowError/OSError — 捕获后该行时间戳留空, 不让单条坏记录中断整个导出。
            try:
                ts_utc = datetime.fromtimestamp(ts_ms / 1000.0, tz=timezone.utc).strftime('%Y-%m-%dT%H:%M:%S.%f')[:-3] + 'Z'
            except (OverflowError, OSError, ValueError):
                ts_utc = ''

            writer.writerow([
                i + 1,
                ts_ms,
                ts_utc,
                fields.get('唯一产品识别码', ''),
                fields.get('实名登记号', ''),
                fields.get('运行类别', ''),
                fields.get('无人机分类', ''),
                fields.get('遥控站位置类型', ''),
                fields.get('遥控站纬度', ''),
                fields.get('遥控站经度', ''),
                fields.get('遥控站高度_m', ''),
                fields.get('无人机纬度', ''),
                fields.get('无人机经度', ''),
                fields.get('航迹角_deg', ''),
                fields.get('地速_mps', ''),
                fields.get('相对高度_m', ''),
                fields.get('垂直速度_mps', ''),
                fields.get('大地高度_m', ''),
                fields.get('气压高度_m', ''),
                fields.get('运行状态', ''),
                fields.get('坐标系', ''),
                fields.get('水平精度', ''),
                fields.get('垂直精度', ''),
                fields.get('速度精度', ''),
                fields.get('数据时间戳', ''),
                fields.get('时间戳精度', ''),
                'Y' if crc_ok else 'N',
            ])
            written += 1

    print(f"Wrote {written} records to {output_path}")
    return written


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    output_path = None
    port = None

    args = sys.argv[1:]
    i = 0
    while i < len(args):
        if args[i] in ('-o', '--output'):
            i += 1
            if i >= len(args):
                print("Error: -o/--output requires a filename argument")
                return
            output_path = args[i]
        elif args[i] in ('-h', '--help'):
            print(__doc__)
            return
        else:
            port = args[i]
        i += 1

    # Pick port
    if port is None:
        available = list_ports()
        if not available:
            return
        sel = input(f"Select port [0-{len(available) - 1}] or type COM port name: ").strip()
        try:
            idx = int(sel)
            port = available[idx].device
        except ValueError:
            port = sel
        except IndexError:
            print(f"Invalid index: {sel}")
            return

    print(f"Connecting to {port}...")
    try:
        records, bad_crc = dump_flight_log(port)
    except serial.SerialException as e:
        print(f"Serial error: {e}")
        return
    except KeyboardInterrupt:
        print("\nCancelled by user.")
        return

    if records is None:
        return

    if not records:
        print("No records received.")
        return

    # Output path
    if output_path is None:
        output_path = f"flight_log_{datetime.now().strftime('%Y%m%d_%H%M%S')}.csv"

    written = records_to_csv(records, output_path)

    total = len(records)
    print(f"\nSummary: {total} records received, {written} written, {bad_crc} CRC errors")


if __name__ == '__main__':
    main()
