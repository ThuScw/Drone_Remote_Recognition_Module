#!/usr/bin/env python3
"""
Proper MAVLink parser using pymavlink's low-level API.
"""

import os
import glob
import struct
import math
from typing import Any

from pymavlink import mavlink
from pymavlink.dialects.v20 import ardupilotmega as apm


def parse_file(filepath: str) -> None:
    """Parse a .DAT file using pymavlink's message parser."""
    fname = os.path.basename(filepath)
    print(f"\n{'='*80}")
    print(f"FILE: {fname}")
    print(f"{'='*80}\n")

    with open(filepath, 'rb') as f:
        data = f.read()

    # Create a MAVLink parser instance
    mav = apm.MAVLink(None)  # No output file

    msg_counts: dict[str, int] = {}
    total_parsed = 0
    crc_fail = 0
    first_msgs: list[Any] = []
    position_samples: list[dict[str, float]] = []
    attitude_samples: list[dict[str, float]] = []
    gps_samples: list[dict[str, Any]] = []
    heartbeat_msgs: list[Any] = []
    opendroneid_msgs: list[Any] = []
    vfr_hud_samples: list[dict[str, float]] = []
    home_msgs: list[Any] = []

    # Feed bytes to the parser one at a time
    for byte in data:
        try:
            msg = mav.parse_char(bytes([byte]))
            if msg is not None:
                total_parsed += 1
                mtype = msg.get_type()
                msg_counts[mtype] = msg_counts.get(mtype, 0) + 1

                if len(first_msgs) < 10:
                    first_msgs.append(msg)

                if mtype == 'GLOBAL_POSITION_INT':
                    position_samples.append({
                        'time_boot_ms': msg.time_boot_ms,
                        'lat': msg.lat / 1e7,
                        'lon': msg.lon / 1e7,
                        'alt': msg.alt / 1000,
                        'rel_alt': msg.relative_alt / 1000,
                        'vx': msg.vx / 100,
                        'vy': msg.vy / 100,
                        'vz': msg.vz / 100,
                        'hdg': msg.hdg / 100 if msg.hdg != 65535 else float('nan'),
                    })
                elif mtype == 'ATTITUDE':
                    attitude_samples.append({
                        'time_boot_ms': msg.time_boot_ms,
                        'roll': math.degrees(msg.roll),
                        'pitch': math.degrees(msg.pitch),
                        'yaw': math.degrees(msg.yaw),
                    })
                elif mtype == 'GPS_RAW_INT':
                    gps_samples.append({
                        'time_usec': msg.time_usec,
                        'fix_type': msg.fix_type,
                        'lat': msg.lat / 1e7,
                        'lon': msg.lon / 1e7,
                        'alt': msg.alt / 1000,
                        'eph': msg.eph / 100,
                        'epv': msg.epv / 100,
                        'vel': msg.vel / 100,
                        'cog': msg.cog / 100,
                        'sats': msg.satellites_visible,
                    })
                elif mtype == 'HEARTBEAT':
                    heartbeat_msgs.append(msg)
                elif mtype == 'VFR_HUD':
                    vfr_hud_samples.append({
                        'airspeed': msg.airspeed,
                        'groundspeed': msg.groundspeed,
                        'alt': msg.alt,
                        'climb': msg.climb,
                        'heading': msg.heading,
                        'throttle': msg.throttle,
                    })
                elif mtype == 'HOME_POSITION':
                    home_msgs.append(msg)
                elif mtype.startswith('OPEN_DRONE_ID'):
                    opendroneid_msgs.append(msg)
        except Exception:
            pass

    print(f"Total messages parsed: {total_parsed}")
    print(f"CRC failures (estimated from parser): {crc_fail}")
    print()

    # Message distribution
    print("--- Message type distribution ---")
    for mtype, count in sorted(msg_counts.items(), key=lambda x: -x[1]):
        print(f"  {mtype:<35s}: {count}")
    print()

    # First 5 messages
    print("--- First 5 messages ---")
    for m in first_msgs[:5]:
        fields = m.to_dict()
        mtype = fields.pop('mavpackettype', '?')
        print(f"  [{mtype}] {fields}")
    print()

    # HEARTBEAT analysis
    if heartbeat_msgs:
        print(f"--- HEARTBEAT ({len(heartbeat_msgs)} messages) ---")
        hb = heartbeat_msgs[0]
        type_names = {0: "GENERIC", 1: "FIXED_WING", 2: "QUADROTOR", 13: "VTOL_DUOROTOR"}
        status_names = {0: "UNINIT", 1: "BOOT", 3: "STANDBY", 4: "ACTIVE", 5: "CRITICAL", 6: "EMERGENCY"}
        print(f"  mav_type: {hb.type} ({type_names.get(hb.type, '?')})")
        print(f"  autopilot: {hb.autopilot}")
        print(f"  base_mode: 0x{hb.base_mode:02X} (armed={bool(hb.base_mode & 0x80)})")
        print(f"  system_status: {hb.system_status} ({status_names.get(hb.system_status, '?')})")
        print(f"  mavlink_version: {hb.mavlink_version}")
        print()

    # Position summary
    if position_samples:
        print(f"--- GLOBAL_POSITION_INT ({len(position_samples)} samples) ---")
        lats = [p['lat'] for p in position_samples]
        lons = [p['lon'] for p in position_samples]
        alts = [p['alt'] for p in position_samples]
        rel_alts = [p['rel_alt'] for p in position_samples]
        print(f"  Lat: {min(lats):.7f} ~ {max(lats):.7f}")
        print(f"  Lon: {min(lons):.7f} ~ {max(lons):.7f}")
        print(f"  Alt (MSL): {min(alts):.1f} ~ {max(alts):.1f} m")
        print(f"  Rel alt: {min(rel_alts):.1f} ~ {max(rel_alts):.1f} m")
        p0 = position_samples[0]
        pN = position_samples[-1]
        print(f"  First: t={p0['time_boot_ms']}ms lat={p0['lat']:.7f} lon={p0['lon']:.7f} alt={p0['alt']:.1f} rel={p0['rel_alt']:.1f} hdg={p0['hdg']:.1f}")
        print(f"  Last:  t={pN['time_boot_ms']}ms lat={pN['lat']:.7f} lon={pN['lon']:.7f} alt={pN['alt']:.1f} rel={pN['rel_alt']:.1f} hdg={pN['hdg']:.1f}")
        print()

    # Attitude summary
    if attitude_samples:
        print(f"--- ATTITUDE ({len(attitude_samples)} samples) ---")
        rolls = [a['roll'] for a in attitude_samples]
        pitches = [a['pitch'] for a in attitude_samples]
        yaws = [a['yaw'] for a in attitude_samples]
        print(f"  Roll: {min(rolls):.1f} ~ {max(rolls):.1f} deg")
        print(f"  Pitch: {min(pitches):.1f} ~ {max(pitches):.1f} deg")
        print(f"  Yaw: {min(yaws):.1f} ~ {max(yaws):.1f} deg")
        a0 = attitude_samples[0]
        print(f"  First: t={a0['time_boot_ms']}ms roll={a0['roll']:.1f} pitch={a0['pitch']:.1f} yaw={a0['yaw']:.1f}")
        print()

    # GPS summary
    if gps_samples:
        print(f"--- GPS_RAW_INT ({len(gps_samples)} samples) ---")
        fix_types = [g['fix_type'] for g in gps_samples]
        sats = [g['sats'] for g in gps_samples]
        print(f"  Fix types: {sorted(set(fix_types))}")
        print(f"  Satellites: min={min(sats)} max={max(sats)}")
        valid_gps = [g for g in gps_samples if g['fix_type'] >= 2]
        if valid_gps:
            lats = [g['lat'] for g in valid_gps]
            lons = [g['lon'] for g in valid_gps]
            print(f"  Valid GPS positions: {len(valid_gps)}")
            print(f"  Lat: {min(lats):.7f} ~ {max(lats):.7f}")
            print(f"  Lon: {min(lons):.7f} ~ {max(lons):.7f}")
            g0 = valid_gps[0]
            print(f"  First valid: fix={g0['fix_type']} lat={g0['lat']:.7f} lon={g0['lon']:.7f} alt={g0['alt']:.1f} sats={g0['sats']} eph={g0['eph']:.1f}")
        print()

    # VFR_HUD summary
    if vfr_hud_samples:
        print(f"--- VFR_HUD ({len(vfr_hud_samples)} samples) ---")
        speeds = [v['groundspeed'] for v in vfr_hud_samples]
        alts = [v['alt'] for v in vfr_hud_samples]
        climbs = [v['climb'] for v in vfr_hud_samples]
        print(f"  Groundspeed: {min(speeds):.2f} ~ {max(speeds):.2f} m/s")
        print(f"  Alt: {min(alts):.1f} ~ {max(alts):.1f} m")
        print(f"  Climb: {min(climbs):.2f} ~ {max(climbs):.2f} m/s")
        print()

    # Home position
    if home_msgs:
        print(f"--- HOME_POSITION ({len(home_msgs)} messages) ---")
        h = home_msgs[0]
        print(f"  lat={h.latitude/1e7:.7f} lon={h.longitude/1e7:.7f} alt={h.altitude/1000:.1f}m")
        print()

    # OpenDroneID messages
    if opendroneid_msgs:
        print(f"--- OpenDroneID Messages ({len(opendroneid_msgs)} total) ---")
        odid_types: dict[str, int] = {}
        for m in opendroneid_msgs:
            t = m.get_type()
            odid_types[t] = odid_types.get(t, 0) + 1
        print(f"  Types: {odid_types}")
        # Show first of each type
        shown_types: set[str] = set()
        for m in opendroneid_msgs:
            t = m.get_type()
            if t in shown_types:
                continue
            shown_types.add(t)
            print(f"\n  === First {t} ===")
            fields = m.to_dict()
            fields.pop('mavpackettype', None)
            for k, v in fields.items():
                print(f"    {k}: {v}")
        print()
    else:
        print("--- No OpenDroneID messages found ---")
        print()


def main() -> None:
    log_dir = os.path.dirname(os.path.abspath(__file__))
    dat_files = sorted(glob.glob(os.path.join(log_dir, '*.DAT')))
    print(f"Found {len(dat_files)} .DAT files\n")

    for filepath in dat_files:
        parse_file(filepath)


if __name__ == '__main__':
    main()
