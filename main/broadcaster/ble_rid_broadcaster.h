#ifndef BLE_RID_BROADCASTER_H
#define BLE_RID_BROADCASTER_H

#include <stdint.h>
#include "rid_messages.h"

// RID BLE Service UUID (16-bit)
// GB 46750-2025 未定义 BLE Service Data 的帧级封装; 采用中性未分配 UUID 0xFFFF
// 承载纯 GB 数据包 (不带 ASTM/OpenDroneID 的 0xFFFA/0x0D 字头)
#define RID_SERVICE_UUID 0xFFFF

class BleRidBroadcaster {
public:
    BleRidBroadcaster() = default;
    BleRidBroadcaster(const BleRidBroadcaster&) = delete;
    BleRidBroadcaster& operator=(const BleRidBroadcaster&) = delete;

    bool begin(const char* deviceName);
    bool selfTest();
    bool runtimeCheck();

    // 首次启动广播: configure + set_data + start (空中模式, 不可连接, 重配参数仅在状态切换时调用)
    bool startBroadcast(const GB46750Packet& pkt);

    // 启动可连接广播 (地面模式): 不携带 GB 数据, 附带 GATT 服务 UUID 等待手机配置
    bool startConnectableAdv();

    // 原地更新广播数据: 仅 set_data, 不停止广播 (周期性调用)
    bool updateBroadcastData(const GB46750Packet& pkt);

    void stopBroadcast();
    bool isAdvertising() const { return _advertising; }
    bool isConnected() const { return _connected; }
    bool isConnectableMode() const { return _connectableMode; }

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

    // 构建可连接广播的 AD Structure (Flags + Name + 16-bit GATT 服务 UUID)
    struct os_mbuf* buildConnectableAdvData(uint16_t& outLen);

    // GAP 事件回调 (连接/断开) — 供 ble_gap_ext_adv_configure 注册
    static int onGapEvent(struct ble_gap_event* event, void* arg);

    char     _deviceName[32] = {};
    uint8_t  _ownAddrType = 0;
    bool     _initialized = false;
    bool     _advertising = false;
    bool     _connectableMode = false;  // 地面可连接模式
    bool     _connected = false;        // 是否有活跃 GATT 连接
    uint16_t _connHandle = 0xFFFF;      // 活跃连接句柄 (0xFFFF = 无)
    uint8_t  _consecutiveFailures = 0;
    uint8_t  _updateFailures = 0;
    bool     _degraded = false;
    bool     _useAltPhy = false;
};

#endif // BLE_RID_BROADCASTER_H
