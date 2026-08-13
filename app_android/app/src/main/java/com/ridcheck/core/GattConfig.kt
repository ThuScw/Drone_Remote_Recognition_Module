package com.ridcheck.core

/**
 * GATT 双向配置常量与校验，逐行移植自 app/rid/gatt_config.py，
 * 与固件 main/gatt/rid_config.h 保持一致的校验规则。
 *
 * 固件暴露私有 GATT 服务 (16-bit 0xFFF0 → 128-bit 0000FFF0-0000-1000-8000-00805F9B34FB)：
 *   FFF1 唯一产品识别码 (20 ASCII)  write/read
 *   FFF2 实名登记标志   (8 ASCII)    write/read
 *   FFF3 运行类别       (1 byte)     write/read
 *   FFF4 无人机分类     (1 byte)     write/read
 *   FFF5 状态          (1 byte)      read/notify
 * 模块仅在地面态 (state != 2) 接受写入。
 */
object GattConfig {
    const val SERVICE_UUID_128 = "0000fff0-0000-1000-8000-00805f9b34fb"
    const val CHAR_UAS_ID = "0000fff1-0000-1000-8000-00805f9b34fb"
    const val CHAR_REALNAME = "0000fff2-0000-1000-8000-00805f9b34fb"
    const val CHAR_OP_CATEGORY = "0000fff3-0000-1000-8000-00805f9b34fb"
    const val CHAR_UA_CLASS = "0000fff4-0000-1000-8000-00805f9b34fb"
    const val CHAR_STATE = "0000fff5-0000-1000-8000-00805f9b34fb"

    const val UAS_ID_LEN = 20
    const val REALNAME_LEN = 8

    const val STATE_UNCONFIGURED = 0
    const val STATE_CONFIGURED = 1
    const val STATE_AIRBORNE = 2

    val STATE_NAMES = mapOf(
        STATE_UNCONFIGURED to "未配置",
        STATE_CONFIGURED to "已配置",
        STATE_AIRBORNE to "空中(写锁定)"
    )

    // GB 46750-2025 表3-003 / 表3-004
    val OP_CATEGORY_NAMES = mapOf(
        0 to "未定义", 1 to "开放类", 2 to "特定类", 3 to "审定类"
    )
    val UA_CLASS_NAMES = mapOf(
        0 to "微型无人驾驶航空器",
        1 to "轻型无人驾驶航空器",
        2 to "小型无人驾驶航空器",
        3 to "中型无人驾驶航空器",
        4 to "大型无人驾驶航空器"
    )

    /** 20 字符 [0-9A-Z]，不含 O/I（GB 46860-2025 §4.1）。 */
    fun validateUasId(s: String): Boolean {
        if (s.length != UAS_ID_LEN) return false
        for (c in s) {
            val alnum = (c in '0'..'9') || (c in 'A'..'Z')
            if (!alnum || c == 'O' || c == 'I') return false
        }
        return true
    }

    /** 8 位数字（UOM 实名登记号后 8 位）。 */
    fun validateRealname(s: String): Boolean =
        s.length == REALNAME_LEN && s.all { it in '0'..'9' }

    /** 0~3（GB 46750-2025 表3-003）。 */
    fun validateOpCategory(v: Int): Boolean = v in 0..3

    /** 0~4（GB 46750-2025 表3-004）。 */
    fun validateUaClass(v: Int): Boolean = v in 0..4
}

/** 四个可配置身份字段 + 当前模块状态。 */
data class RidConfigData(
    val uasId: String = "",
    val realname: String = "",
    val opCategory: Int = -1,
    val uaClass: Int = -1,
    val state: Int = -1
) {
    val stateName: String get() = GattConfig.STATE_NAMES[state] ?: "未知($state)"

    val isAirborne: Boolean get() = state == GattConfig.STATE_AIRBORNE

    /** 返回人类可读的校验错误列表（空 = 合法）。 */
    fun validate(): List<String> {
        val errs = ArrayList<String>()
        if (!GattConfig.validateUasId(uasId))
            errs.add("唯一产品识别码：需 20 位 [0-9A-Z] 且不含字母 O/I")
        if (!GattConfig.validateRealname(realname))
            errs.add("实名登记标志：需 8 位数字")
        if (!GattConfig.validateOpCategory(opCategory))
            errs.add("运行类别：需 0~3")
        if (!GattConfig.validateUaClass(uaClass))
            errs.add("无人机分类：需 0~4")
        return errs
    }
}
