// ======================== RIDBroadcastManager 实现 ========================
//
// 所有广播编排、安全校验、BLE 恢复、飞行日志逻辑集中于此。
// 主循环不直接操作广播状态，只调用 update()。

#include <esp_log.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include "broadcast_manager.h"
#include "config.h"

static const char* TAG = "BCAST";

// ======================== 构造 / 初始化 ========================

RIDBroadcastManager::RIDBroadcastManager(
    BleRidBroadcaster& broadcaster,
    FlightLog& flightLog,
    StatusLed& statusLed,
    RIDInterlock& interlock)
    : _broadcaster(broadcaster)
    , _flightLog(flightLog)
    , _statusLed(statusLed)
    , _interlock(interlock)
    , _broadcastActive(false)
    , _prevStatus(0xFF)
    , _lastBroadcastMs(0)
    , _lastDataUpdateMs(0)
    , _lastSelfTestMs(0)
    , _lastFlightLogMs(0)
    , _lastHeapCheckMs(0)
    , _broadcastCount(0)
    , _validationFailCount(0)
{
    memset(&_currentPacket, 0, sizeof(_currentPacket));
    memset(&_lastValidData, 0, sizeof(_lastValidData));
}

bool RIDBroadcastManager::init() {
    // 1. 配置校验
    // UAS ID: 20 字符 ASCII [0-9A-HJ-NP-Z] (GB 46860-2025)
    if (strlen(UAS_ID) != 20) {
        ESP_LOGE(TAG, "UAS_ID must be 20 chars (current: %d)", (int)strlen(UAS_ID));
        return false;
    }
    for (int i = 0; i < 20; i++) {
        char c = UAS_ID[i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z'))) {
            ESP_LOGE(TAG, "UAS_ID[%d]='%c' — only [0-9A-Z] allowed", i, c);
            return false;
        }
        if (c == 'O' || c == 'I') {
            ESP_LOGE(TAG, "UAS_ID[%d]='%c' — O/I prohibited by GB 46860", i, c);
            return false;
        }
    }
    if (strlen(REALNAME_ID) != 8) {
        ESP_LOGE(TAG, "REALNAME_ID must be 8 chars (current: %d)", (int)strlen(REALNAME_ID));
        return false;
    }
    if (BROADCAST_INTERVAL_MS > 1000) {
        ESP_LOGE(TAG, "BROADCAST_INTERVAL_MS must <= 1000 (GB 46750 5.1.3)");
        return false;
    }
    if (BLE_ADV_INTERVAL_MS > BROADCAST_INTERVAL_MS) {
        ESP_LOGE(TAG, "BLE_ADV_INTERVAL_MS (%d) must <= BROADCAST_INTERVAL_MS (%d)",
                 BLE_ADV_INTERVAL_MS, BROADCAST_INTERVAL_MS);
        return false;
    }

    // 2. BLE 自检
    if (!_broadcaster.selfTest()) {
        ESP_LOGE(TAG, "BLE self-test failed");
        return false;
    }

    // 3. 联锁就绪 — 模块自检通过，允许飞控起飞 (GB 46750-2025 5.1.7)
    _interlock.arm();
    _statusLed.setState(LedState::STANDBY);

    uint64_t nowMs = (uint64_t)(esp_timer_get_time() / 1000);
    _lastDataUpdateMs = nowMs;
    _lastBroadcastMs  = nowMs;
    _lastSelfTestMs   = nowMs;
    _lastHeapCheckMs  = nowMs;

    ESP_LOGI(TAG, "Init OK — Packet=%d bytes, broadcast=%dms, update=%dms",
             _currentPacket.totalLen, BROADCAST_INTERVAL_MS, DATA_UPDATE_INTERVAL_MS);
    return true;
}

// ======================== 主更新循环 ========================

void RIDBroadcastManager::update(const FlightData& fd, uint64_t nowMs) {
    // 1. BLE 控制器恢复 (检测 needsRecovery)
    handleBleRecovery();

    // 2. 数据更新: 验证 + 构建新包
    if (nowMs - _lastDataUpdateMs >= DATA_UPDATE_INTERVAL_MS) {
        _lastDataUpdateMs = nowMs;
        validateAndBuildPacket(fd, nowMs);
    }

    // 3. 状态转换 (地面↔空中↔紧急)
    handleStatusTransition();

    // 4. 广播发送
    handleBroadcast(nowMs);

    // 5. 飞行日志 (GB 46750-2025 5.1.8, 每 10s)
    handleFlightLog(nowMs);

    // 6. 运行时自检 (GB 42590-2023 A.2.3.5.5, 每 5s)
    handleSelfTest();

    // 7. 堆内存监控 (P1, 每 60s)
    handleHeapMonitor(nowMs);
}

// ======================== 数据验证与包构建 ========================
//
// P0 合规修复:
//   - 始终构建新包，不保留旧包
//   - 范围验证失败的字段: 仍编码为 0 (unknown)，但不阻止包构建
//   - M 字段缺失 (validMask 未置位): 编码为 unknown (0)，dataId 位仍为 1
//   - 这确保 Message Counter 每包自增，接收方始终看到最新状态

void RIDBroadcastManager::validateAndBuildPacket(const FlightData& fd, uint64_t nowMs) {
    // 保存最新数据供广播使用
    _lastValidData = fd;

    // 范围验证 (物理边界)
    uint32_t validationFlags = 0;
    bool rangeValid = gb46750_validateFlightData(fd, validationFlags);

    if (!rangeValid) {
        _validationFailCount++;
        ESP_LOGW(TAG, "Data range validation failed (flags=0x%08lx, count=%lu), "
                 "building with unknown values for invalid fields",
                 (unsigned long)validationFlags, (unsigned long)_validationFailCount);
        // 不阻止包构建 — 无效字段在 buildPacket 中编码为 0
    }

    // 始终构建新包 (P0: 不再 "keeping previous packet")
    gb46750_buildPacket(_currentPacket, fd, UAS_ID, REALNAME_ID,
                        OP_CATEGORY, UA_CLASS, OP_LOCATION_TYPE, COORD_SYS,
                        HORIZ_ACC, VERT_ACC, SPEED_ACC, TS_ACC, nowMs);
}

// ======================== BLE 自修复 ========================
//
// P1 合规修复: 三处重复代码合并为单一方法
// 触发源: needsRecovery() 标志 / updateFailures >= 3 / 自检失败

void RIDBroadcastManager::triggerSelfHeal() {
    auto result = _broadcaster.attemptSelfHeal(_currentPacket);

    if (result != BleRidBroadcaster::RecoveryResult::FAILED) {
        const char* mode = (result == BleRidBroadcaster::RecoveryResult::RECOVERED)
                           ? "recovered" : "degraded";
        ESP_LOGI(TAG, "Self-heal OK (%s)", mode);

        if (isAirborne()) {
            _broadcastActive = true;
            _statusLed.setState(LedState::BROADCASTING);
        } else {
            _broadcastActive = false;
            _broadcaster.stopBroadcast();
            _statusLed.setState(LedState::STANDBY);
            ESP_LOGI(TAG, "On ground — broadcast stopped, interlock ready");
        }
    } else {
        ESP_LOGE(TAG, "Self-heal FAILED — all 3 tiers exhausted");
        _statusLed.setState(LedState::FAULT);
        _broadcastActive = false;

        if (isAirborne()) {
            // 空中: 只告警不拉闸，飞控继续自主飞行 (GB 46750-2025 5.1.7b)
            ESP_LOGW(TAG, "AIRBORNE: keeping interlock armed — drone flies on");
        } else {
            // 地面: 拉闸禁止起飞 (GB 46750-2025 5.1.7a)
            if (_interlock.isArmed()) {
                _interlock.disarm();
                ESP_LOGW(TAG, "GROUND: interlock DISARMED — takeoff blocked");
            }
        }
    }
}

void RIDBroadcastManager::handleBleRecovery() {
    // 触发源 1: BLE 控制器复位回调
    if (_broadcaster.needsRecovery()) {
        ESP_LOGE(TAG, "BLE controller reset detected — starting self-heal");
        triggerSelfHeal();
        return;
    }

    // 触发源 2: 广播数据更新连续失败
    if (_broadcaster.getUpdateFailures() >= 3) {
        ESP_LOGE(TAG, "%d consecutive update failures — triggering self-heal",
                 _broadcaster.getUpdateFailures());
        triggerSelfHeal();
    }
}

// ======================== 状态转换 ========================

bool RIDBroadcastManager::isAirborne() const {
    uint8_t s = _lastValidData.opStatus;
    return (s == STATUS_AIRBORNE || s == STATUS_EMERGENCY ||
            s == STATUS_FAIL_SAFE || s == STATUS_FAIL_EMERG);
}

void RIDBroadcastManager::handleStatusTransition() {
    uint8_t newStatus = _lastValidData.opStatus;
    if (newStatus == _prevStatus) return;

    bool shouldBroadcast = isAirborne();

    if (shouldBroadcast && !_broadcastActive) {
        ESP_LOGI(TAG, "Broadcast START (status=%d)", newStatus);
        if (_broadcaster.startBroadcast(_currentPacket)) {
            _broadcastActive = true;
            _statusLed.setState(LedState::BROADCASTING);
        } else {
            ESP_LOGE(TAG, "Failed to start broadcast");
        }
    } else if (!shouldBroadcast && _broadcastActive) {
        ESP_LOGI(TAG, "Broadcast STOP (status=%d, ground)", newStatus);
        _broadcaster.stopBroadcast();
        _broadcastActive = false;
        _statusLed.setState(LedState::STANDBY);
    }

    _prevStatus = newStatus;
}

// ======================== 广播发送 ========================
//
// P0 合规修复:
//   - 永不跳过广播 (删除 goto skip_broadcast)
//   - 数据过期/缺失时仍广播，附加日志告警
//   - 满足 GB 46750-2025 5.1.2 "全过程自动持续发送"

void RIDBroadcastManager::handleBroadcast(uint64_t nowMs) {
    if (!_broadcastActive) return;
    if (nowMs - _lastBroadcastMs < BROADCAST_INTERVAL_MS) return;

    _lastBroadcastMs = nowMs;
    _broadcastCount++;

    // 新鲜度检查 — 仅用于日志告警，不阻止广播
    DataFreshness freshness = gb46750_checkFreshness(
        _lastValidData, nowMs, DATA_FRESH_THRESHOLD_MS);

    if (freshness == FRESH_STALE) {
        ESP_LOGW(TAG, "Data STALE (> %d ms), broadcasting anyway",
                 DATA_FRESH_THRESHOLD_MS);
    } else if (freshness == FRESH_INVALID) {
        ESP_LOGW(TAG, "Data INVALID (M-fields missing), broadcasting with unknown values");
    }

    // 序列化并发送
    uint8_t serialized[GB46750_MAX_PACKET];
    uint16_t len = gb46750_serialize(_currentPacket, serialized, sizeof(serialized));
    if (len == 0) {
        ESP_LOGE(TAG, "Serialize failed — packet too large");
        return;
    }

    if (!_broadcaster.updateBroadcastData(_currentPacket)) {
        ESP_LOGW(TAG, "Broadcast data update failed (count=%d)",
                 _broadcaster.getUpdateFailures());
        // handleBleRecovery() 会在下次 update() 中处理连续失败
    }
}

// ======================== 飞行日志 ========================
// GB 46750-2025 5.1.8: 滚动存储，间隔 ≤10s

void RIDBroadcastManager::handleFlightLog(uint64_t nowMs) {
    if (nowMs - _lastFlightLogMs < (uint64_t)(FLIGHT_LOG_INTERVAL_S * 1000)) return;
    _lastFlightLogMs = nowMs;

    uint8_t serialized[GB46750_MAX_PACKET];
    uint16_t len = gb46750_serialize(_currentPacket, serialized, sizeof(serialized));
    if (len > 0) {
        _flightLog.enqueueRecord(serialized, len, nowMs);
    }
}

// ======================== 运行时自检 ========================
// GB 42590-2023 A.2.3.5.5: 全飞行周期持续监测

void RIDBroadcastManager::handleSelfTest() {
    if (_lastSelfTestMs == 0) return;  // 尚未初始化完成
    if ((uint64_t)(esp_timer_get_time() / 1000) - _lastSelfTestMs < SELF_TEST_INTERVAL_MS) return;
    _lastSelfTestMs = (uint64_t)(esp_timer_get_time() / 1000);

    bool healthy = _broadcaster.runtimeCheck();
    if (!healthy) {
        ESP_LOGW(TAG, "Runtime self-test FAIL — triggering self-heal");
        triggerSelfHeal();
    }
}

// ======================== 堆内存监控 (P1) ========================
// 每 60s 检查最小可用堆，低于 10KB 输出告警

void RIDBroadcastManager::handleHeapMonitor(uint64_t nowMs) {
    if (nowMs - _lastHeapCheckMs < 60000) return;
    _lastHeapCheckMs = nowMs;

    size_t freeHeap = esp_get_minimum_free_heap_size();
    ESP_LOGI(TAG, "Heap monitor: min_free=%u bytes, broadcast_count=%lu",
             (unsigned)freeHeap, (unsigned long)_broadcastCount);

    if (freeHeap < 10000) {
        ESP_LOGW(TAG, "LOW HEAP WARNING: only %u bytes remaining", (unsigned)freeHeap);
    }
}
