"""GATT configuration client for the RID module.

The firmware exposes a private GATT service (16-bit ``0xFFF0`` → 128-bit
``0000FFF0-0000-1000-8000-00805F9B34FB``) used to reverse-configure the four
identity fields before takeoff:

    FFF1  唯一产品识别码  (20 ASCII)   write/read
    FFF2  实名登记标志   (8 ASCII)     write/read
    FFF3  运行类别       (1 byte)      write/read
    FFF4  无人机分类     (1 byte)      write/read
    FFF5  状态          (1 byte)       read/notify

The module only accepts writes while on the ground (state != 2). Field
validation/encoding here mirrors the firmware (`main/gatt/rid_config.h`) so bad
input is rejected before it reaches the radio.
"""
from __future__ import annotations

from dataclasses import dataclass

# 16-bit → 128-bit Bluetooth SIG base UUID expansion
SERVICE_UUID = "0000fff0-0000-1000-8000-00805f9b34fb"
CHAR_UAS_ID = "0000fff1-0000-1000-8000-00805f9b34fb"
CHAR_REALNAME = "0000fff2-0000-1000-8000-00805f9b34fb"
CHAR_OP_CATEGORY = "0000fff3-0000-1000-8000-00805f9b34fb"
CHAR_UA_CLASS = "0000fff4-0000-1000-8000-00805f9b34fb"
CHAR_STATE = "0000fff5-0000-1000-8000-00805f9b34fb"

UAS_ID_LEN = 20
REALNAME_LEN = 8

STATE_UNCONFIGURED = 0
STATE_CONFIGURED = 1
STATE_AIRBORNE = 2

STATE_NAMES = {
    STATE_UNCONFIGURED: "未配置",
    STATE_CONFIGURED: "已配置",
    STATE_AIRBORNE: "空中(写锁定)",
}

# GB 46750-2025 表3-003 / 表3-004
OP_CATEGORY_NAMES = {
    0: "未定义",
    1: "开放类",
    2: "特定类",
    3: "审定类",
}
UA_CLASS_NAMES = {
    0: "微型无人驾驶航空器",
    1: "轻型无人驾驶航空器",
    2: "小型无人驾驶航空器",
    3: "中型无人驾驶航空器",
    4: "大型无人驾驶航空器",
}


def validate_uas_id(s: str) -> bool:
    """20 chars [0-9A-Z], excluding O/I (GB 46860-2025 §4.1)."""
    if len(s) != UAS_ID_LEN:
        return False
    for c in s:
        if not (("0" <= c <= "9") or ("A" <= c <= "Z")):
            return False
        if c in ("O", "I"):
            return False
    return True


def validate_realname(s: str) -> bool:
    """8 digits (UOM 实名登记号后 8 位)."""
    return len(s) == REALNAME_LEN and all("0" <= c <= "9" for c in s)


def validate_op_category(v: int) -> bool:
    """0~3 (GB 46750-2025 表3-003)."""
    return 0 <= v <= 3


def validate_ua_class(v: int) -> bool:
    """0~4 (GB 46750-2025 表3-004)."""
    return 0 <= v <= 4


def encode_uas_id(s: str) -> bytes:
    return s.encode("ascii")


def encode_realname(s: str) -> bytes:
    return s.encode("ascii")


@dataclass
class RidConfig:
    """The four configurable identity fields + current module state."""

    uas_id: str = ""
    realname: str = ""
    op_category: int = -1
    ua_class: int = -1
    state: int = -1

    @property
    def state_name(self) -> str:
        return STATE_NAMES.get(self.state, f"未知({self.state})")

    @property
    def is_airborne(self) -> bool:
        return self.state == STATE_AIRBORNE

    def validate(self) -> list[str]:
        """Return a list of human-readable validation errors (empty = valid)."""
        errs: list[str] = []
        if not validate_uas_id(self.uas_id):
            errs.append("唯一产品识别码：需 20 位 [0-9A-Z] 且不含字母 O/I")
        if not validate_realname(self.realname):
            errs.append("实名登记标志：需 8 位数字")
        if not validate_op_category(self.op_category):
            errs.append("运行类别：需 0~3")
        if not validate_ua_class(self.ua_class):
            errs.append("无人机分类：需 0~4")
        return errs


async def read_config(client) -> RidConfig:
    """Read all five characteristics from an already-connected client."""
    cfg = RidConfig()
    cfg.uas_id = (
        (await client.read_gatt_char(CHAR_UAS_ID))
        .rstrip(b"\x00")
        .decode("ascii", errors="replace")
    )
    cfg.realname = (
        (await client.read_gatt_char(CHAR_REALNAME))
        .rstrip(b"\x00")
        .decode("ascii", errors="replace")
    )
    opc = await client.read_gatt_char(CHAR_OP_CATEGORY)
    cfg.op_category = opc[0] if opc else -1
    uac = await client.read_gatt_char(CHAR_UA_CLASS)
    cfg.ua_class = uac[0] if uac else -1
    st = await client.read_gatt_char(CHAR_STATE)
    cfg.state = st[0] if st else -1
    return cfg


async def write_config(client, cfg: RidConfig) -> None:
    """Write the four identity fields (caller validates first)."""
    await client.write_gatt_char(CHAR_UAS_ID, encode_uas_id(cfg.uas_id), response=True)
    await client.write_gatt_char(CHAR_REALNAME, encode_realname(cfg.realname), response=True)
    await client.write_gatt_char(CHAR_OP_CATEGORY, bytes([cfg.op_category]), response=True)
    await client.write_gatt_char(CHAR_UA_CLASS, bytes([cfg.ua_class]), response=True)
