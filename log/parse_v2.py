#!/usr/bin/env python3
"""
Proper MAVLink v1/v2 parser with frame chain verification.
"""

import struct
import os
import glob
import math
from typing import Any


def crc_x25(data: bytes) -> int:
    """MAVLink X.25 CRC (CRC-16-MCRF4XX)."""
    crc = 0xFFFF
    for byte in data:
        tmp = byte ^ (crc & 0xFF)
        tmp ^= (tmp << 4) & 0xFF
        crc = ((crc >> 8) ^ (tmp << 8) ^ (tmp << 3) ^ (tmp >> 4)) & 0xFFFF
    return crc


# MAVLink v2 message CRC extras (indexed by msgid)
# Source: MAVLink message definitions
MSG_CRC_EXTRAS: dict[int, int] = {
    0: 50,    # HEARTBEAT
    1: 124,   # SYS_STATUS
    2: 222,   # SYSTEM_TIME
    4: 237,   # PING
    11: 89,   # SET_MODE
    24: 24,   # GPS_RAW_INT
    30: 39,   # ATTITUDE
    33: 104,  # GLOBAL_POSITION_INT
    36: 220,  # SERVO_OUTPUT_RAW
    65: 146,  # RC_CHANNELS
    74: 20,   # VFR_HUD
    105: 251, # HOME_POSITION (approx)
    109: 183, # RADIO_STATUS
    111: 113, # SCALED_PRESSURE (approx)
    253: 83,  # STATUSTEXT
    256: 10,  # SETUP_SIGNING
    419: 23,  # ArduPilot REMOTE_ID (need to verify)
    12900: 101,  # OPEN_DRONE_ID_BASIC_ID
    12901: 106,  # OPEN_DRONE_ID_LOCATION
    12902: 122,  # OPEN_DRONE_ID_AUTHORIZATION
    12903: 24,   # OPEN_DRONE_ID_SELF_ID
    12904: 105,  # OPEN_DRONE_ID_SYSTEM
    12905: 86,   # OPEN_DRONE_ID_OPERATOR_ID
    12906: 169,  # OPEN_DRONE_ID_MESSAGE_PACK
}


def parse_and_verify(filepath: str) -> None:
    """Parse MAVLink frames, verify by chaining, and check CRC."""
    with open(filepath, 'rb') as f:
        data = f.read()

    fname = os.path.basename(filepath)
    print(f"\n{'='*80}")
    print(f"FILE: {fname}  ({len(data)} bytes)")
    print(f"{'='*80}\n")

    # Phase 1: Find all potential v2 frames (0xFD header)
    v2_frames: list[dict[str, Any]] = []
    pos = 0
    while pos < len(data) - 12:
        if data[pos] != 0xFD:
            pos += 1
            continue
        plen = data[pos + 1]
        if plen > 253 or pos + 12 + plen > len(data):
            pos += 1
            continue

        incompat = data[pos + 2]
        compat = data[pos + 3]
        seq = data[pos + 4]
        sysid = data[pos + 5]
        compid = data[pos + 6]
        msgid = data[pos + 7] | (data[pos + 8] << 8) | (data[pos + 9] << 16)
        payload = data[pos + 10: pos + 10 + plen]
        crc_received = struct.unpack_from('<H', data, pos + 10 + plen)[0]

        # CRC input: all bytes after magic, including msgid bytes
        crc_input = data[pos + 1: pos + 10 + plen]

        # Add CRC extra byte if message type is known
        crc_extra = MSG_CRC_EXTRAS.get(msgid, 0)
        crc_calculated = crc_x25(crc_input)
        # MAVLink CRC: calculated XOR with extra byte
        crc_calculated_with_extra = crc_calculated  # We'll check both

        frame_size = 12 + plen  # 1(magic) + 1(len) + 1(incompat) + 1(compat) + 1(seq) + 1(sysid) + 1(compid) + 3(msgid) + plen + 2(crc)

        # Verify by checking if next FD is at expected position
        next_pos = pos + frame_size
        chained = (next_pos < len(data) and data[next_pos] == 0xFD)

        v2_frames.append({
            'offset': pos,
            'payload_len': plen,
            'seq': seq,
            'sysid': sysid,
            'compid': compid,
            'msgid': msgid,
            'payload': payload,
            'crc_received': crc_received,
            'crc_calculated': crc_calculated,
            'crc_extra': crc_extra,
            'frame_size': frame_size,
            'chained': chained,
            'incompat': incompat,
            'compat': compat,
        })
        pos = next_pos if chained else pos + 1

    # Also find v1 frames (0xFE header)
    v1_frames: list[dict[str, Any]] = []
    pos = 0
    while pos < len(data) - 8:
        if data[pos] != 0xFE:
            pos += 1
            continue
        plen = data[pos + 1]
        if plen > 253 or pos + 8 + plen > len(data):
            pos += 1
            continue

        seq = data[pos + 2]
        sysid = data[pos + 3]
        compid = data[pos + 4]
        msgid = data[pos + 5]
        payload = data[pos + 6: pos + 6 + plen]
        crc_received = struct.unpack_from('<H', data, pos + 6 + plen)[0]

        # CRC input: all bytes after magic
        crc_input = data[pos + 1: pos + 6 + plen]
        crc_extra = MSG_CRC_EXTRAS.get(msgid, 0)
        crc_calculated = crc_x25(crc_input)

        frame_size = 8 + plen
        next_pos = pos + frame_size
        chained = (next_pos < len(data) and data[next_pos] == 0xFE)

        v1_frames.append({
            'offset': pos,
            'payload_len': plen,
            'seq': seq,
            'sysid': sysid,
            'compid': compid,
            'msgid': msgid,
            'payload': payload,
            'crc_received': crc_received,
            'crc_calculated': crc_calculated,
            'crc_extra': crc_extra,
            'frame_size': frame_size,
            'chained': chained,
        })
        pos = next_pos if chained else pos + 1

    # Print summary
    chained_v2 = [f for f in v2_frames if f['chained']]
    chained_v1 = [f for f in v1_frames if f['chained']]

    print(f"MAVLink v2 (0xFD) frames found: {len(v2_frames)} (chained: {len(chained_v2)})")
    print(f"MAVLink v1 (0xFE) frames found: {len(v1_frames)} (chained: {len(chained_v1)})")
    print()

    # CRC verification for chained frames
    if chained_v2:
        crc_ok = 0
        crc_ok_with_extra = 0
        for f in chained_v2:
            if f['crc_received'] == f['crc_calculated']:
                crc_ok += 1
            # Try with CRC extra byte appended
            extra_bytes = bytes([f['crc_extra'] & 0xFF, (f['crc_extra'] >> 8) & 0xFF]) if f['crc_extra'] else b''
            if extra_bytes:
                crc_with_extra = crc_x25(bytes(f['payload'][:f['payload_len']]) + extra_bytes)
                # Actually, CRC extra is XORed into the final CRC
                calc = f['crc_calculated']
                low = (calc & 0xFF) ^ (f['crc_extra'] & 0xFF)
                high = ((calc >> 8) & 0xFF) ^ ((f['crc_extra'] >> 8) & 0xFF)
                crc_xor = low | (high << 8)
                if f['crc_received'] == crc_xor:
                    crc_ok_with_extra += 1

        print(f"V2 chained CRC check (no extra): {crc_ok}/{len(chained_v2)}")
        print(f"V2 chained CRC check (with extra XOR): {crc_ok_with_extra}/{len(chained_v2)}")
        print()

    if chained_v1:
        crc_ok = 0
        crc_ok_with_extra = 0
        for f in chained_v1:
            if f['crc_received'] == f['crc_calculated']:
                crc_ok += 1
            extra_bytes = f['crc_extra']
            calc = f['crc_calculated']
            low = (calc & 0xFF) ^ (extra_bytes & 0xFF)
            high = ((calc >> 8) & 0xFF) ^ ((extra_bytes >> 8) & 0xFF)
            crc_xor = low | (high << 8)
            if f['crc_received'] == crc_xor:
                crc_ok_with_extra += 1
        print(f"V1 chained CRC check (no extra): {crc_ok}/{len(chained_v1)}")
        print(f"V1 chained CRC check (with extra XOR): {crc_ok_with_extra}/{len(chained_v1)}")
        print()

    # Show message distribution for chained frames
    print("--- V2 chained frame message distribution ---")
    msg_counts: dict[int, int] = {}
    for f in chained_v2:
        msg_counts[f['msgid']] = msg_counts.get(f['msgid'], 0) + 1
    for msgid, count in sorted(msg_counts.items(), key=lambda x: -x[1]):
        print(f"  msgid={msgid:>5d} (0x{msgid:04X}): {count}")
    print()

    print("--- V1 chained frame message distribution ---")
    msg_counts_v1: dict[int, int] = {}
    for f in chained_v1:
        msg_counts_v1[f['msgid']] = msg_counts_v1.get(f['msgid'], 0) + 1
    for msgid, count in sorted(msg_counts_v1.items(), key=lambda x: -x[1]):
        print(f"  msgid={msgid:>5d} (0x{msgid:02X}): {count}")
    print()

    # Decode first few chained frames
    all_chained = chained_v2 + chained_v1
    if not all_chained:
        print("No chained frames to decode!")
        return

    print("--- First 5 chained frames detail ---")
    for f in sorted(all_chained, key=lambda x: x['offset'])[:5]:
        ver = "v2" if 'incompat' in f else "v1"
        print(f"\n  [{ver}] offset={f['offset']} msgid={f['msgid']} "
              f"sys={f['sysid']} comp={f['compid']} seq={f['seq']} "
              f"plen={f['payload_len']} CRC_recv=0x{f['crc_received']:04X} "
              f"CRC_calc=0x{f['crc_calculated']:04X}")
        print(f"  payload hex: {f['payload'][:48].hex()}")

        # Try to decode specific messages
        if f['msgid'] == 0:  # HEARTBEAT
            decode_heartbeat(f['payload'])
        elif f['msgid'] == 24:  # GPS_RAW_INT
            decode_gps_raw_int(f['payload'])
        elif f['msgid'] == 30:  # ATTITUDE
            decode_attitude(f['payload'])
        elif f['msgid'] == 33:  # GLOBAL_POSITION_INT
            decode_global_position_int(f['payload'])
        elif f['msgid'] == 74:  # VFR_HUD
            decode_vfr_hud(f['payload'])
        elif f['msgid'] == 419:  # ArduPilot REMOTE_ID
            decode_remoteid_419(f['payload'])
        elif f['msgid'] in (12900, 12901, 12902, 12903, 12904, 12905, 12906):
            decode_opendroneid(f['msgid'], f['payload'])


def decode_heartbeat(payload: bytes) -> None:
    """Decode HEARTBEAT."""
    if len(payload) < 9:
        return
    custom_mode = struct.unpack_from('<I', payload, 0)[0]
    mav_type = payload[4]
    autopilot = payload[5]
    base_mode = payload[6]
    system_status = payload[7]
    mavlink_version = payload[8]
    type_names = {0: "GENERIC", 1: "FIXED_WING", 2: "QUADROTOR", 13: "VTOL_DUOROTOR"}
    status_names = {0: "UNINIT", 1: "BOOT", 3: "STANDBY", 4: "ACTIVE", 5: "CRITICAL", 6: "EMERGENCY"}
    print(f"  HEARTBEAT: type={mav_type}({type_names.get(mav_type, '?')}) "
          f"autopilot={autopilot} base_mode=0x{base_mode:02X}(armed={bool(base_mode & 0x80)}) "
          f"status={system_status}({status_names.get(system_status, '?')}) mavlink_v={mavlink_version}")


def decode_gps_raw_int(payload: bytes) -> None:
    """Decode GPS_RAW_INT."""
    if len(payload) < 30:
        return
    fix_type = payload[8]
    lat = struct.unpack_from('<i', payload, 9)[0]
    lon = struct.unpack_from('<i', payload, 13)[0]
    alt = struct.unpack_from('<i', payload, 17)[0]
    eph = struct.unpack_from('<H', payload, 21)[0]
    epv = struct.unpack_from('<H', payload, 23)[0]
    vel = struct.unpack_from('<H', payload, 25)[0]
    sats = payload[29]
    print(f"  GPS: fix={fix_type} lat={lat/1e7:.7f} lon={lon/1e7:.7f} alt={alt/1000:.1f}m "
          f"eph={eph/100:.1f} vel={vel/100:.2f}m/s sats={sats}")


def decode_attitude(payload: bytes) -> None:
    """Decode ATTITUDE."""
    if len(payload) < 28:
        return
    roll = struct.unpack_from('<f', payload, 4)[0]
    pitch = struct.unpack_from('<f', payload, 8)[0]
    yaw = struct.unpack_from('<f', payload, 12)[0]
    print(f"  ATTITUDE: roll={math.degrees(roll):.1f} pitch={math.degrees(pitch):.1f} yaw={math.degrees(yaw):.1f}")


def decode_global_position_int(payload: bytes) -> None:
    """Decode GLOBAL_POSITION_INT."""
    if len(payload) < 28:
        return
    lat = struct.unpack_from('<i', payload, 4)[0]
    lon = struct.unpack_from('<i', payload, 8)[0]
    alt = struct.unpack_from('<i', payload, 12)[0]
    rel_alt = struct.unpack_from('<i', payload, 16)[0]
    vx = struct.unpack_from('<h', payload, 20)[0]
    vy = struct.unpack_from('<h', payload, 22)[0]
    vz = struct.unpack_from('<h', payload, 24)[0]
    hdg = struct.unpack_from('<H', payload, 26)[0]
    print(f"  POS: lat={lat/1e7:.7f} lon={lon/1e7:.7f} alt={alt/1000:.1f}m rel={rel_alt/1000:.1f}m "
          f"v=({vx/100:.2f},{vy/100:.2f},{vz/100:.2f}) hdg={hdg/100:.1f}")


def decode_vfr_hud(payload: bytes) -> None:
    """Decode VFR_HUD."""
    if len(payload) < 20:
        return
    groundspeed = struct.unpack_from('<f', payload, 4)[0]
    alt = struct.unpack_from('<f', payload, 8)[0]
    climb = struct.unpack_from('<f', payload, 12)[0]
    heading = struct.unpack_from('<h', payload, 16)[0]
    throttle = struct.unpack_from('<H', payload, 18)[0]
    print(f"  VFR: spd={groundspeed:.2f} alt={alt:.1f} climb={climb:.2f} hdg={heading} thr={throttle}%")


def decode_remoteid_419(payload: bytes) -> None:
    """
    Decode ArduPilot REMOTE_ID message (msgid 419).
    Based on ArduPilot MAVLink dialect definition.

    Note: Field layout based on ArduPilot's common.xml REMOTE_ID definition.
    Actual field order may differ - this is a best guess.
    """
    print(f"  REMOTE_ID (419) raw ({len(payload)} bytes): {payload[:64].hex()}")

    # ArduPilot REMOTE_ID (from ArduPilot MAVLink dialect):
    # This is actually the OPEN_DRONE_ID_MESSAGE_PACK or similar
    # Let me try to decode as standard OpenDroneID fields

    # For msgid 419 in ArduPilot dialect, it could be:
    # target_system(u8), target_component(u8), id_or_mac(u8[20]),
    # operator_location_type(u8), classification_type(u8),
    # operator_latitude(i32), operator_longitude(i32),
    # area_count(u16), area_radius(u16),
    # area_ceiling(f32), area_floor(f32),
    # category_eu(u8), class_eu(u8),
    # operator_altitude_geo(f32), timestamp(u32)
    # Total: 1+1+20+1+1+4+4+2+2+4+4+1+1+4+4 = 54 bytes

    if len(payload) >= 54:
        off = 0
        target_sys = payload[off]; off += 1
        target_comp = payload[off]; off += 1
        id_or_mac = payload[off:off+20]; off += 20
        op_loc_type = payload[off]; off += 1
        classif_type = payload[off]; off += 1
        op_lat = struct.unpack_from('<i', payload, off)[0]; off += 4
        op_lon = struct.unpack_from('<i', payload, off)[0]; off += 4
        area_count = struct.unpack_from('<H', payload, off)[0]; off += 2
        area_radius = struct.unpack_from('<H', payload, off)[0]; off += 2
        area_ceiling = struct.unpack_from('<f', payload, off)[0]; off += 4
        area_floor = struct.unpack_from('<f', payload, off)[0]; off += 4
        cat_eu = payload[off]; off += 1
        cls_eu = payload[off]; off += 1
        op_alt_geo = struct.unpack_from('<f', payload, off)[0]; off += 4
        timestamp = struct.unpack_from('<I', payload, off)[0]; off += 4

        try:
            id_str = id_or_mac.decode('ascii', errors='replace').strip('\x00')
        except:
            id_str = id_or_mac.hex()

        print(f"  target: sys={target_sys} comp={target_comp}")
        print(f"  id_or_mac: '{id_str}'")
        print(f"  op_loc_type: {op_loc_type}  classification: {classif_type}")
        print(f"  operator_pos: lat={op_lat/1e7:.7f} lon={op_lon/1e7:.7f}")
        print(f"  area: count={area_count} radius={area_radius} ceiling={area_ceiling:.1f} floor={area_floor:.1f}")
        print(f"  EU: cat={cat_eu} class={cls_eu}")
        print(f"  op_alt_geo: {op_alt_geo:.1f}m  timestamp: {timestamp}")


def decode_opendroneid(msgid: int, payload: bytes) -> None:
    """Decode standard MAVLink OpenDroneID messages (12900-12906)."""
    names = {
        12900: "BASIC_ID",
        12901: "LOCATION",
        12902: "AUTHORIZATION",
        12903: "SELF_ID",
        12904: "SYSTEM",
        12905: "OPERATOR_ID",
        12906: "MESSAGE_PACK",
    }
    name = names.get(msgid, f"UNKNOWN_{msgid}")
    print(f"  [{name}] ({len(payload)} bytes): {payload[:48].hex()}")


def main() -> None:
    log_dir = os.path.dirname(os.path.abspath(__file__))
    dat_files = sorted(glob.glob(os.path.join(log_dir, '*.DAT')))
    print(f"Found {len(dat_files)} .DAT files\n")

    for filepath in dat_files:
        parse_and_verify(filepath)


if __name__ == '__main__':
    main()
