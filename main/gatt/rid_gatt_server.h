#ifndef RID_GATT_SERVER_H
#define RID_GATT_SERVER_H

#include <stdint.h>
#include "rid_config.h"
#include "rid_config_store.h"

// ======================== RidGattServer ========================
//
// 双向配置 GATT 服务 (16-bit 0xFFF0 → 128-bit 0000FFF0-0000-1000-8000-00805F9B34FB):
//   FFF1 唯一产品识别码 (20 ASCII, write)
//   FFF2 实名登记标志   (8 ASCII,  write)
//   FFF3 运行类别       (1 byte,  write)
//   FFF4 无人机分类     (1 byte,  write)
//   FFF5 状态           (1 byte,  read/notify)
//
// 空中状态 (RID_STATE_AIRBORNE) 拒绝一切写入 (GB 46750-2025 起飞后配置锁定)。
// 写入通过 RidConfigStore 校验 + NVS 持久化。
class RidGattServer {
public:
    RidGattServer() = default;
    RidGattServer(const RidGattServer&) = delete;
    RidGattServer& operator=(const RidGattServer&) = delete;

    // 注册 GATT 服务 (需在 NimBLE host 同步后调用)
    bool init(RidConfigStore& store);

    // 向已连接手机推送当前状态 (通知)。无连接时忽略。
    void notifyState();

private:
    RidConfigStore* _store = nullptr;
    bool _registered = false;
};

#endif // RID_GATT_SERVER_H
