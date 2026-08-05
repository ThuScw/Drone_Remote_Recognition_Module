"""Built-in judge: tells whether the RID module / its broadcast is healthy.

Per-packet checks mirror the firmware's own validation
(`gb46750_validateFlightData` in main/protocol/rid_messages.cpp) plus
protocol-structure checks and GB 46750-2025 compliance rules (5.1.3 rate,
field presence, real-name registration, sentinel handling).

Stream-level checks (rate, staleness, frozen content) live in
`StreamAssessor`.
"""
from __future__ import annotations

import re
import time
from collections import deque
from typing import Callable

from .decoder import (
    GB46750_DATA_TYPE,
    GB46750_VERSION,
    OP_STATUS,
    SENT_SPEED_HEADING,
    SENT_VSPEED,
)
from .models import DecodedPacket, HealthIssue, HealthLevel, HealthReport

UAS_ID_CHARSET = re.compile(r"^[0-9A-Z]+$")
UAS_ID_FORBIDDEN = set("OI")  # 字符范围: 0-9 及除 O/I 外的大写字母

GB_MIN_RATE_HZ = 1.0     # GB 46750-2025 5.1.3: 广播间隔 ≤ 1s
FRESH_THRESHOLD_S = 2.0  # DATA_FRESH_THRESHOLD_MS = 2000


def _now_ms() -> int:
    return int(time.monotonic() * 1000)


def _is_nan(v: float) -> bool:
    return v != v


def assess_packet(pkt: DecodedPacket) -> list[HealthIssue]:
    """Judging a single decoded packet. Returns issues (empty = healthy)."""
    issues: list[HealthIssue] = []

    # --- structure ---
    if pkt.data_type != GB46750_DATA_TYPE:
        issues.append(HealthIssue(HealthLevel.FAIL, "STRUCT_TYPE",
                                  f"dataType=0x{pkt.data_type:02X}，应为 0xFF"))
    if pkt.version != GB46750_VERSION:
        issues.append(HealthIssue(HealthLevel.FAIL, "STRUCT_VER",
                                  f"版本=0x{pkt.version:02X}，应为 0x20 (V1.0)"))
    if pkt.structure_error:
        issues.append(HealthIssue(HealthLevel.FAIL, "STRUCT_LEN", pkt.structure_error))

    # --- UAS_ID (001, M) ---
    uas = pkt.uas_id
    if not uas:
        issues.append(HealthIssue(HealthLevel.FAIL, "UAS_EMPTY", "唯一产品识别码为空"))
    elif len(uas) != 20:
        issues.append(HealthIssue(HealthLevel.WARN, "UAS_LEN",
                                  f"唯一产品识别码长度 {len(uas)}，应为 20"))
    else:
        if not UAS_ID_CHARSET.match(uas):
            issues.append(HealthIssue(HealthLevel.WARN, "UAS_CHARSET",
                                      "唯一产品识别码含非 0-9/A-Z 字符"))
        if any(ch in UAS_ID_FORBIDDEN for ch in uas):
            issues.append(HealthIssue(HealthLevel.WARN, "UAS_OI",
                                      "唯一产品识别码含禁用字符 O/I"))

    # --- 实名登记 (002, M) ---
    if pkt.realname == "00000000" or not pkt.realname:
        issues.append(HealthIssue(HealthLevel.WARN, "REALNAME_EMPTY",
                                  "实名登记号为默认值/空，未完成 UOM 实名登记"))
    elif len(pkt.realname) != 8:
        issues.append(HealthIssue(HealthLevel.WARN, "REALNAME_LEN",
                                  f"实名登记号长度 {len(pkt.realname)}，应为 8"))

    # --- 运行类别/无人机分类 (M) ---
    if pkt.op_category not in (1, 2, 3):
        issues.append(HealthIssue(HealthLevel.WARN, "OP_CATEGORY",
                                  f"运行类别 {pkt.op_category} 无效（应为 1/2/3）"))
    if pkt.ua_class not in (0, 1, 2, 3, 4):
        issues.append(HealthIssue(HealthLevel.WARN, "UA_CLASS",
                                  f"无人机分类 {pkt.ua_class} 无效（应为 0-4）"))

    # --- 位置 (008, M) ---
    if _is_nan(pkt.ua_lat) or _is_nan(pkt.ua_lon):
        issues.append(HealthIssue(HealthLevel.WARN, "POS_UNKNOWN",
                                  "无人机位置未知（0xFFFFFFFF）— 未收到 GPS/飞控数据"))
    else:
        if not (-90.0 <= pkt.ua_lat <= 90.0):
            issues.append(HealthIssue(HealthLevel.FAIL, "LAT_RANGE",
                                      f"纬度 {pkt.ua_lat:.7f} 超出 [-90,90]"))
        if not (-180.0 <= pkt.ua_lon <= 180.0):
            issues.append(HealthIssue(HealthLevel.FAIL, "LON_RANGE",
                                      f"经度 {pkt.ua_lon:.7f} 超出 [-180,180]"))

    # --- 高度 (013, M) ---
    if _is_nan(pkt.geo_alt):
        issues.append(HealthIssue(HealthLevel.WARN, "GEOALT_UNKNOWN",
                                  "大地高度未知（0）— 未收到高度数据"))
    elif not (-1000.0 <= pkt.geo_alt <= 10000.0):
        issues.append(HealthIssue(HealthLevel.FAIL, "GEOALT_RANGE",
                                  f"大地高度 {pkt.geo_alt:.1f}m 超出 [-1000,10000]"))

    # --- 航迹/地速 (009/010, M) ---
    if _is_nan(pkt.heading):
        issues.append(HealthIssue(HealthLevel.WARN, "HEADING_UNKNOWN",
                                  "航迹角未知（0xFFFF）"))
    elif not (0.0 <= pkt.heading < 360.0):
        issues.append(HealthIssue(HealthLevel.FAIL, "HEADING_RANGE",
                                  f"航迹角 {pkt.heading:.1f}° 超出 [0,360)"))
    if _is_nan(pkt.speed):
        issues.append(HealthIssue(HealthLevel.WARN, "SPEED_UNKNOWN",
                                  "地速未知（0xFFFF）"))
    elif not (0.0 <= pkt.speed <= 6553.5):
        issues.append(HealthIssue(HealthLevel.FAIL, "SPEED_RANGE",
                                  f"地速 {pkt.speed:.1f}m/s 超出范围"))

    # --- 运行状态 (015, M) ---
    if pkt.op_status not in OP_STATUS:
        issues.append(HealthIssue(HealthLevel.FAIL, "STATUS_INVALID",
                                  f"运行状态 {pkt.op_status} 无效（应为 0-5）"))
    elif pkt.op_status in (4, 5):
        issues.append(HealthIssue(HealthLevel.FAIL, "STATUS_FAIL",
                                  f"模块上报故障状态：{OP_STATUS[pkt.op_status]}"))

    # --- 遥控站位置 (006, M) ---
    if _is_nan(pkt.op_lat) or _is_nan(pkt.op_lon):
        issues.append(HealthIssue(HealthLevel.WARN, "OPPOS_UNKNOWN",
                                  "遥控站位置未知 — 未配置起飞点/Home"))
    elif not (-90.0 <= pkt.op_lat <= 90.0 and -180.0 <= pkt.op_lon <= 180.0):
        issues.append(HealthIssue(HealthLevel.FAIL, "OPPOS_RANGE",
                                  "遥控站位置超出合理范围"))

    # --- 时间戳 (020, M) ---
    if pkt.timestamp_ms == 0:
        issues.append(HealthIssue(HealthLevel.WARN, "TS_UNSYNCED",
                                  "时间戳为 0 — 模块尚未 GPS 授时"))
    else:
        now = int(time.time() * 1000)
        drift = abs(now - pkt.timestamp_ms)
        if drift > 5 * 60 * 1000:
            issues.append(HealthIssue(HealthLevel.WARN, "TS_DRIFT",
                                      f"时间戳与电脑时钟偏差 {drift / 1000:.0f}s，GPS 授时可能异常"))

    return issues


class StreamAssessor:
    """Accumulates distinct decoded packets and produces a stream verdict."""

    def __init__(self, window_s: float = 10.0, now_func: Callable[[], int] | None = None):
        self._window_s = window_s
        self._now = now_func or _now_ms
        self._times: deque[int] = deque()
        self._last_pkt: DecodedPacket | None = None
        self._last_raw: bytes = b""
        self._ident_seq = 0
        self._start_ms: int | None = None

    def push(self, pkt: DecodedPacket) -> None:
        now = self._now()
        if self._start_ms is None:
            self._start_ms = now
        cutoff = now - int(self._window_s * 1000)
        while self._times and self._times[0] < cutoff:
            self._times.popleft()
        self._times.append(now)

        if pkt.raw == self._last_raw:
            self._ident_seq += 1
        else:
            self._ident_seq = 1
        self._last_raw = pkt.raw
        self._last_pkt = pkt

    def reset(self) -> None:
        self._times.clear()
        self._last_pkt = None
        self._last_raw = b""
        self._ident_seq = 0
        self._start_ms = None

    def report(self) -> HealthReport:
        now = self._now()
        rep = HealthReport()
        if self._last_pkt is None:
            rep.level = HealthLevel.FAIL
            rep.note = "尚未收到任何数据包"
            return rep

        elapsed_s = (now - self._start_ms) / 1000.0 if self._start_ms else 0.0
        if self._times:
            rep.avg_rate_hz = len(self._times) / min(elapsed_s, self._window_s)
        rep.packets_seen = len(self._times)
        rep.stale_seconds = (now - self._times[-1]) / 1000.0

        # --- packet-level issues from the latest packet ---
        pkt_issues = assess_packet(self._last_pkt)
        rep.issues = list(pkt_issues)
        rep.packets_ok = 1 if not pkt_issues else 0

        # --- rate (GB 5.1.3: ≥ 1/s) ---
        if elapsed_s >= 3.0:
            if rep.packets_seen == 0:
                rep.issues.append(HealthIssue(HealthLevel.FAIL, "RATE_ZERO",
                                              f"近 {self._window_s:.0f}s 内未收到数据包"))
            elif rep.avg_rate_hz < GB_MIN_RATE_HZ * 0.5:
                rep.issues.append(HealthIssue(HealthLevel.WARN, "RATE_LOW",
                                              f"广播速率仅 {rep.avg_rate_hz:.1f} 包/s，低于 1 包/s 标准"))
            elif rep.avg_rate_hz < GB_MIN_RATE_HZ:
                rep.issues.append(HealthIssue(HealthLevel.WARN, "RATE_SLOW",
                                              f"广播速率 {rep.avg_rate_hz:.1f} 包/s，略低于 1 包/s"))

        # --- staleness ---
        if rep.stale_seconds > 5.0:
            rep.issues.append(HealthIssue(HealthLevel.FAIL, "STALE",
                                          f"已 {rep.stale_seconds:.0f}s 无新数据"))
        elif rep.stale_seconds > FRESH_THRESHOLD_S:
            rep.issues.append(HealthIssue(HealthLevel.WARN, "STALE",
                                          f"数据已 {rep.stale_seconds:.0f}s 未更新"))

        # --- frozen content (data source stalled, not just RF silence) ---
        if self._ident_seq >= 5 and self._last_pkt.timestamp_ms != 0:
            rep.issues.append(HealthIssue(HealthLevel.WARN, "FROZEN",
                                          "广播内容长时间未变化 — 数据源可能停滞"))

        # --- verdict = worst issue ---
        worst = max((i.level for i in rep.issues), default=HealthLevel.PASS)
        rep.level = HealthLevel(worst)

        if rep.level == HealthLevel.PASS:
            rep.note = "模块工作正常，广播符合 GB 46750-2025 要求"
        elif rep.level == HealthLevel.WARN:
            rep.note = "存在可改善项，不影响基本广播"
        else:
            rep.note = "存在故障，请根据下方问题清单排查"

        return rep
