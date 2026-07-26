#ifndef BLE_RID_BROADCASTER_H
#define BLE_RID_BROADCASTER_H

#include <stdint.h>
#include "rid_messages.h"

// ASTM F3411 / GB 42590-2023 RID Service UUID (16-bit SIG-assigned)
#define RID_SERVICE_UUID 0x0D50

class BleRidBroadcaster {
public:
    BleRidBroadcaster() = default;
    BleRidBroadcaster(const BleRidBroadcaster&) = delete;
    BleRidBroadcaster& operator=(const BleRidBroadcaster&) = delete;

    bool begin(const char* deviceName);
    bool selfTest();
    bool runtimeCheck();

    // 首次启动广播: configure + set_data + start (重配参数, 仅在状态切换时调用)
    bool startBroadcast(const GB46750Packet& pkt);

    // 原地更新广播数据: 仅 set_data, 不停止广播 (周期性调用)
    bool updateBroadcastData(const GB46750Packet& pkt);

    void stopBroadcast();
    bool isAdvertising() const { return _advertising; }

    // 自修复结果
    enum class RecoveryResult {
        RECOVERED,  // 完全恢复
        DEGRADED,   // 降级恢复 (备用参数)
        FAILED      // 三级全部失败
    };

    // 三级递进自修复 — 广播异常时调用，从轻到重尝试恢复
    RecoveryResult attemptSelfHeal(const GB46750Packet& pkt);

    // 控制器复位检测 (供主循环轮询)
    bool needsRecovery() const;

    uint8_t getUpdateFailures() const { return _updateFailures; }
    bool isDegraded() const { return _degraded; }

private:
    // 构建 BLE5 AD Structure 并写入 os_mbuf, 返回 mbuf 指针
    struct os_mbuf* buildAdvData(const GB46750Packet& pkt, uint16_t& outLen);

    // NimBLE 完整重初始化 (Tier 3)
    bool reinitNimble();

    char     _deviceName[32] = {};
    uint8_t  _ownAddrType = 0;
    bool     _initialized = false;
    bool     _advertising = false;
    uint8_t  _consecutiveFailures = 0;
    uint8_t  _updateFailures = 0;
    bool     _degraded = false;
    bool     _useAltPhy = false;
};

#endif // BLE_RID_BROADCASTER_H
