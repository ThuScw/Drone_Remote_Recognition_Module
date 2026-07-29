import struct
import math

def parse_tlog_final(path, label):
    with open(path, 'rb') as f:
        data = f.read()

    print(f"\n{'='*60}")
    print(f"{label} ({len(data)} bytes)")
    print(f"{'='*60}")

    # Format: MAVLink v2, 0xFD magic, 10-byte header, 3-byte msgid
    # From heuristic: GLOBAL_POSITION_INT (#33) works with this format

    pos = 0
    total = 0
    gps_raw = []
    global_pos = []
    heartbeat = []
    attitude = []
    vfr_hud = []
    highres_imu = []
    battery = []
    msg_counts = {}

    while pos < len(data) - 12:
        if data[pos] != 0xFD:
            pos += 1
            continue

        payload_len = data[pos + 1]
        if payload_len < 1 or payload_len > 250:
            pos += 1
            continue

        total_len = 10 + payload_len + 2  # v2: hdr(10) + payload + CRC(2)
        if pos + total_len > len(data):
            break

        # msgid: 3 bytes little-endian at offset 7
        msgid = data[pos + 7] | (data[pos + 8] << 8) | (data[pos + 9] << 16)
        payload = data[pos + 10 : pos + 10 + payload_len]
        pos += total_len
        total += 1
        msg_counts[msgid] = msg_counts.get(msgid, 0) + 1

        # GLOBAL_POSITION_INT (#33)
        if msgid == 33 and payload_len >= 28:
            time_ms = struct.unpack('<I', payload[0:4])[0]
            lat = struct.unpack('<i', payload[4:8])[0] / 1e7
            lon = struct.unpack('<i', payload[8:12])[0] / 1e7
            alt = struct.unpack('<i', payload[12:16])[0] / 1000.0
            rel_alt = struct.unpack('<i', payload[16:20])[0] / 1000.0
            vx = struct.unpack('<h', payload[20:22])[0] / 100.0
            vy = struct.unpack('<h', payload[22:24])[0] / 100.0
            vz = struct.unpack('<h', payload[24:26])[0] / 100.0
            hdg = struct.unpack('<H', payload[26:28])[0] / 100.0
            global_pos.append((time_ms, lat, lon, alt, rel_alt, hdg, vx, vy, vz))

        # GPS_RAW_INT (#24)
        elif msgid == 24 and payload_len >= 30:
            time_us = struct.unpack('<I', payload[0:4])[0]
            fix = payload[4]
            lat = struct.unpack('<i', payload[5:9])[0] / 1e7
            lon = struct.unpack('<i', payload[9:13])[0] / 1e7
            alt = struct.unpack('<i', payload[13:17])[0] / 1000.0
            eph = struct.unpack('<H', payload[17:19])[0] / 100.0
            epv = struct.unpack('<H', payload[19:21])[0] / 100.0
            vel = struct.unpack('<H', payload[21:23])[0] / 100.0
            cog = struct.unpack('<H', payload[23:25])[0] / 100.0
            sats = payload[25]
            gps_raw.append((lat, lon, alt, fix, sats, vel, cog))

        # HEARTBEAT (#0)
        elif msgid == 0 and payload_len >= 9:
            hb_type = payload[0]
            autopilot = payload[1]
            base_mode = payload[2]
            custom_mode = struct.unpack('<I', payload[3:7])[0]
            system_status = payload[7]
            mavlink_version = payload[8]
            heartbeat.append((base_mode, custom_mode, system_status))

        # ATTITUDE (#30)
        elif msgid == 30 and payload_len >= 16:
            time_ms = struct.unpack('<I', payload[0:4])[0]
            roll = math.degrees(struct.unpack('<f', payload[4:8])[0])
            pitch = math.degrees(struct.unpack('<f', payload[8:12])[0])
            yaw = math.degrees(struct.unpack('<f', payload[12:16])[0])
            rollspeed = math.degrees(struct.unpack('<f', payload[16:20])[0]) if payload_len >= 20 else 0
            pitchspeed = math.degrees(struct.unpack('<f', payload[20:24])[0]) if payload_len >= 24 else 0
            yawspeed = math.degrees(struct.unpack('<f', payload[24:28])[0]) if payload_len >= 28 else 0
            attitude.append((time_ms, roll, pitch, yaw, rollspeed, pitchspeed, yawspeed))

        # VFR_HUD (#74)
        elif msgid == 74 and payload_len >= 20:
            airspeed = struct.unpack('<f', payload[0:4])[0]
            groundspeed = struct.unpack('<f', payload[4:8])[0]
            heading = struct.unpack('<h', payload[8:10])[0]
            throttle = struct.unpack('<H', payload[10:12])[0]
            alt = struct.unpack('<f', payload[12:16])[0]
            climb = struct.unpack('<f', payload[16:20])[0]
            vfr_hud.append((groundspeed, heading, alt, climb, throttle))

    print(f"Parsed: {total} messages, {len(msg_counts)} msg types")

    # --- GPS_RAW_INT ---
    if gps_raw:
        lats = [m[0] for m in gps_raw]
        lons = [m[1] for m in gps_raw]
        print(f"\n--- GPS_RAW_INT (#24): {len(gps_raw)} msgs ---")
        print(f"  Lat: {min(lats):.7f} -> {max(lats):.7f}")
        print(f"  Lon: {min(lons):.7f} -> {max(lons):.7f}")
        seen = set()
        for lat, lon, alt, fix, sats, vel, cog in gps_raw:
            key = f"{lat:.6f},{lon:.6f}"
            if key not in seen:
                seen.add(key)
                if len(seen) <= 5:
                    print(f"  lat={lat:.7f}, lon={lon:.7f}, alt={alt:.1f}m, fix={fix}, sats={sats}, vel={vel:.1f}m/s")
    else:
        print("\n--- GPS_RAW_INT (#24): 0 msgs ---")

    # --- GLOBAL_POSITION_INT ---
    print(f"\n--- GLOBAL_POSITION_INT (#33): {len(global_pos)} msgs ---")
    if global_pos:
        lats = [m[1] for m in global_pos]
        rel_alts = [m[4] for m in global_pos]
        speeds = [math.sqrt(m[6]**2 + m[7]**2) for m in global_pos]
        hdgs = [m[5] for m in global_pos]

        print(f"  Lat range: {min(lats):.7f} -> {max(lats):.7f}")
        print(f"  Lon range: {min(lons):.7f} -> {max(lons):.7f}")
        print(f"  Rel Alt: {min(rel_alts):.2f}m -> {max(rel_alts):.2f}m")
        print(f"  Speed: {min(speeds):.2f}m/s -> {max(speeds):.2f}m/s")
        print(f"  Heading: {min(hdgs):.1f} -> {max(hdgs):.1f}")

        # Find flight phases
        flying = [(m[0], m[1], m[2], m[3], m[4], m[5], math.sqrt(m[6]**2 + m[7]**2), m[8])
                  for m in global_pos if m[4] > 2.0]  # rel_alt > 2m = flying

        if flying:
            print(f"\n  *** IN-FLIGHT DATA: {len(flying)} samples ***")
            print(f"  Max altitude (rel): {max(m[4] for m in global_pos):.1f}m")
            print(f"  Max speed: {max(speeds):.1f}m/s")
            # Show climb
            alt_start = global_pos[0][4]
            max_alt = max(m[4] for m in global_pos)
            print(f"  Altitude change: {alt_start:.1f}m -> {max_alt:.1f}m (climb: {max_alt - alt_start:.1f}m)")

            # Show samples during flight
            print(f"\n  Flight samples:")
            step = max(1, len(flying) // 15)
            for i in range(0, len(flying), step):
                t, lat, lon, alt, rel_alt, hdg, spd, vz = flying[i]
                print(f"    t={t/1000:.1f}s: lat={lat:.6f}, lon={lon:.6f}, rel_alt={rel_alt:.1f}m, spd={spd:.1f}m/s, hdg={hdg:.0f}deg, vz={vz:.1f}m/s")
        else:
            print("\n  *** NEVER TOOK OFF (rel_alt never > 2m) ***")
            # Check if armed
            if heartbeat:
                armed = [h for h in heartbeat if h[0] & 0x80]
                print(f"  Armed samples: {len(armed)}/{len(heartbeat)}")

        # First and last positions
        first = global_pos[0]
        last = global_pos[-1]
        dlat = (last[1] - first[1]) * 111000
        dlon = (last[2] - first[2]) * 111000 * math.cos(math.radians(first[1]))
        dist = math.sqrt(dlat*dlat + dlon*dlon)
        print(f"\n  Start: lat={first[1]:.7f}, lon={first[2]:.7f}, rel_alt={first[4]:.1f}m")
        print(f"  End:   lat={last[1]:.7f}, lon={last[2]:.7f}, rel_alt={last[4]:.1f}m")
        print(f"  Duration: {(global_pos[-1][0] - global_pos[0][0])/1000:.1f}s")
        print(f"  Total distance: {dist:.1f}m")

    # --- HEARTBEAT ---
    print(f"\n--- HEARTBEAT (#0): {len(heartbeat)} msgs ---")
    if heartbeat:
        armed_changes = []
        prev_armed = None
        for base_mode, custom_mode, status in heartbeat:
            armed = bool(base_mode & 0x80)
            if prev_armed is None or armed != prev_armed:
                armed_changes.append(armed)
                prev_armed = armed
        for a in armed_changes:
            print(f"  ARM STATE CHANGE: armed={a}")
        if len(armed_changes) == 1:
            print(f"  Final armed state: {armed_changes[0]} (never changed)")

    # --- ATTITUDE ---
    print(f"\n--- ATTITUDE (#30): {len(attitude)} msgs ---")
    if attitude:
        max_roll = max(abs(m[1]) for m in attitude)
        max_pitch = max(abs(m[2]) for m in attitude)
        flying_att = [(m[0], m[1], m[2], m[3], m[4], m[5], m[6])
                      for m in attitude if abs(m[1]) > 15 or abs(m[2]) > 15]
        print(f"  Max roll: {max_roll:.1f}deg, max pitch: {max_pitch:.1f}deg")
        print(f"  In-flight attitudes (|r|>15 or |p|>15): {len(flying_att)}")
        if flying_att:
            for t, r, p, y, rs, ps, ys in flying_att[:5]:
                print(f"    t={t/1000:.1f}s: roll={r:.1f}, pitch={p:.1f}, yaw={y:.1f}")

    # --- VFR_HUD ---
    print(f"\n--- VFR_HUD (#74): {len(vfr_hud)} msgs ---")
    if vfr_hud:
        max_gs = max(m[0] for m in vfr_hud)
        max_alt = max(m[2] for m in vfr_hud)
        moving = [m for m in vfr_hud if m[0] > 1.0]
        print(f"  Max groundspeed: {max_gs:.1f}m/s, max alt: {max_alt:.1f}m")
        print(f"  Moving (gs>1m/s): {len(moving)}/{len(vfr_hud)}")
        if moving:
            for gs, hdg, alt, climb, thr in moving[:5]:
                print(f"    gs={gs:.1f}m/s, hdg={hdg}deg, alt={alt:.1f}m, climb={climb:.1f}m/s, thr={thr}")

# Parse both files
parse_tlog_final('C:/Users/86134/Desktop/Code/ESP/Drone_Remote_Recognition_Module/logs/Telemetry/2026-07-28 10-35-05.tlog', 'TLOG 1 (10-35-05)')
parse_tlog_final('C:/Users/86134/Desktop/Code/ESP/Drone_Remote_Recognition_Module/logs/Telemetry/2026-07-28 10-56-25.tlog', 'TLOG 2 (10-56-25)')
