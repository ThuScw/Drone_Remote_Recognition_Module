"""Data models shared by the decoder, health judge and GUI."""
from __future__ import annotations

import enum
from dataclasses import dataclass, field
from datetime import datetime, timezone


class HealthLevel(enum.IntEnum):
    """Overall health verdict for a packet / stream."""

    PASS = 0
    WARN = 1
    FAIL = 2


@dataclass
class HealthIssue:
    """A single problem found by the judge."""

    level: HealthLevel
    code: str
    message: str

    @property
    def label(self) -> str:
        return {
            HealthLevel.PASS: "通过",
            HealthLevel.WARN: "警告",
            HealthLevel.FAIL: "故障",
        }[self.level]


@dataclass
class DecodedPacket:
    """Decoded GB 46750-2025 packet plus reception metadata."""

    # --- reception metadata ---
    address: str = ""            # BLE MAC address, e.g. "A4:CF:12:34:56:78"
    rssi: int = 0
    received_at_ms: int = 0      # monotonic ms (GUI clock)
    source: str = "ble"          # "ble" or "manual" or "serial"

    # --- raw ---
    raw: bytes = b""
    data_type: int = 0           # 0xFF
    version: int = 0             # 0x20 = V1.0
    declared_len: int = 0        # dataLength (content bytes)
    data_id: bytes = b""         # 3-byte data identifier
    content_len: int = 0         # parsed content length actually consumed

    # --- GB 46750 fields ---
    uas_id: str = ""
    realname: str = ""
    op_category: int = -1        # -1 = field absent/invalid
    ua_class: int = -1
    op_loc_type: int = -1
    op_lat: float = float("nan")
    op_lon: float = float("nan")
    op_alt: float = float("nan")
    ua_lat: float = float("nan")
    ua_lon: float = float("nan")
    heading: float = float("nan")
    speed: float = float("nan")
    rel_height: float = float("nan")
    vspeed: float = float("nan")
    geo_alt: float = float("nan")
    baro_alt: float = float("nan")
    op_status: int = -1
    coord_sys: int = -1
    horiz_acc: int = -1
    vert_acc: int = -1
    speed_acc: int = -1
    timestamp_ms: int = 0        # 0 = not GPS-synced
    ts_acc: int = -1

    # --- raw-availability flags (for UI) ---
    has_rel_height: bool = False
    has_vspeed: bool = False
    has_baro_alt: bool = False
    structure_error: str = ""    # non-empty if packet header/parse failed

    # --- human-readable formatting ---
    fmt: dict = field(default_factory=dict)

    @property
    def timestamp_utc(self) -> str:
        if self.timestamp_ms <= 0:
            return "未授时"
        try:
            return datetime.fromtimestamp(
                self.timestamp_ms / 1000.0, tz=timezone.utc
            ).strftime("%Y-%m-%d %H:%M:%S.%f")[:-3] + " UTC"
        except (OverflowError, OSError, ValueError):
            return f"无效({self.timestamp_ms})"


@dataclass
class HealthReport:
    """Result of judging one packet or a stream of packets."""

    level: HealthLevel = HealthLevel.PASS
    issues: list[HealthIssue] = field(default_factory=list)
    packets_seen: int = 0
    packets_ok: int = 0
    avg_rate_hz: float = 0.0     # distinct packets per second over window
    stale_seconds: float = 0.0   # seconds since last packet (negative if none)
    note: str = ""

    @property
    def verdict_label(self) -> str:
        return {HealthLevel.PASS: "正常", HealthLevel.WARN: "警告", HealthLevel.FAIL: "故障"}[
            self.level
        ]

    def worst_issue(self) -> HealthIssue | None:
        return self.issues[0] if self.issues else None
