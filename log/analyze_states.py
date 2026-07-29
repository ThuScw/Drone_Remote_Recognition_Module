#!/usr/bin/env python3
"""
分析不同场景下的HEARTBEAT和状态字段差异
"""

import struct
import os
import glob


def parse_heartbeat_from_file(filepath):
    """解析文件中的所有HEARTBEAT消息"""
    with open(filepath, 'rb') as f:
        data = f.read()

    fname = os.path.basename(filepath)
    print(f"\n{'='*70}")
    print(f"文件: {fname} ({len(data)} bytes)")
    print(f"{'='*70}\n")

    # 查找MAVLink v2帧
    heartbeats = []
    pos = 0
    while pos < len(data) - 12:
        if data[pos] == 0xFD:  # MAVLink v2
            plen = data[pos + 1]
            if plen <= 253 and pos + 12 + plen <= len(data):
                msgid = data[pos + 7] | (data[pos + 8] << 8) | (data[pos + 9] << 16)
                if msgid == 0:  # HEARTBEAT
                    payload = data[pos + 10: pos + 10 + plen]
                    if len(payload) >= 9:
                        custom_mode = struct.unpack_from('<I', payload, 0)[0]
                        mav_type = payload[4]
                        autopilot = payload[5]
                        base_mode = payload[6]
                        system_status = payload[7]
                        mavlink_version = payload[8]

                        armed = bool(base_mode & 0x80)
                        heartbeats.append({
                            'custom_mode': custom_mode,
                            'mav_type': mav_type,
                            'autopilot': autopilot,
                            'base_mode': base_mode,
                            'armed': armed,
                            'system_status': system_status,
                            'mavlink_version': mavlink_version,
                        })
                pos += 12 + plen
                continue
        pos += 1

    if not heartbeats:
        print("未找到HEARTBEAT消息\n")
        return

    # 统计
    type_names = {
        0: "GENERIC", 1: "FIXED_WING", 2: "QUADROTOR",
        3: "COAXIAL", 4: "HELICOPTER", 13: "VTOL_DUOROTOR"
    }
    status_names = {
        0: "UNINIT", 1: "BOOT", 2: "CALIBRATING",
        3: "STANDBY", 4: "ACTIVE", 5: "CRITICAL",
        6: "EMERGENCY", 7: "POWEROFF"
    }

    print(f"共找到 {len(heartbeats)} 条 HEARTBEAT 消息\n")

    # 显示第一条
    hb = heartbeats[0]
    print("第一条 HEARTBEAT:")
    print(f"  mav_type: {hb['mav_type']} ({type_names.get(hb['mav_type'], '?')})")
    print(f"  autopilot: {hb['autopilot']}")
    print(f"  base_mode: 0x{hb['base_mode']:02X}")
    print(f"  armed: {hb['armed']}")
    print(f"  system_status: {hb['system_status']} ({status_names.get(hb['system_status'], '?')})")
    print(f"  mavlink_version: {hb['mavlink_version']}")

    # 统计armed状态分布
    armed_count = sum(1 for h in heartbeats if h['armed'])
    disarmed_count = len(heartbeats) - armed_count
    print(f"\narmed 统计: armed={armed_count}, disarmed={disarmed_count}")

    # 统计system_status分布
    status_count = {}
    for h in heartbeats:
        s = h['system_status']
        status_count[s] = status_count.get(s, 0) + 1
    print("system_status 分布:")
    for s, count in sorted(status_count.items()):
        print(f"  {s} ({status_names.get(s, '?')}): {count}")

    # 检查状态变化
    if len(heartbeats) > 1:
        changes = []
        for i in range(1, len(heartbeats)):
            if heartbeats[i]['armed'] != heartbeats[i-1]['armed']:
                changes.append(f"  #{i}: armed {heartbeats[i-1]['armed']} → {heartbeats[i]['armed']}")
            if heartbeats[i]['system_status'] != heartbeats[i-1]['system_status']:
                changes.append(f"  #{i}: status {heartbeats[i-1]['system_status']} → {heartbeats[i]['system_status']}")
        if changes:
            print(f"\n状态变化:")
            for c in changes[:10]:
                print(c)
            if len(changes) > 10:
                print(f"  ... 共 {len(changes)} 处变化")

    print()


def main():
    log_dir = os.path.dirname(os.path.abspath(__file__))
    dat_files = sorted(glob.glob(os.path.join(log_dir, '*.DAT')))

    print(f"找到 {len(dat_files)} 个 .DAT 文件")

    for filepath in dat_files:
        parse_heartbeat_from_file(filepath)


if __name__ == '__main__':
    main()
