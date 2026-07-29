#!/usr/bin/env python3
"""
Generic binary analysis of serial port captures.
No protocol assumptions - just find patterns, periodicity, and structure.
"""

import os
import sys
import glob
import struct
from collections import Counter
from typing import Any


def analyze_raw(filepath: str) -> None:
    """Analyze raw binary data for patterns."""
    with open(filepath, 'rb') as f:
        data = f.read()

    fname = os.path.basename(filepath)
    print(f"\n{'='*80}")
    print(f"FILE: {fname}")
    print(f"SIZE: {len(data)} bytes")
    print(f"{'='*80}\n")

    # 1. Byte frequency distribution
    freq = Counter(data)
    print("Top 20 most frequent bytes:")
    for byte, count in freq.most_common(20):
        pct = count / len(data) * 100
        bar = '#' * int(pct)
        print(f"  0x{byte:02X} ({byte:>3d}): {count:>6d} ({pct:5.1f}%) {bar}")
    print()

    # 2. Find positions of specific marker bytes
    for marker in [0xFD, 0xFE, 0xFF, 0x00, 0x55, 0xAA]:
        positions = [i for i, b in enumerate(data[:2000]) if b == marker]
        if positions:
            gaps = [positions[i+1] - positions[i] for i in range(min(len(positions)-1, 30))]
            gap_counter = Counter(gaps)
            print(f"Byte 0x{marker:02X} positions (first 20): {positions[:20]}")
            print(f"  Gaps: {gaps[:20]}")
            if gap_counter:
                print(f"  Most common gaps: {gap_counter.most_common(5)}")
            print()

    # 3. Look for fixed-size frame patterns
    print("--- Frame detection: checking if 0xFD starts regular fixed-size frames ---")
    fd_positions = [i for i in range(len(data)) if data[i] == 0xFD]
    print(f"Total 0xFD occurrences: {len(fd_positions)}")

    if len(fd_positions) > 2:
        gaps = [fd_positions[i+1] - fd_positions[i] for i in range(len(fd_positions)-1)]
        gap_counter = Counter(gaps)
        print(f"Gap distribution between consecutive 0xFD bytes:")
        for gap, count in gap_counter.most_common(10):
            print(f"  gap={gap}: {count} times")
    print()

    # 4. Look for repeating patterns (autocorrelation-like)
    print("--- Auto-correlation: looking for periodic patterns ---")
    # Check if bytes repeat at certain intervals
    sample_size = min(1000, len(data) // 2)
    for period in [16, 20, 24, 28, 30, 32, 36, 40, 48, 50, 60, 64, 80, 100, 128]:
        if period * 2 > len(data):
            break
        matches = 0
        total = min(sample_size, len(data) - period)
        for i in range(total):
            if data[i] == data[i + period]:
                matches += 1
        pct = matches / total * 100
        if pct > 30:
            print(f"  Period {period:>3d}: {pct:.1f}% match")
    print()

    # 5. Show first 256 bytes as grouped hex with ASCII
    print("--- First 256 bytes (16 bytes per line) ---")
    for offset in range(0, min(256, len(data)), 16):
        chunk = data[offset:offset+16]
        hex_part = ' '.join(f'{b:02x}' for b in chunk)
        ascii_part = ''.join(chr(b) if 32 <= b < 127 else '.' for b in chunk)
        print(f"  {offset:06x}: {hex_part:<48s}  |{ascii_part}|")
    print()

    # 6. Try to detect structure using 0xFD as delimiter with different frame sizes
    print("--- Trying 0xFD as frame start with payload_len at offset+1 ---")
    pos = 0
    frame_sizes = Counter()
    frame_count = 0
    while pos < len(data) - 3 and frame_count < 100:
        if data[pos] == 0xFD:
            plen = data[pos + 1]
            if plen <= 253 and plen > 0:
                frame_size = 10 + plen + 2  # header(10) + payload + crc(2)
                # Check if next FD is at exactly frame_size distance
                next_fd = pos + frame_size
                if next_fd < len(data) and data[next_fd] == 0xFD:
                    frame_sizes[frame_size] += 1
                    frame_count += 1
                    if frame_count <= 5:
                        print(f"  Frame at {pos}: len_byte={plen}, frame_size={frame_size}")
        pos += 1
    if frame_sizes:
        print(f"  Frame sizes that chain: {frame_sizes.most_common(5)}")
    else:
        print("  No chaining frames found")
    print()


def compare_files(dat_files: list[str]) -> None:
    """Compare byte patterns across files to identify what changes between scenarios."""
    print("\n" + "=" * 80)
    print("CROSS-FILE COMPARISON")
    print("=" * 80 + "\n")

    file_data: dict[str, bytes] = {}
    for f in dat_files:
        with open(f, 'rb') as fh:
            file_data[os.path.basename(f)] = fh.read()

    for name, data in file_data.items():
        print(f"  {name}: {len(data)} bytes")
    print()

    # Check if files share common prefixes
    names = list(file_data.keys())
    if len(names) >= 2:
        d1 = file_data[names[0]]
        d2 = file_data[names[1]]
        common = 0
        for i in range(min(len(d1), len(d2))):
            if d1[i] == d2[i]:
                common += 1
            else:
                break
        print(f"Common prefix bytes between {names[0]} and {names[1]}: {common}")
        if common > 0:
            print(f"  Common prefix: {d1[:common].hex()}")
        print()


def main() -> None:
    log_dir = os.path.dirname(os.path.abspath(__file__))
    dat_files = sorted(glob.glob(os.path.join(log_dir, '*.DAT')))
    print(f"Found {len(dat_files)} .DAT files\n")

    for filepath in dat_files:
        analyze_raw(filepath)

    compare_files(dat_files)


if __name__ == '__main__':
    main()
