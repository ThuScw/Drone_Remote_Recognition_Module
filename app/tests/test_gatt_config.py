"""GATT config validation + encoding tests (mirror firmware rid_config.h)."""
from __future__ import annotations

from rid.gatt_config import (
    UAS_ID_LEN,
    REALNAME_LEN,
    encode_realname,
    encode_uas_id,
    validate_op_category,
    validate_realname,
    validate_ua_class,
    validate_uas_id,
    RidConfig,
)


def test_validate_uas_id():
    assert validate_uas_id("1581FA6QC25B500C2H74")  # firmware placeholder
    assert validate_uas_id("0123456789ABCDEFGHJK")
    assert not validate_uas_id("1581FA6QC25B500C2H7O")  # contains O
    assert not validate_uas_id("1581FA6QC25B500C2H7I")  # contains I
    assert not validate_uas_id("1581FA6QC25B500C2H7a")  # lowercase
    assert not validate_uas_id("1581FA6QC25B500C2H7 ")  # space
    assert not validate_uas_id("1581FA6QC25B500C2H7")   # 19 chars
    assert not validate_uas_id("1581FA6QC25B500C2H744")  # 21 chars
    assert not validate_uas_id("")                        # empty


def test_validate_realname():
    assert validate_realname("07564244")
    assert validate_realname("00000000")
    assert not validate_realname("0756424A")  # letter
    assert not validate_realname("0756424")   # 7 chars
    assert not validate_realname("075642440")  # 9 chars
    assert not validate_realname("0756 244")  # space
    assert not validate_realname("")           # empty


def test_validate_op_category():
    for v in (0, 1, 2, 3):
        assert validate_op_category(v)
    assert not validate_op_category(-1)
    assert not validate_op_category(4)


def test_validate_ua_class():
    for v in (0, 1, 2, 3, 4):
        assert validate_ua_class(v)
    assert not validate_ua_class(-1)
    assert not validate_ua_class(5)


def test_encode_lengths():
    assert len(encode_uas_id("1581FA6QC25B500C2H74")) == UAS_ID_LEN
    assert len(encode_realname("07564244")) == REALNAME_LEN
    assert encode_uas_id("1581FA6QC25B500C2H74") == b"1581FA6QC25B500C2H74"


def test_rid_config_validate_collects_errors():
    cfg = RidConfig(uas_id="1581FA6QC25B500C2H74", realname="07564244",
                    op_category=1, ua_class=1, state=1)
    assert cfg.validate() == []

    bad = RidConfig(uas_id="BAD", realname="x", op_category=9, ua_class=9)
    errs = bad.validate()
    assert len(errs) == 4


def test_rid_config_state_props():
    cfg = RidConfig(state=2)
    assert cfg.is_airborne
    assert cfg.state_name == "空中(写锁定)"
    cfg = RidConfig(state=0)
    assert not cfg.is_airborne
    assert cfg.state_name == "未配置"
