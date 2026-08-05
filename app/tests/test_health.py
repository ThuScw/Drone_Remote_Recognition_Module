"""Health-judge tests."""
from __future__ import annotations

import time

from packet_builder import build_packet

from rid.decoder import decode_gb_packet
from rid.health import StreamAssessor, assess_packet
from rid.models import HealthLevel


def _pkt(**kwargs):
    now = int(time.time() * 1000)
    kwargs.setdefault("timestamp_ms", now)
    return decode_gb_packet(build_packet(**kwargs))


def test_healthy_packet_passes():
    issues = assess_packet(_pkt())
    assert issues == [], [i.message for i in issues]


def test_missing_realname_warns():
    issues = assess_packet(_pkt(realname="00000000"))
    codes = {i.code for i in issues}
    assert "REALNAME_EMPTY" in codes
    assert all(i.level == HealthLevel.WARN for i in issues)


def test_position_unknown_warns():
    issues = assess_packet(_pkt(ua_pos_unknown=True))
    codes = {i.code for i in issues}
    assert "POS_UNKNOWN" in codes


def test_invalid_status_fails():
    issues = assess_packet(_pkt(op_status=9))
    assert any(i.code == "STATUS_INVALID" for i in issues)


def test_module_fault_status_fails():
    issues = assess_packet(_pkt(op_status=5))
    assert any(i.code == "STATUS_FAIL" for i in issues)


def test_wrong_version_fails():
    issues = assess_packet(_pkt(version=0x01))
    assert any(i.code == "STRUCT_VER" for i in issues)


def test_uas_oid_forbidden_warns():
    issues = assess_packet(_pkt(uas_id="CPNYMDL00O123456789A"))
    codes = {i.code for i in issues}
    assert "UAS_OI" in codes


def test_timestamp_drift_warns():
    issues = assess_packet(_pkt(timestamp_ms=1700000000000))  # years ago
    assert any(i.code == "TS_DRIFT" for i in issues)


def test_stream_assessor_rate_and_staleness():
    clock = [1_000_000]
    def now():
        return clock[0]

    ass = StreamAssessor(window_s=10.0, now_func=now)
    rep = ass.report()
    assert rep.level == HealthLevel.FAIL  # no packets yet

    base = build_packet()
    for i in range(5):
        pkt = decode_gb_packet(base, received_at_ms=clock[0])
        ass.push(pkt)
        clock[0] += 800  # 1.25 pkt/s

    rep = ass.report()
    assert rep.level != HealthLevel.FAIL
    assert 0.8 <= rep.avg_rate_hz <= 1.5
    assert rep.stale_seconds <= 0.8

    # go silent for 6s → FAIL
    clock[0] += 6000
    rep = ass.report()
    assert any(i.code == "STALE" for i in rep.issues)
    assert rep.level == HealthLevel.FAIL
