#ifndef MAVLINK_PARSER_H
#define MAVLINK_PARSER_H

#include <stdint.h>
#include "flight_data.h"

// ================= MAVLink v2 帧结构 =================

#define MAVLINK_V2_MAGIC          0xFD
#define MAVLINK_V2_HEADER_LEN     10   // STX+LEN+INCOMPAT+COMPAT+SEQ+SYSID+COMPID+MSGID(3)
#define MAVLINK_V2_CRC_LEN        2
#define MAVLINK_V2_MAX_PAYLOAD    253
#define MAVLINK_V2_SIG_LEN        13   // 签名帧: link_id(1) + timestamp(6) + signature(6)
#define MAVLINK_IFLAG_SIGNED      0x01 // INCOMPAT 标志位 0: 帧已签名

#define MAVLINK_V1_MAGIC          0xFE
#define MAVLINK_V1_HEADER_LEN     6    // STX+LEN+SEQ+SYSID+COMPID+MSGID(1)
#define MAVLINK_V1_CRC_LEN        2
#define MAVLINK_V1_MAX_PAYLOAD    255

// 常用消息ID (ArduPilot common.xml)
#define MAVLINK_MSG_HEARTBEAT              0
#define MAVLINK_MSG_GPS_RAW_INT           24
#define MAVLINK_MSG_ATTITUDE              30
#define MAVLINK_MSG_GLOBAL_POSITION_INT   33
#define MAVLINK_MSG_VFR_HUD               74
#define MAVLINK_MSG_SYSTEM_TIME            2
#define MAVLINK_MSG_HIGHRES_IMU          105
#define MAVLINK_MSG_HOME_POSITION        242

// ================= 解析状态 =================

enum MavlinkParseState {
    PARSE_STATE_IDLE,      // 等待 STX (0xFD v2 / 0xFE v1)
    PARSE_STATE_HEADER,    // 接收帧头 (10 bytes v2 / 6 bytes v1)
    PARSE_STATE_PAYLOAD,   // 接收 payload
    PARSE_STATE_CRC,       // 接收 CRC (2 bytes)
};

// ================= 解析上下文 =================

struct MavlinkParser {
    MavlinkParseState state;
    uint8_t  buffer[MAVLINK_V2_HEADER_LEN + MAVLINK_V2_MAX_PAYLOAD
                    + MAVLINK_V2_CRC_LEN + MAVLINK_V2_SIG_LEN];
    uint16_t bufferIdx;
    uint16_t expectedLen;  // 预期帧总长度 (含 CRC, 签名帧含签名)
    uint16_t payloadLen;   // payload 长度

    bool isV2;      // 当前帧是 MAVLink v2 (true) 还是 v1 (false)
    bool isSigned;  // v2 签名帧 (incompat_flags bit0), CRC 后追加 13 字节

    // 统计
    uint32_t totalFrames;
    uint32_t totalV1Frames;
    uint32_t totalV2Frames;
    uint32_t crcErrors;
    uint32_t parseErrors;
    uint32_t consecutiveCrcErrors;  // 连续 CRC 失败计数，成功帧归零
    uint64_t lastValidFrameMs;      // 最后一次成功解析帧的时间戳

    // 最新消息时间戳 (ms)
    uint64_t lastHeartbeatMs;
    uint64_t lastPositionMs;
    uint64_t lastGpsMs;

    // 系统状态
    bool armed;            // 从 HEARTBEAT 提取
    uint8_t systemStatus;  // 0=UNINIT, 3=STANDBY, 4=ACTIVE, 5=CRITICAL, 6=EMERGENCY

    // 位置数据
    float lat;             // 纬度 (度)
    float lon;             // 经度 (度)
    float altMsl;          // 海拔高度 (m MSL)
    float altRel;          // 相对高度 (m AGL)
    float velN;            // 北向速度 (m/s)
    float velE;            // 东向速度 (m/s)
    float velD;            // 地向速度 (m/s, 正=向下)
    float heading;         // 航向角 (度, 0-360)

    // GPS 数据
    uint8_t  gpsFixType;   // 0=NO_GPS, 2=2D, 3=3D
    uint8_t  gpsSats;      // 可见卫星数
    float    gpsEph;       // 水平精度因子
    float    gpsEpv;       // 垂直精度因子

    // VFR_HUD 数据
    float groundspeed;     // 地速 (m/s)
    float climbRate;       // 爬升率 (m/s, 正=上升)

    // HOME 位置
    float homeLat;
    float homeLon;
    float homeAlt;
    bool  homeValid;

    // Takeoff point — recorded when first ARMED heartbeat received
    // Used as operator position fallback when HOME_POSITION unavailable
    float takeoffLat;
    float takeoffLon;
    float takeoffAlt;
    bool  takeoffValid;

    // Unix time (from SYSTEM_TIME)
    int64_t  unixBootOffsetMs;    // Unix epoch ms when FC booted
    bool     unixTimeValid;
    uint64_t lastSystemTimeMs;    // ESP32 uptime of last SYSTEM_TIME message
    uint32_t lastPositionBootMs;  // FC time_boot_ms from latest GLOBAL_POSITION_INT
};

// ================= API =================

// 初始化解析器
void mavlink_init(MavlinkParser& parser);

// 输入单字节, 返回 true 表示成功解析一帧
bool mavlink_parseByte(MavlinkParser& parser, uint8_t byte, uint64_t nowMs);

// 将解析结果填充到 FlightData
// 返回 true 表示数据有效 (有位置+有GPS+armed)
bool mavlink_fillFlightData(const MavlinkParser& parser, FlightData& fd, uint64_t nowMs);

// 获取解析器状态摘要 (用于调试)
void mavlink_getStatus(const MavlinkParser& parser, char* buf, uint16_t bufLen);

// 检查数据是否超时
bool mavlink_isDataStale(const MavlinkParser& parser, uint64_t nowMs, uint64_t timeoutMs);

// 检查是否需要触发 USB 恢复 (连续 CRC 失败超阈值)
// 返回 true 表示数据流可能已损坏，应关闭并重新打开 USB 设备
bool mavlink_needsRecovery(const MavlinkParser& parser, uint64_t nowMs, uint32_t errorLimit);

#endif // MAVLINK_PARSER_H
