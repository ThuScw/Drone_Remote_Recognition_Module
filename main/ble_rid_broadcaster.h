#ifndef BLE_RID_BROADCASTER_H
#define BLE_RID_BROADCASTER_H

#include <stdint.h>
#include "rid_messages.h"

// ASTM F3411 / GB 42590-2023 RID Service UUID (16-bit SIG-assigned)
#define RID_SERVICE_UUID 0x0D50

class BleRidBroadcaster {
public:
    bool begin(const char* deviceName);
    bool selfTest();
    bool runtimeCheck();

    // 首次启动广播: configure + set_data + start (重配参数, 仅在状态切换时调用)
    bool startBroadcast(const GB46750Packet& pkt);

    // 原地更新广播数据: 仅 set_data, 不停止广播 (周期性调用)
    bool updateBroadcastData(const GB46750Packet& pkt);

    void stopBroadcast();
    bool isAdvertising() const { return _advertising; }

    // 恢复接口 — BLE 控制器复位后自动检测并恢复
    bool needsRecovery() const;       // 控制器是否发生过复位
    bool attemptRecovery();           // 等待 NimBLE 重新同步, 成功返回 true
    uint8_t getUpdateFailures() const { return _updateFailures; }

private:
    // 构建 BLE5 AD Structure 并写入 os_mbuf, 返回 mbuf 指针 (调用方负责在失败时释放)
    struct os_mbuf* buildAdvData(const GB46750Packet& pkt, uint16_t& outLen);

    uint8_t  _ownAddrType = 0;
    bool     _initialized = false;
    bool     _advertising = false;
    uint8_t  _consecutiveFailures = 0;
    uint8_t  _updateFailures = 0;
};

#endif // BLE_RID_BROADCASTER_H
