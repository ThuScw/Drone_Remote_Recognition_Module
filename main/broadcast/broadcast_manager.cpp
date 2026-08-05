// ======================== RIDBroadcastManager 实现 ========================
//
// 所有广播编排、安全校验、BLE 恢复、飞行日志逻辑集中于此。
// 主循环不直接操作广播状态，只调用 update()。

#include <esp_log.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <esp_mac.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "broadcast_manager.h"
#include "config.h"
#include "fault_log.h"

static const char* TAG = "BCAST";

// ======================== 构造 / 初始化 ========================

RIDBroadcastManager::RIDBroadcastManager(
    BleRidBroadcaster& broadcaster,
    FlightLog& flightLog,
    StatusLed& statusLed,
    IFcInterlink& interlink)
    : _broadcaster(broadcaster)
    , _flightLog(flightLog)
    , _statusLed(statusLed)
    , _interlink(interlink)
    , _broadcastActive(false)
    , _nextBroadcastMs(0)
    , _lastBroadcastSuccessMs(0)
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

    // 3. 待机指示 (GB 46750-2025 5.1.5)
    _statusLed.setState(LedState::STANDBY);

    uint64_t nowMs = (uint64_t)(esp_timer_get_time() / 1000);
    _lastDataUpdateMs = nowMs;
    _nextBroadcastMs  = nowMs;
    _lastFlightLogMs  = nowMs;
    _lastSelfTestMs   = nowMs;
    _lastHeapCheckMs  = nowMs;

    // 基于蓝牙 MAC 地址的广播时隙偏移
    // 每颗 ESP32 MAC 全球唯一，无需配置即可实现多机广播自然错开
    uint8_t mac[6];
    esp_err_t macRet = esp_read_mac(mac, ESP_MAC_BT);
    uint16_t jitterMs = 0;
    if (macRet == ESP_OK) {
        // djb2 hash of BT MAC — 均匀分布, 使不同设备的首包时隙错开
        uint32_t hash = 5381;
        for (int i = 0; i < 6; i++) {
            hash = ((hash << 5) + hash) + mac[i];
        }
        jitterMs = (uint16_t)(hash % BROADCAST_INTERVAL_MS);
        // 首包在 nowMs + jitterMs 时刻发送; 之后的时隙在网格上累加 (见 handleBroadcast)
        _nextBroadcastMs = nowMs + jitterMs;
        ESP_LOGI(TAG, "Broadcast jitter: %dms (MAC %02X:%02X:%02X:%02X:%02X:%02X)",
                 jitterMs, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
        ESP_LOGE(TAG, "Failed to read BT MAC (err=%d), using zero jitter", (int)macRet);
    }

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
    handleStatusTransition(nowMs);

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
//   - 范围验证失败的字段: 编码为表3未知哨兵值，但不阻止包构建
//   - M 字段缺失 (validMask 未置位): dataId 位仍为 1，值编码为表3未知哨兵值
//   - 这确保每包都携带最新状态，接收方始终能解析到最新数据

void RIDBroadcastManager::validateAndBuildPacket(const FlightData& fd, uint64_t nowMs) {
    // 数据无效时保留上次有效数据，防止飞行中因数据短暂丢失而误判为地面状态
    if (fd.freshness == FRESH_INVALID) {
        ESP_LOGW(TAG, "Data INVALID — keeping last known state, not updating packet");
        return;
    }

    // 保存最新数据供广播使用
    _lastValidData = fd;

    // 范围验证 (物理边界)
    uint32_t validationFlags = 0;
    bool rangeValid = gb46750_validateFlightData(fd, validationFlags);

    if (!rangeValid) {
        _validationFailCount++;
        // 只在"进入故障"时记录, 防持续越界数据每更新周期刷环形缓冲
        if (!_rangeBad) {
            _rangeBad = true;
            faultLogRecord(FAULT_VALIDATION, nowMs);
        }
        ESP_LOGW(TAG, "Data range validation failed (flags=0x%08lx, count=%lu), "
                 "building with unknown values for invalid fields",
                 (unsigned long)validationFlags, (unsigned long)_validationFailCount);
        // 不阻止包构建 — 无效字段在 buildPacket 中编码为表3未知哨兵值
    } else {
        _rangeBad = false;
    }

    // 位置老化 (GB 46750-2025 表3-008/009/010/013): 超过阈值未更新的
    // 位置/高度/速度/航向编码为"未知"哨兵值，而不是广播过期旧坐标 —
    // 防止监管设备依据过期位置做禁飞区/冲突判断时产生安全事故。
    FlightData broadcastFd = fd;
    gb46750_expireStaleFields(broadcastFd, nowMs, DATA_FRESH_THRESHOLD_MS);

    // 范围校验失败的字段: 清除 validMask, 让 buildPacket 走"缺失"分支编码表3哨兵值。
    // 若不清除, 编码函数会对越界值做钳位广播 (如 400° 航向→359.9°, 越界速度→6553.5),
    // 接收方得到伪造的精确值而非"未知/不可用", 与哨兵语义不一致 (GB 46750-2025 表3)。
    if (validationFlags) {
        broadcastFd.validMask &= ~validationFlags;
    }

    // 始终构建新包 (P0: 不再 "keeping previous packet")
    // 精度: 直接用 GPS eph/epv 映射结果; eph/epv 不可用 (≤0) 时映射为 0 (unknown),
    // 如实上报 unknown, 不做 fallback 伪造 (表3-017/018 unknown=0)
    uint8_t horizAcc = gb46750_mapHorizAcc(broadcastFd.horizAccM);
    uint8_t vertAcc  = gb46750_mapVertAcc(broadcastFd.vertAccM);

    uint8_t tsAcc = (broadcastFd.unixTimestampMs == 0) ? 0 : TS_ACC;

    gb46750_buildPacket(_currentPacket, broadcastFd, UAS_ID, REALNAME_ID,
                        OP_CATEGORY, UA_CLASS, OP_LOCATION_TYPE, COORD_SYS,
                        horizAcc, vertAcc, SPEED_ACC, tsAcc,
                        broadcastFd.unixTimestampMs);
}

// ======================== BLE 自修复 ========================
//
// P1 合规修复: 三处重复代码合并为单一方法
// 触发源: needsRecovery() 标志 / updateFailures >= 3 / 自检失败

void RIDBroadcastManager::triggerSelfHeal() {
    auto result = _broadcaster.attemptSelfHeal(_currentPacket);

    if (result != BleRidBroadcaster::RecoveryResult::FAILED) {
        // DEGRADED (PHY 切换/NimBLE 重初始化后) → LED 橙色降级提示 (旧7)
        const bool degraded = (result == BleRidBroadcaster::RecoveryResult::DEGRADED);
        const char* mode = degraded ? "degraded" : "recovered";
        faultLogRecord(degraded ? FAULT_BLE_HEAL_DEGRADED : FAULT_BLE_HEAL_OK,
                       (uint64_t)(esp_timer_get_time() / 1000));
        ESP_LOGI(TAG, "Self-heal OK (%s)", mode);
        _interlink.notifyRecovered((uint64_t)(esp_timer_get_time() / 1000));

        if (isAirborne()) {
            _broadcastActive = true;
            _statusLed.setState(degraded ? LedState::DEGRADED : LedState::BROADCASTING);
        } else {
            _broadcastActive = false;
            _broadcaster.stopBroadcast();
            _statusLed.setState(degraded ? LedState::DEGRADED : LedState::STANDBY);
            ESP_LOGI(TAG, "On ground — broadcast stopped");
        }
    } else {
        ESP_LOGE(TAG, "Self-heal FAILED — all 3 tiers exhausted");
        faultLogRecord(FAULT_BLE_HEAL_FAILED, (uint64_t)(esp_timer_get_time() / 1000));
        // GB 46750-2025 5.1.7: 识别发送功能失效 → 通知飞控安全处置
        _interlink.notifyFault(InterlinkReason::BLE_HEAL_FAILED, isAirborne(),
                               (uint64_t)(esp_timer_get_time() / 1000));
        _statusLed.setState(LedState::FAULT);
        _broadcastActive = false;
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

void RIDBroadcastManager::handleStatusTransition(uint64_t nowMs) {
    uint8_t newStatus = _lastValidData.opStatus;
    uint8_t oldConfirmed = _debounce.confirmed;
    uint64_t oldStartMs  = _debounce.startMs;

    // 决策逻辑为纯函数 (status_machine.h)，这里只做日志与副作用执行
    StatusStepResult r = statusStep(_debounce, newStatus, nowMs);

    switch (r) {
    case StatusStepResult::UNCHANGED:
        // 状态一致 — 消抖已重置
        break;
    case StatusStepResult::EMERGENCY:
        ESP_LOGW(TAG, "Status EMERGENCY/FAIL-SAFE — immediate transition (status=%d)", newStatus);
        applyStatusChange(_debounce.confirmed);
        break;
    case StatusStepResult::DEBOUNCED:
        ESP_LOGI(TAG, "Status transition %d→%d (debounced, %lums)",
                 oldConfirmed, newStatus, (unsigned long)(nowMs - oldStartMs));
        applyStatusChange(_debounce.confirmed);
        break;
    case StatusStepResult::PENDING:
        break;
    }
}

// ======================== 状态切换执行 ========================

void RIDBroadcastManager::applyStatusChange(uint8_t newStatus) {
    bool shouldBroadcast = (newStatus == STATUS_AIRBORNE || newStatus == STATUS_EMERGENCY ||
                            newStatus == STATUS_FAIL_SAFE || newStatus == STATUS_FAIL_EMERG);

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
}

// ======================== 广播发送 ========================
//
// P0 合规修复:
//   - 永不跳过广播 (删除 goto skip_broadcast)
//   - 数据过期/缺失时仍广播，附加日志告警
//   - 满足 GB 46750-2025 5.1.2 "全过程自动持续发送"

void RIDBroadcastManager::handleBroadcast(uint64_t nowMs) {
    if (!_broadcastActive) return;
    if (nowMs < _nextBroadcastMs) return;

    // 相位累加防漂移 (P1-5): 下一发送时刻在理想网格上累加 INTERVAL,
    // 而不是重置为 nowMs — 否则每次广播最多额外漂移一个循环周期(≈10ms),
    // 长时统计上间隔不均 (主循环延迟后基准点左移, 后续间隔被压缩)。
    _nextBroadcastMs += BROADCAST_INTERVAL_MS;

    // 追赶保护: 主循环长时间阻塞 (USB/Flash/BLE 自修复) 导致落后多个周期时,
    // 只补发本次并把网格前移到当前时刻, 避免连续突发补包; 下一周期即恢复 ≤1s 间隔。
    if (_nextBroadcastMs < nowMs) {
        _nextBroadcastMs = nowMs;
    }

    _broadcastCount++;

    // 新鲜度检查 — 仅用于日志告警，不阻止广播
    DataFreshness freshness = gb46750_checkFreshness(
        _lastValidData, nowMs, DATA_FRESH_THRESHOLD_MS);

    if (freshness == FRESH_STALE) {
        ESP_LOGW(TAG, "Data STALE (> %d ms), broadcasting anyway",
                 DATA_FRESH_THRESHOLD_MS);
        if (!_staleReported) { _staleReported = true; faultLogRecord(FAULT_STALE_BROADCAST, nowMs); }
    } else if (freshness == FRESH_INVALID) {
        ESP_LOGW(TAG, "Data INVALID (M-fields missing), broadcasting with unknown values");
        if (!_staleReported) { _staleReported = true; faultLogRecord(FAULT_STALE_BROADCAST, nowMs); }
    } else {
        _staleReported = false;
    }

    // 序列化并发送
    uint8_t serialized[GB46750_MAX_PACKET];
    uint16_t len = gb46750_serialize(_currentPacket, serialized, sizeof(serialized));
    if (len == 0) {
        ESP_LOGE(TAG, "Serialize failed — packet too large");
        return;
    }

#if CONFIG_RID_VERBOSE_LOG
    ESP_LOGI(TAG, "TX #%lu (%d bytes):", (unsigned long)_broadcastCount, len);
    ESP_LOG_BUFFER_HEXDUMP(TAG, serialized, len, ESP_LOG_INFO);
#endif

    if (!_broadcaster.updateBroadcastData(_currentPacket)) {
        ESP_LOGW(TAG, "Broadcast data update failed (count=%d)",
                 _broadcaster.getUpdateFailures());
        // handleBleRecovery() 会在下次 update() 中处理连续失败
    } else {
        _lastBroadcastSuccessMs = nowMs;  // 仅记录实际发送成功的时刻 (问题2)
    }
}

// ======================== 飞行日志 ========================
// GB 46750-2025 5.1.8: 滚动存储，间隔 ≤10s

void RIDBroadcastManager::handleFlightLog(uint64_t nowMs) {
    if (nowMs - _lastFlightLogMs < (uint64_t)(FLIGHT_LOG_INTERVAL_S * 1000)) return;
    _lastFlightLogMs = nowMs;

    // 包未构建时 (FRESH_INVALID, 尚无有效飞行数据) 不落盘:
    // gb46750_serialize 对全零包返回 3 (仅固定头), len>0 判据形同虚设, 会存空 stub。
    if (_currentPacket.dataIdLen == 0 || _currentPacket.contentLen == 0) return;

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

    // 合规监测 (问题2): 空中广播期间若实际发送成功时间落后过多, 提示潜在静默数据缺口
    uint64_t nowMs = (uint64_t)(esp_timer_get_time() / 1000);
    if (_lastBroadcastSuccessMs != 0 &&
        nowMs - _lastBroadcastSuccessMs > (uint64_t)(BROADCAST_INTERVAL_MS * 3)) {
        ESP_LOGW(TAG, "No successful broadcast for %llu ms (interval=%dms) — possible silent data gap",
                 (unsigned long long)(nowMs - _lastBroadcastSuccessMs),
                 (int)BROADCAST_INTERVAL_MS);
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
        faultLogRecord(FAULT_LOW_HEAP, nowMs);
    }

    // 主任务 (app_main) 栈高水位 — update() 在主循环上下文执行
    // StackType_t 在 ESP32 上为 1 字节, 乘以 sizeof 以保持跨端口一致
    uint32_t stackHwm = (uint32_t)uxTaskGetStackHighWaterMark(NULL) * (uint32_t)sizeof(StackType_t);
    ESP_LOGI(TAG, "Main stack high-water: %lu bytes", (unsigned long)stackHwm);
    if (stackHwm < 1024) {
        ESP_LOGW(TAG, "Main stack LOW (HWM=%lu) — consider increasing CONFIG_ESP_MAIN_TASK_STACK_SIZE",
                 (unsigned long)stackHwm);
    }
}
