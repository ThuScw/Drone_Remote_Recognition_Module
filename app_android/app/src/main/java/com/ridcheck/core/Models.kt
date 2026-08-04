package com.ridcheck.core

import java.time.Instant
import java.time.ZoneOffset
import java.time.format.DateTimeFormatter

/** GB 46750-2025 表3 数据项：序号、名称、必选性、DecodedPacket.fmt 的取值键。 */
data class GbField(val num: String, val name: String, val optional: Boolean, val fmtKey: String)

/** 表3 全部 21 项数据项（顺序与标准一致，M 必选 / O 可选）。 */
val GB_FIELD_TABLE: List<GbField> = listOf(
    GbField("001", "唯一产品识别码", false, "唯一产品识别码"),
    GbField("002", "实名登记号", false, "实名登记号"),
    GbField("003", "运行类别", false, "运行类别"),
    GbField("004", "无人机分类", false, "无人机分类"),
    GbField("005", "遥控站位置类型", false, "遥控站位置类型"),
    GbField("006", "遥控站位置", false, "遥控站位置"),
    GbField("007", "遥控站高度", false, "遥控站高度_m"),
    GbField("008", "无人机位置", false, "无人机位置"),
    GbField("009", "航迹角", false, "航迹角_deg"),
    GbField("010", "地速", false, "地速_mps"),
    GbField("011", "相对高度", true, "相对高度_m"),
    GbField("012", "垂直速度", true, "垂直速度_mps"),
    GbField("013", "大地高度", false, "大地高度_m"),
    GbField("014", "气压高度", true, "气压高度_m"),
    GbField("015", "运行状态", false, "运行状态"),
    GbField("016", "坐标系", false, "坐标系"),
    GbField("017", "水平精度", false, "水平精度"),
    GbField("018", "垂直精度", false, "垂直精度"),
    GbField("019", "速度精度", false, "速度精度"),
    GbField("020", "时间戳", false, "时间戳"),
    GbField("021", "时间戳精度", false, "时间戳精度")
)

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

/** 单个健康问题。clause = GB 46750-2025 条款/表3字段编号（如 "表3-001"、"5.1.3"）。 */
data class HealthIssue(
    val level: HealthLevel,
    val code: String,
    val message: String,
    val clause: String = ""
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

    /** 表3 序号 → 该字段原始字节（HEX 空格分隔），供解析对照表/报告使用。 */
    val fieldHex: LinkedHashMap<String, String> = LinkedHashMap()

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

/** 一秒一次的历史采样点（用于曲线图与 CSV 导出）。 */
data class SamplePoint(
    val timeMs: Long,
    val rssi: Int,
    val rateHz: Double,
    val level: HealthLevel
)

/** 一帧接收记录：仅保留 GB 包原始字节，导出/报告时按需重解码，避免逐帧存完整解码对象占内存。 */
data class FrameRecord(
    val timeMs: Long,
    val rssi: Int,
    val raw: ByteArray
)

/** 相对轨迹点（米，相对首点/起飞点），由每秒采样追加。relN/relE 由 DeviceRegistry 用等距圆柱投影预计算。 */
data class TrackPoint(
    val timeMs: Long,
    val lat: Double,
    val lon: Double,
    val relN: Double,
    val relE: Double
)

/** 运行状态变化记录：opStatus 变化时追加一条。 */
data class StatusLogEntry(val timeMs: Long, val opStatus: Int)

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
