#ifndef RID_CONFIG_H
#define RID_CONFIG_H

#include <stdint.h>
#include <stddef.h>

// ======================== 可配置身份字段 (GATT 双向配置) ========================
//
// 手机 APP 通过 GATT 写入的 4 个字段 (001/002/003/004)，起飞前配置、起飞后广播。
// 其余字段 (005~021) 仍从飞控串口读取或硬编码。
//
// 校验规则依据:
//   - 001 唯一产品识别码: GB 46860-2025 §4.1，20 字符，[0-9A-Z] 且禁 O/I
//   - 002 实名登记标志: GB 46750-2025 表3-002，UOM 实名登记号后 8 位数字
//   - 003 运行类别: GB 46750-2025 表3-003，0~3
//   - 004 无人机分类: GB 46750-2025 表3-004，0~4

#define RID_CONFIG_UAS_ID_LEN    20
#define RID_CONFIG_REALNAME_LEN  8

// 配置状态 (GATT STATE 特征值)
enum RidConfigState {
    RID_STATE_UNCONFIGURED = 0,  // 待配置 (仍为占位值)
    RID_STATE_CONFIGURED   = 1,  // 已配置 (地面, 尚未起飞)
    RID_STATE_AIRBORNE     = 2,  // 空中 (广播中, 写锁定)
};

// ======================== GATT 配置服务 UUID ========================
//
// 国标未规定广播式远程识别的 GATT 服务 UUID (GB 46750-2025 §6.1.2 / GB 42590-2023
// §A.1.2.1.1 均未定义 BLE Service UUID), 故采用行业通用写法:
//   16-bit 0xFFF0 → 128-bit 0000FFF0-0000-1000-8000-00805F9B34FB
//   (Bluetooth SIG 基础 UUID 展开, 与广播 Service Data 的 0xFFFF 区分)
#define RID_GATT_SVC_UUID16        0xFFF0

// 特征 UUID (16-bit, 挂载于 0xFFF0 服务下)
#define RID_GATT_CHR_UAS_ID        0xFFF1  // 001 唯一产品识别码 (20 ASCII)
#define RID_GATT_CHR_REALNAME      0xFFF2  // 002 实名登记标志 (8 ASCII)
#define RID_GATT_CHR_OP_CATEGORY   0xFFF3  // 003 运行类别 (1 byte)
#define RID_GATT_CHR_UA_CLASS      0xFFF4  // 004 无人机分类 (1 byte)
#define RID_GATT_CHR_STATE         0xFFF5  // 状态 (1 byte, read/notify)

// 可配置字段枚举 — 供 GATT 写回调与存储层统一分发
enum class RidConfigField : uint8_t {
    UAS_ID      = 0,
    REALNAME_ID = 1,
    OP_CATEGORY = 2,
    UA_CLASS    = 3,
};

// 可配置的 4 个字段，末尾 NUL 便于作为 C 字符串传给 gb46750_buildPacket
struct RidConfig {
    char    uasId[RID_CONFIG_UAS_ID_LEN + 1];
    char    realNameId[RID_CONFIG_REALNAME_LEN + 1];
    uint8_t opCategory;  // 003 运行类别
    uint8_t uaClass;     // 004 无人机分类
};

// 唯一产品识别码: 20 字符 [0-9A-Z] 且不含 O/I
inline bool ridConfigValidateUasId(const char* s, size_t len) {
    if (len != RID_CONFIG_UAS_ID_LEN) return false;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        bool alnum = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z');
        if (!alnum || c == 'O' || c == 'I') return false;
    }
    return true;
}

// 实名登记标志: 8 位数字
inline bool ridConfigValidateRealName(const char* s, size_t len) {
    if (len != RID_CONFIG_REALNAME_LEN) return false;
    for (size_t i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') return false;
    }
    return true;
}

inline bool ridConfigValidateOpCategory(uint8_t v) { return v <= 3; }
inline bool ridConfigValidateUaClass(uint8_t v) { return v <= 4; }

#endif // RID_CONFIG_H
