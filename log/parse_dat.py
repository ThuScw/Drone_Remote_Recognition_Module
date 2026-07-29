#!/usr/bin/env python3
"""
Parse MAVLink v2 frames from raw serial port captures (.DAT files).
Focus on identifying all message types and decoding key ones.
"""

import struct
import os
import sys
import glob
import math
from typing import Any


def crc_x25(data: bytes) -> int:
    """Calculate MAVLink X.25 CRC (CRC-16-MCRF4XX)."""
    crc = 0xFFFF
    for byte in data:
        tmp = byte ^ (crc & 0xFF)
        tmp ^= (tmp << 4) & 0xFF
        crc = ((crc >> 8) ^ (tmp << 8) ^ (tmp << 3) ^ (tmp >> 4)) & 0xFFFF
    return crc


def parse_mavlink_v2(filepath: str) -> list[dict[str, Any]]:
    """Parse MAVLink v2 frames from a binary file."""
    with open(filepath, 'rb') as f:
        data = f.read()

    print(f"=== File: {os.path.basename(filepath)} ===")
    print(f"=== Size: {len(data)} bytes ===")
    print()

    frames: list[dict[str, Any]] = []
    pos = 0

    while pos < len(data) - 12:
        if data[pos] == 0xFD:  # MAVLink v2 magic
            plen = data[pos + 1]
            if plen <= 253 and pos + 12 + plen <= len(data):
                incompat = data[pos + 2]
                compat = data[pos + 3]
                seq = data[pos + 4]
                sysid = data[pos + 5]
                compid = data[pos + 6]
                msgid = data[pos + 7] | (data[pos + 8] << 8) | (data[pos + 9] << 16)

                payload = data[pos + 10: pos + 10 + plen]
                crc_received = data[pos + 10 + plen] | (data[pos + 10 + plen + 1] << 8)

                # MAVLink v2 CRC: covers len(1)+incompat(1)+compat(1)+seq(1)+sysid(1)+compid(1)+msgid(3)+payload
                crc_input = data[pos + 1: pos + 10 + plen]
                crc_calculated = crc_x25(crc_input)
                crc_ok = (crc_received == crc_calculated)

                frames.append({
                    'offset': pos,
                    'payload_len': plen,
                    'incompat': incompat,
                    'compat': compat,
                    'seq': seq,
                    'sysid': sysid,
                    'compid': compid,
                    'msgid': msgid,
                    'payload': payload,
                    'crc_ok': crc_ok,
                })
                pos += 12 + plen
                continue
        pos += 1

    # Summary
    msg_counts: dict[tuple[int, int, int], int] = {}
    sysid_counts: dict[int, int] = {}
    compid_counts: dict[int, int] = {}
    for f in frames:
        key = (f['msgid'], f['sysid'], f['compid'])
        msg_counts[key] = msg_counts.get(key, 0) + 1
        sysid_counts[f['sysid']] = sysid_counts.get(f['sysid'], 0) + 1
        compid_counts[f['compid']] = compid_counts.get(f['compid'], 0) + 1

    crc_ok_count = sum(1 for f in frames if f['crc_ok'])

    print(f"Total frames: {len(frames)}")
    print(f"CRC OK: {crc_ok_count}/{len(frames)}")
    print()
    print("System IDs:", dict(sorted(sysid_counts.items())))
    print("Component IDs:", dict(sorted(compid_counts.items())))
    print()
    print("Message distribution:")
    for (msgid, sysid, compid), count in sorted(msg_counts.items(), key=lambda x: -x[1]):
        name = MSG_NAMES.get(msgid, f"UNKNOWN_0x{msgid:04X}")
        print(f"  msgid=0x{msgid:04X} ({msgid:>5d}) [{name:<25s}] sys={sysid} comp={compid} count={count}")

    return frames


MSG_NAMES: dict[int, str] = {
    0: "HEARTBEAT",
    1: "SYS_STATUS",
    2: "SYSTEM_TIME",
    4: "PING",
    11: "SET_MODE",
    24: "GPS_RAW_INT",
    30: "ATTITUDE",
    33: "GLOBAL_POSITION_INT",
    36: "SERVO_OUTPUT_RAW",
    65: "RC_CHANNELS",
    74: "VFR_HUD",
    105: "HOME_POSITION",
    109: "RADIO_STATUS",
    111: "SCALED_PRESSURE",
    147: "STATUSTEXT_LONG",
    165: "MISSION_CURRENT",
    230: "ESTIMATOR_STATUS",
    253: "STATUSTEXT",
    256: "SETUP_SIGNING",
    419: "REMOTE_ID (ArduPilot)",
}


def decode_heartbeat(payload: bytes) -> None:
    """Decode HEARTBEAT (msgid 0)."""
    if len(payload) < 9:
        return
    custom_mode = struct.unpack_from('<I', payload, 0)[0]
    mav_type = payload[4]
    autopilot = payload[5]
    base_mode = payload[6]
    system_status = payload[7]
    mavlink_version = payload[8]

    type_names = {0: "GENERIC", 1: "FIXED_WING", 2: "QUADROTOR", 3: "COAXIAL",
                  4: "HELICOPTER", 5: "ANTENNA_TRACKER", 6: "GCS", 13: "VTOL_DUOROTOR"}
    autopilot_names = {0: "GENERIC", 3: "ARDUPILOTMEGA"}
    status_names = {0: "UNINIT", 1: "BOOT", 2: "CALIBRATING", 3: "STANDBY",
                    4: "ACTIVE", 5: "CRITICAL", 6: "EMERGENCY",
                    7: "POWEROFF", 8: "FLIGHT_TERMINATION"}

    print(f"  custom_mode: {custom_mode}")
    print(f"  mav_type: {mav_type} ({type_names.get(mav_type, 'UNKNOWN')})")
    print(f"  autopilot: {autopilot} ({autopilot_names.get(autopilot, 'UNKNOWN')})")
    print(f"  base_mode: 0x{base_mode:02X} (armed={bool(base_mode & 0x80)})")
    print(f"  system_status: {system_status} ({status_names.get(system_status, 'UNKNOWN')})")
    print(f"  mavlink_version: {mavlink_version}")


def decode_global_position_int(payload: bytes) -> None:
    """Decode GLOBAL_POSITION_INT (msgid 33)."""
    if len(payload) < 28:
        return
    time_boot_ms = struct.unpack_from('<I', payload, 0)[0]
    lat = struct.unpack_from('<i', payload, 4)[0]
    lon = struct.unpack_from('<i', payload, 8)[0]
    alt = struct.unpack_from('<i', payload, 12)[0]
    relative_alt = struct.unpack_from('<i', payload, 16)[0]
    vx = struct.unpack_from('<h', payload, 20)[0]
    vy = struct.unpack_from('<h', payload, 22)[0]
    vz = struct.unpack_from('<h', payload, 24)[0]
    hdg = struct.unpack_from('<H', payload, 26)[0]

    print(f"  time_boot_ms: {time_boot_ms}")
    print(f"  lat: {lat} ({lat / 1e7:.7f} deg)")
    print(f"  lon: {lon} ({lon / 1e7:.7f} deg)")
    print(f"  alt: {alt} ({alt / 1000:.1f} m MSL)")
    print(f"  relative_alt: {relative_alt} ({relative_alt / 1000:.1f} m AGL)")
    print(f"  vx: {vx / 100:.2f} vy: {vy / 100:.2f} vz: {vz / 100:.2f} m/s")
    print(f"  heading: {hdg / 100:.1f} deg" if hdg != 65535 else "  heading: UNKNOWN")


def decode_gps_raw_int(payload: bytes) -> None:
    """Decode GPS_RAW_INT (msgid 24)."""
    if len(payload) < 30:
        return
    time_usec = struct.unpack_from('<Q', payload, 0)[0]
    fix_type = payload[8]
    lat = struct.unpack_from('<i', payload, 9)[0]
    lon = struct.unpack_from('<i', payload, 13)[0]
    alt = struct.unpack_from('<i', payload, 17)[0]
    eph = struct.unpack_from('<H', payload, 21)[0]
    epv = struct.unpack_from('<H', payload, 23)[0]
    vel = struct.unpack_from('<H', payload, 25)[0]
    cog = struct.unpack_from('<H', payload, 27)[0]
    satellites_visible = payload[29]

    fix_names = {0: "NO_GPS", 1: "NO_FIX", 2: "2D_FIX", 3: "3D_FIX",
                 4: "DGPS", 5: "RTK_FLOAT", 6: "RTK_FIXED"}

    print(f"  time_usec: {time_usec}")
    print(f"  fix_type: {fix_type} ({fix_names.get(fix_type, 'UNKNOWN')})")
    print(f"  lat: {lat} ({lat / 1e7:.7f} deg)")
    print(f"  lon: {lon} ({lon / 1e7:.7f} deg)")
    print(f"  alt: {alt} ({alt / 1000:.1f} m)")
    print(f"  eph: {eph / 100:.1f}  epv: {epv / 100:.1f}")
    print(f"  vel: {vel / 100:.2f} m/s  cog: {cog / 100:.1f} deg")
    print(f"  satellites_visible: {satellites_visible}")


def decode_vfr_hud(payload: bytes) -> None:
    """Decode VFR_HUD (msgid 74)."""
    if len(payload) < 20:
        return
    airspeed = struct.unpack_from('<f', payload, 0)[0]
    groundspeed = struct.unpack_from('<f', payload, 4)[0]
    alt = struct.unpack_from('<f', payload, 8)[0]
    climb = struct.unpack_from('<f', payload, 12)[0]
    heading = struct.unpack_from('<h', payload, 16)[0]
    throttle = struct.unpack_from('<H', payload, 18)[0]

    print(f"  airspeed: {airspeed:.2f} m/s")
    print(f"  groundspeed: {groundspeed:.2f} m/s")
    print(f"  alt: {alt:.1f} m")
    print(f"  climb: {climb:.2f} m/s")
    print(f"  heading: {heading} deg")
    print(f"  throttle: {throttle}%")


def decode_statustext(payload: bytes) -> None:
    """Decode STATUSTEXT (msgid 253)."""
    if len(payload) < 2:
        return
    severity = payload[0]
    text = payload[1:].split(b'\x00')[0].decode('ascii', errors='replace')
    sev_names = {0: "EMERGENCY", 1: "ALERT", 2: "CRITICAL", 3: "ERROR",
                 4: "WARNING", 5: "NOTICE", 6: "INFO", 7: "DEBUG"}
    print(f"  severity: {severity} ({sev_names.get(severity, '?')})")
    print(f"  text: '{text}'")


def decode_attitude(payload: bytes) -> None:
    """Decode ATTITUDE (msgid 30)."""
    if len(payload) < 28:
        return
    time_boot_ms = struct.unpack_from('<I', payload, 0)[0]
    roll = struct.unpack_from('<f', payload, 4)[0]
    pitch = struct.unpack_from('<f', payload, 8)[0]
    yaw = struct.unpack_from('<f', payload, 12)[0]
    rollspeed = struct.unpack_from('<f', payload, 16)[0]
    pitchspeed = struct.unpack_from('<f', payload, 20)[0]
    yawspeed = struct.unpack_from('<f', payload, 24)[0]

    print(f"  time_boot_ms: {time_boot_ms}")
    print(f"  roll: {math.degrees(roll):.1f} deg  pitch: {math.degrees(pitch):.1f} deg  yaw: {math.degrees(yaw):.1f} deg")
    print(f"  rollspeed: {math.degrees(rollspeed):.1f}  pitchspeed: {math.degrees(pitchspeed):.1f}  yawspeed: {math.degrees(yawspeed):.1f} deg/s")


def decode_remoteid(payload: bytes) -> None:
    """Show raw payload for ArduPilot REMOTE_ID (msgid 419)."""
    print(f"  payload_len: {len(payload)}")
    print(f"  raw hex (first 60 bytes): {payload[:60].hex()}")

    non_zero_ranges = []
    start = None
    for i, b in enumerate(payload):
        if b != 0 and start is None:
            start = i
        elif b == 0 and start is not None:
            non_zero_ranges.append((start, i - 1))
            start = None
    if start is not None:
        non_zero_ranges.append((start, len(payload) - 1))
    print(f"  non-zero byte ranges: {non_zero_ranges}")


def analyze_file(filepath: str) -> None:
    """Full analysis of a .DAT file."""
    frames = parse_mavlink_v2(filepath)
    if not frames:
        print("No MAVLink v2 frames found!\n")
        return

    print("\n=== Detailed decoding (first 2 of each type) ===\n")

    decoders = {
        0: decode_heartbeat,
        24: decode_gps_raw_int,
        30: decode_attitude,
        33: decode_global_position_int,
        74: decode_vfr_hud,
        253: decode_statustext,
        419: decode_remoteid,
    }

    shown: dict[tuple[int, int, int], int] = {}
    for f in frames:
        key = (f['msgid'], f['sysid'], f['compid'])
        if key not in shown:
            shown[key] = 0
        if shown[key] >= 2:
            continue

        msgname = MSG_NAMES.get(f['msgid'], f"UNKNOWN_0x{f['msgid']:04X}")
        print(f"--- offset={f['offset']} msgid=0x{f['msgid']:04X} ({msgname}) "
              f"sys={f['sysid']} comp={f['compid']} len={f['payload_len']} "
              f"CRC={'OK' if f['crc_ok'] else 'FAIL'} ---")

        decoder = decoders.get(f['msgid'])
        if decoder:
            try:
                decoder(f['payload'])
            except Exception as e:
                print(f"  Decode error: {e}")
        else:
            print(f"  payload hex: {f['payload'][:40].hex()}...")
        print()
        shown[key] += 1

    # Special analysis for msgid 419
    rid_frames = [f for f in frames if f['msgid'] == 419]
    if rid_frames:
        print(f"\n=== msgid=419 (REMOTE_ID) Analysis ===")
        print(f"Count: {len(rid_frames)}")
        lengths = sorted(set(f['payload_len'] for f in rid_frames))
        print(f"Payload lengths: {lengths}")

        first = rid_frames[0]
        last = rid_frames[-1]
        print(f"First frame seq={first['seq']}: {first['payload'][:80].hex()}")
        print(f"Last  frame seq={last['seq']}: {last['payload'][:80].hex()}")

    print("\n" + "=" * 80 + "\n")


def main() -> None:
    log_dir = os.path.dirname(os.path.abspath(__file__))
    dat_files = sorted(glob.glob(os.path.join(log_dir, '*.DAT')))
    print(f"Found {len(dat_files)} .DAT files in {log_dir}\n")

    for filepath in dat_files:
        analyze_file(filepath)


if __name__ == '__main__':
    main()
