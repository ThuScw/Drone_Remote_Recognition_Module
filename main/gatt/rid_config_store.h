#ifndef RID_CONFIG_STORE_H
#define RID_CONFIG_STORE_H

#include <stdint.h>
#include <stddef.h>
#include "rid_config.h"

// ======================== RidConfigStore ========================
//
// 运行时配置 + NVS 持久化 + 跨任务互斥。
//
// 写路径 (GATT 写回调, NimBLE host 任务上下文) 与读路径 (广播包构建, 主循环上下文)
// 分属不同 FreeRTOS 任务, 用互斥锁保护。
//
// 状态语义 (无硬门槛, 起飞前未配置则广播占位/上次存储值):
//   UNCONFIGURED — 首次启动, NVS 无记录, 使用 config.h 占位值
//   CONFIGURED   — 至少一次成功 GATT 写入 (或上次会话已配置并持久化)
//   AIRBORNE     — 空中 (广播中, 写锁定)
class RidConfigStore {
public:
    RidConfigStore() = default;
    RidConfigStore(const RidConfigStore&) = delete;
    RidConfigStore& operator=(const RidConfigStore&) = delete;

    // 初始化互斥锁 + 从 NVS 加载。首次启动无记录 → 占位值, 状态 UNCONFIGURED。
    // 需在 nvs_flash_init() 之后调用。
    bool init();

    // 线程安全读取当前配置快照
    void get(RidConfig& out) const;

    // 线程安全读取当前状态
    RidConfigState state() const;

    // 写入单个字段 (校验 + 内存更新 + NVS 持久化)。
    // 返回 false 表示校验失败 (值未变更)。空中状态一律拒绝。
    bool setField(RidConfigField field, const uint8_t* data, uint16_t len);

    // 状态机驱动: 空中广播切换时调用。AIRBORNE 拒绝后续 setField。
    void setState(RidConfigState s);

    // 是否已配置 (状态 != UNCONFIGURED)
    bool isConfigured() const;

private:
    RidConfig      _cfg;
    RidConfigState _state = RID_STATE_UNCONFIGURED;
    void*          _mutex = nullptr;  // SemaphoreHandle_t (避免引入 FreeRTOS 头)
};

#endif // RID_CONFIG_STORE_H
