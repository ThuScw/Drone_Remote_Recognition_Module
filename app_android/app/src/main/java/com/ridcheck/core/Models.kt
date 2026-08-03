package com.ridcheck.core

import java.time.Instant
import java.time.ZoneOffset
import java.time.format.DateTimeFormatter

/** 整体健康判定等级，与 PC 版 models.py 的 HealthLevel IntEnum 对应。 */
enum class HealthLevel {
    PASS, WARN, FAIL;

    fun label(): String = when (this) {
        PASS -> "通过"
        WARN -> "警告"
        FAIL -> "故障"
    }

    fun verdictLabel(): String = when (this) {
        PASS -> "正常"
        WARN -> "警告"
        FAIL -> "故障"
    }
}

/** 单个健康问题。 */
data class HealthIssue(
    val level: HealthLevel,
    val code: String,
    val message: String
) {
    val label: String get() = level.label()
}

/**
 * 解码后的 GB 46750-2025 数据包 + 接收元信息。
 * 与 PC 版 models.py 的 DecodedPacket 一致，解析时逐字段填充。
 */
class DecodedPacket {
    // --- 接收元信息 ---
    var address: String = ""
    var rssi: Int = 0
    var receivedAtMs: Long = 0
    var source: String = "ble"

    // --- 原始 ---
    var raw: ByteArray = ByteArray(0)
    var dataType: Int = 0          // 0xFF
    var version: Int = 0           // 0x20 = V1.0
    var declaredLen: Int = 0       // dataLength
    var dataId: ByteArray = ByteArray(0)
    var contentLen: Int = 0

    // --- GB 46750 字段 ---
    var uasId: String = ""
    var realname: String = ""
    var opCategory: Int = -1
    var uaClass: Int = -1
    var opLocType: Int = -1
    var opLat: Double = Double.NaN
    var opLon: Double = Double.NaN
    var opAlt: Double = Double.NaN
    var uaLat: Double = Double.NaN
    var uaLon: Double = Double.NaN
    var heading: Double = Double.NaN
    var speed: Double = Double.NaN
    var relHeight: Double = Double.NaN
    var vspeed: Double = Double.NaN
    var geoAlt: Double = Double.NaN
    var baroAlt: Double = Double.NaN
    var opStatus: Int = -1
    var coordSys: Int = -1
    var horizAcc: Int = -1
    var vertAcc: Int = -1
    var speedAcc: Int = -1
    var timestampMs: Long = 0
    var tsAcc: Int = -1

    // --- 原始可用性标志（供 UI） ---
    var hasRelHeight: Boolean = false
    var hasVspeed: Boolean = false
    var hasBaroAlt: Boolean = false
    var structureError: String = ""

    // --- 人类可读格式化（保持插入顺序） ---
    val fmt: LinkedHashMap<String, String> = LinkedHashMap()

    val timestampUtc: String
        get() {
            if (timestampMs <= 0) return "未授时"
            return try {
                val s = DateTimeFormatter.ofPattern("yyyy-MM-dd HH:mm:ss.SSS")
                    .withZone(ZoneOffset.UTC)
                    .format(Instant.ofEpochMilli(timestampMs))
                "$s UTC"
            } catch (e: Exception) {
                "无效($timestampMs)"
            }
        }
}

/** 一次流式判定结果。 */
class HealthReport {
    var level: HealthLevel = HealthLevel.PASS
    val issues: MutableList<HealthIssue> = ArrayList()
    var packetsSeen: Int = 0
    var packetsOk: Int = 0
    var avgRateHz: Double = 0.0
    var staleSeconds: Double = 0.0
    var note: String = ""

    val verdictLabel: String get() = level.verdictLabel()

    fun worstIssue(): HealthIssue? = issues.firstOrNull()
}
