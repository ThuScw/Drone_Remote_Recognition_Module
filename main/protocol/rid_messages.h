#ifndef RID_MESSAGES_H
#define RID_MESSAGES_H

#include <stdint.h>

// GB 46750-2025 Section 5.2.1 数据包头部常量
#define GB46750_DATA_TYPE   0xFF
#define GB46750_VERSION     0x01  // V1.0

// 最大数据包长度: 1(type)+1(ver)+1(len)+3(id)+71(content) = 77
#define GB46750_MAX_PACKET  128

// 数据标识位映射 (Section 5.2.3 Table 2)
enum DataIdentifierBit {
    // Byte 1
    DID_UPIC          = 0x80,  // 001 唯一产品识别码 (M)
    DID_REALNAME      = 0x40,  // 002 实名登记标志 (M)
    DID_OP_CATEGORY   = 0x20,  // 003 运行类别 (O)
    DID_UA_CLASS      = 0x10,  // 004 无人机分类 (M)
    DID_OP_LOC_TYPE   = 0x08,  // 005 遥控站位置类型 (M)
    DID_OP_LOC        = 0x04,  // 006 遥控站位置 (M)
    DID_OP_ALT        = 0x02,  // 007 遥控站高度 (M)
    DID_EXT_FLAG      = 0x01,

    // Byte 2
    DID_UA_POS        = 0x80,  // 008 无人机位置 (M)
    DID_TRACK_ANGLE   = 0x40,  // 009 航迹角 (M)
    DID_GROUND_SPEED  = 0x20,  // 010 地速 (M)
    DID_REL_HEIGHT    = 0x10,  // 011 相对高度 (O)
    DID_VERT_SPEED    = 0x08,  // 012 垂直速度 (O)
    DID_GEO_ALT       = 0x04,  // 013 大地高度 (M)
    DID_BARO_ALT      = 0x02,  // 014 气压高度 (O)

    // Byte 3
    DID_OP_STATUS     = 0x80,  // 015 运行状态 (M)
    DID_COORD_SYS     = 0x40,  // 016 坐标系类型 (M)
    DID_HORIZ_ACC     = 0x20,  // 017 水平精度 (M)
    DID_VERT_ACC      = 0x10,  // 018 垂直精度 (M)
    DID_SPEED_ACC     = 0x08,  // 019 速度精度 (M)
    DID_TIMESTAMP     = 0x04,  // 020 时间戳 (M)
    DID_TS_ACC        = 0x02,  // 021 时间戳精度 (M)
};

// 运行状态 (Table 3-015)
enum OpStatus {
    STATUS_UNREPORTED  = 0,  // 未报告
    STATUS_GROUND      = 1,  // 地面
    STATUS_AIRBORNE    = 2,  // 空中
    STATUS_EMERGENCY   = 3,  // 紧急状态
    STATUS_FAIL_SAFE   = 4,  // 识别发送功能失效(非紧急)
    STATUS_FAIL_EMERG  = 5,  // 识别发送功能失效(紧急)
};

// GB 46750-2025 数据包结构体
struct GB46750Packet {
    uint8_t  dataType;       // 0xFF
    uint8_t  version;        // 0x01
    uint8_t  dataLength;     // 内容项总字节数
    uint8_t  dataId[8];      // 数据标识 (最多8字节)
    uint8_t  dataIdLen;      // 实际数据标识字节数
    uint8_t  content[128];   // 数据内容项
    uint16_t contentLen;     // 数据内容项实际长度
    uint16_t totalLen;       // 完整数据包长度
};

// 飞行数据字段有效标志位 — 由数据源置位, 编码侧据此动态生成 dataId
enum FlightDataField : uint32_t {
    FLD_POS        = 1 << 0,   // lat, lon
    FLD_GEO_ALT    = 1 << 1,   // 大地高度
    FLD_BARO_ALT   = 1 << 2,   // 气压高度
    FLD_HEIGHT_AGL = 1 << 3,   // 相对高度 (O)
    FLD_SPEED      = 1 << 4,   // 地速
    FLD_HEADING    = 1 << 5,   // 航迹角
    FLD_VSPEED     = 1 << 6,   // 垂直速度 (O)
    FLD_OP_STATUS  = 1 << 7,   // 运行状态
    FLD_OP_POS     = 1 << 8,   // 遥控站位置
    FLD_OP_ALT     = 1 << 9,   // 遥控站高度

    FLD_ALL_M      = FLD_POS | FLD_GEO_ALT | FLD_SPEED | FLD_HEADING
                   | FLD_OP_STATUS | FLD_OP_POS | FLD_OP_ALT,
    FLD_ALL        = FLD_ALL_M | FLD_BARO_ALT | FLD_HEIGHT_AGL | FLD_VSPEED,
};

// 数据新鲜度等级 (GB 46750-2025 实时性要求)
enum DataFreshness {
    FRESH_OK       = 0,   // 数据新鲜 (< DATA_FRESH_THRESHOLD_MS)
    FRESH_STALE    = 1,   // 数据过期 (≥ DATA_FRESH_THRESHOLD_MS)
    FRESH_INVALID  = 2,   // 数据无效 (范围检查失败)
};

// 飞行数据输入结构体 — 带时间戳和新鲜度追踪
struct FlightData {
    float lat, lon;          // 纬度/经度 (度)
    float geoAlt;            // 大地高度 (m)
    float baroAlt;           // 气压高度 (m)
    float heightAgl;         // 相对高度 (m)
    float speed;             // 地速 (m/s)
    float heading;           // 航迹角 (度, 0-359)
    float vspeed;            // 垂直速度 (m/s, 正=上升)
    uint8_t opStatus;        // 运行状态
    float opLat, opLon;      // 操作员/遥控站位置
    float opAlt;             // 操作员高度 (m)

    float horizAccM;         // GPS 水平精度 (m, from eph)
    float vertAccM;          // GPS 垂直精度 (m, from epv)

    uint32_t validMask;      // FlightDataField 按位或, 标记哪些字段本周期有效

    // 数据新鲜度追踪 (每个字段的时间戳)
    uint64_t ts_pos;         // lat/lon 更新时间
    uint64_t ts_geoAlt;      // geoAlt 更新时间
    uint64_t ts_speed;       // speed 更新时间
    uint64_t ts_heading;     // heading 更新时间
    uint64_t ts_opStatus;    // opStatus 更新时间
    uint64_t ts_opPos;       // opLat/opLon 更新时间

    // 数据质量指标
    DataFreshness freshness; // 整体新鲜度 (取最差值)
    uint32_t validationFlags; // 范围验证结果 (每位置位表示无效)

    uint64_t unixTimestampMs; // Unix 纪元毫秒 (0 = 尚未授时)
};

// --- API ---

// 构建 GB 46750-2025 完整数据包
// timestampMs: Unix 毫秒时间戳
void gb46750_buildPacket(GB46750Packet& pkt, const FlightData& fd,
                          const char* uasId, const char* realNameId,
                          uint8_t opCategory, uint8_t uaClass,
                          uint8_t opLocType, uint8_t coordSys,
                          uint8_t horizAcc, uint8_t vertAcc,
                          uint8_t speedAcc, uint8_t tsAcc,
                          uint64_t timestampMs);

// 将数据包序列化为字节数组，返回实际长度
uint16_t gb46750_serialize(const GB46750Packet& pkt, uint8_t* out, uint16_t maxLen);

// 发送前结构自检: 验证 totalLen/datalen 自洽, dataType/version 合规, dataId 与 contentLen 一致
// 返回 true 表示数据包结构正确可发送
bool gb46750_packetVerify(const GB46750Packet& pkt);

// --- 数据验证 API ---

// 验证飞行数据范围 (GPS、高度、速度等物理边界)
// 返回 true 表示所有 M 字段范围有效
// validationFlags 输出: 每位置位表示对应字段无效
bool gb46750_validateFlightData(const FlightData& fd, uint32_t& validationFlags);

// 检查数据新鲜度 (基于时间戳)
// 返回整体新鲜度等级 (取最差值)
DataFreshness gb46750_checkFreshness(const FlightData& fd, uint64_t nowMs, uint64_t thresholdMs);

// GPS 精度 (m) → GB 46750-2025 精度枚举值。返回 0 = unknown
uint8_t gb46750_mapHorizAcc(float ephM);
uint8_t gb46750_mapVertAcc(float epvM);

#endif // RID_MESSAGES_H
