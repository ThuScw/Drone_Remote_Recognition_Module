#include <stdio.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_task_wdt.h>
#include <nvs_flash.h>
#include "config.h"
#include "rid_messages.h"
#include "ble_rid_broadcaster.h"
#include "flight_data.h"
#include "indicators.h"
#include "flight_log.h"

static const char* TAG = "SYS";

static BleRidBroadcaster broadcaster;
static GB46750Packet      gbPacket;
static FlightData         currentFlightData;
static RIDInterlock       interlock;
static StatusLed          statusLed;
static FlightLog          flightLog;

static uint64_t lastBroadcastTime  = 0;
static uint64_t lastDataUpdateTime = 0;
static uint64_t lastSelfTestTime   = 0;
static uint64_t lastFlightLogTime  = 0;
static uint8_t  prevStatus         = 0xFF;
static bool     broadcastActive    = false;

static const char* statusNames[] = {"UNRPT", "GND", "AIR", "EMERG", "FAILSAFE", "FAILEND"};

static const char* getStatusName(uint8_t status) {
    if (status < sizeof(statusNames) / sizeof(statusNames[0])) {
        return statusNames[status];
    }
    return "INVALID";
}

static bool validateConfig() {
    // GB 46860-2025: UAS ID 必须为 20 字符 ASCII
    if (strlen(UAS_ID) != 20) {
        printf("[CONFIG] FATAL: UAS_ID must be 20 characters (current: %d)\n",
               (int)strlen(UAS_ID));
        return false;
    }
    // GB 46860-2025: 字符范围 [0-9A-HJ-NP-Z] (禁止 O 和 I)
    for (int i = 0; i < 20; i++) {
        char c = UAS_ID[i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z'))) {
            printf("[CONFIG] FATAL: UAS_ID[%d]='%c' — 仅允许 [0-9A-Z]\n", i, c);
            return false;
        }
        if (c == 'O' || c == 'I') {
            printf("[CONFIG] FATAL: UAS_ID[%d]='%c' — 字母 O/I 被 GB 46860 禁用\n", i, c);
            return false;
        }
    }
    if (strlen(REALNAME_ID) != 8) {
        printf("[CONFIG] FATAL: REALNAME_ID must be 8 characters (current: %d)\n",
               (int)strlen(REALNAME_ID));
        return false;
    }
    if (BROADCAST_INTERVAL_MS > 1000) {
        printf("[CONFIG] FATAL: BROADCAST_INTERVAL_MS must ≤ 1000ms (GB 46750 5.1.3)\n");
        return false;
    }
    if (BLE_ADV_INTERVAL_MS > BROADCAST_INTERVAL_MS) {
        printf("[CONFIG] FATAL: BLE_ADV_INTERVAL_MS must ≤ BROADCAST_INTERVAL_MS\n");
        return false;
    }
    printf("[CONFIG] Configuration validated OK\n");
    return true;
}

static void handleStatusTransition(uint8_t newStatus) {
    // 空中、紧急、失效状态都需要持续广播
    // see GB 46750-2025 Table 3-015
    bool shouldBroadcast = (newStatus == STATUS_AIRBORNE ||
                            newStatus == STATUS_EMERGENCY ||
                            newStatus == STATUS_FAIL_SAFE ||
                            newStatus == STATUS_FAIL_EMERG);

    if (shouldBroadcast && !broadcastActive) {
        printf("[SYS] Broadcast START (airborne/emergency)\n");
        if (broadcaster.startBroadcast(gbPacket)) {
            broadcastActive = true;
        } else {
            ESP_LOGE(TAG, "Failed to start broadcast");
        }
    } else if (!shouldBroadcast && broadcastActive) {
        printf("[SYS] Broadcast STOP (ground)\n");
        broadcaster.stopBroadcast();
        broadcastActive = false;
    }
}

extern "C" void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    // Task Watchdog — 主循环若卡死超过 5s, 触发系统复位
    esp_task_wdt_config_t twdt_cfg = {
        .timeout_ms = WATCHDOG_TIMEOUT_MS,
        .idle_core_mask = 0,
        .trigger_panic = true,
    };
    ret = esp_task_wdt_init(&twdt_cfg);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "TWDT init failed, rc=%d — watchdog disabled", ret);
    } else {
        esp_task_wdt_add(NULL);
        printf("[SYS] Task watchdog enabled (%ld ms timeout)\n",
               (long)WATCHDOG_TIMEOUT_MS);
    }

    printf("\n============================================\n");
    printf("ESP32-C5 RID Broadcaster — GB 46750-2025\n");
    printf("Light Show Drone (35cm, Class 1)\n");
    printf("BLE5 Extended Advertising Mode\n");
    printf("Data Source: Mock (65s flight cycle)\n");
    printf("============================================\n\n");

    if (!validateConfig()) {
        ESP_LOGE(TAG, "FATAL: Configuration validation failed. Halting.");
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    if (!broadcaster.begin("ESP32C5_RID")) {
        ESP_LOGE(TAG, "FATAL: BLE init failed. Halting.");
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    if (!broadcaster.selfTest()) {
        ESP_LOGE(TAG, "FATAL: Self-test failed. Halting.");
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    // 初始化指示灯和联锁 (GB 46750-2025 5.1.5, 5.1.7)
    if (!statusLed.init()) {
        ESP_LOGW(TAG, "Status LED init failed — continuing without visual indicator");
    }
    if (!interlock.init()) {
        ESP_LOGW(TAG, "Interlock init failed — continuing without flight controller interlock");
    }

    // 飞行数据存储 (GB 46750-2025 5.1.8)
    if (!flightLog.init()) {
        ESP_LOGW(TAG, "Flight log init failed — continuing without persistent storage");
    }

    // 模块自检全部通过 → 联锁就绪, 允许飞控起飞
    interlock.arm();
    statusLed.setState(LedState::STANDBY);

    uint64_t nowMs = (uint64_t)(esp_timer_get_time() / 1000);

    getFlightData(currentFlightData, nowMs);
    gb46750_buildPacket(gbPacket, currentFlightData, UAS_ID, REALNAME_ID,
                        OP_CATEGORY, UA_CLASS, OP_LOCATION_TYPE, COORD_SYS,
                        HORIZ_ACC, VERT_ACC, SPEED_ACC, TS_ACC, nowMs);

    printf("[SYS] Packet=%d bytes | Broadcast every %ld ms | Update every %ld ms\n",
           gbPacket.totalLen, (long)BROADCAST_INTERVAL_MS, (long)DATA_UPDATE_INTERVAL_MS);
    printf("[SYS] Ready. Monitor with nRF Connect.\n\n");

    lastDataUpdateTime = nowMs;
    lastBroadcastTime  = nowMs;
    lastSelfTestTime   = nowMs;

    while (1) {
        esp_task_wdt_reset();

        nowMs = (uint64_t)(esp_timer_get_time() / 1000);

        // --- Step 1: Read data source ---
        if (nowMs - lastDataUpdateTime >= DATA_UPDATE_INTERVAL_MS) {
            lastDataUpdateTime = nowMs;
            getFlightData(currentFlightData, nowMs);
            gb46750_buildPacket(gbPacket, currentFlightData, UAS_ID, REALNAME_ID,
                                OP_CATEGORY, UA_CLASS, OP_LOCATION_TYPE, COORD_SYS,
                                HORIZ_ACC, VERT_ACC, SPEED_ACC, TS_ACC, nowMs);
        }

        // --- Step 2: Handle flight phase transition ---
        if (currentFlightData.opStatus != prevStatus) {
            handleStatusTransition(currentFlightData.opStatus);
            prevStatus = currentFlightData.opStatus;
            // 状态变更时更新 LED: 广播中=快闪, 待机=慢闪
            if (broadcastActive) {
                statusLed.setState(LedState::BROADCASTING);
            } else {
                statusLed.setState(LedState::STANDBY);
            }
        }

        // --- Step 3: BLE recovery ---
        if (broadcaster.needsRecovery()) {
            printf("[SYS] !!! BLE controller reset detected — starting self-heal !!!\n");
            broadcastActive = false;

            auto result = broadcaster.attemptSelfHeal(gbPacket);
            bool isAirborne = (currentFlightData.opStatus == STATUS_AIRBORNE ||
                               currentFlightData.opStatus == STATUS_EMERGENCY ||
                               currentFlightData.opStatus == STATUS_FAIL_SAFE ||
                               currentFlightData.opStatus == STATUS_FAIL_EMERG);

            if (result != BleRidBroadcaster::RecoveryResult::FAILED) {
                printf("[SYS] BLE self-heal OK (%s) — broadcast restarted\n",
                       result == BleRidBroadcaster::RecoveryResult::RECOVERED
                           ? "recovered" : "degraded");
                if (isAirborne) {
                    broadcastActive = true;
                } else {
                    broadcastActive = false;
                    broadcaster.stopBroadcast();
                    printf("[SYS] On ground — broadcast stopped, interlock ready\n");
                }
            } else {
                printf("[SYS] BLE self-heal FAILED — all 3 tiers exhausted\n");
                statusLed.setState(LedState::FAULT);
                if (isAirborne) {
                    // 空中: 只告警不拉闸, 飞控继续自主飞行
                    printf("[SYS] AIRBORNE: keeping interlock armed — drone flies on\n");
                } else {
                    // 地面: 拉闸禁止起飞
                    if (interlock.isArmed()) {
                        interlock.disarm();
                        printf("[SYS] GROUND: interlock DISARMED — takeoff blocked\n");
                    }
                }
            }
        }

        // --- Step 4: Broadcast ---
        if (broadcastActive && (nowMs - lastBroadcastTime >= BROADCAST_INTERVAL_MS)) {
            lastBroadcastTime = nowMs;

#if CONFIG_RID_VERBOSE_LOG
            printf("[TX] Status=%s Alt=%.1fm Spd=%.1fm/s Hdg=%.0f\n",
                   getStatusName(currentFlightData.opStatus),
                   currentFlightData.geoAlt, currentFlightData.speed,
                   currentFlightData.heading);
#endif

            uint8_t serialized[GB46750_MAX_PACKET];
            uint16_t len = gb46750_serialize(gbPacket, serialized, sizeof(serialized));
#if CONFIG_RID_VERBOSE_LOG
            printf("[TX] Packet (%d bytes): ", len);
            for (uint16_t i = 0; i < len; i++) {
                printf("%02X ", serialized[i]);
            }
            printf("\n");
#endif

            if (!broadcaster.updateBroadcastData(gbPacket)) {
                uint8_t fails = broadcaster.getUpdateFailures();
                if (fails >= 3) {
                    printf("[SYS] CRITICAL: %d consecutive update failures — self-healing\n",
                           fails);
                    auto result = broadcaster.attemptSelfHeal(gbPacket);
                    bool isAirborne = (currentFlightData.opStatus == STATUS_AIRBORNE ||
                                       currentFlightData.opStatus == STATUS_EMERGENCY ||
                                       currentFlightData.opStatus == STATUS_FAIL_SAFE ||
                                       currentFlightData.opStatus == STATUS_FAIL_EMERG);

                    if (result != BleRidBroadcaster::RecoveryResult::FAILED) {
                        broadcastActive = true;
                        printf("[SYS] Update failure self-heal OK (%s)\n",
                               result == BleRidBroadcaster::RecoveryResult::RECOVERED
                                   ? "recovered" : "degraded");
                    } else {
                        printf("[SYS] Update self-heal FAILED\n");
                        broadcastActive = false;
                        statusLed.setState(LedState::FAULT);
                        if (!isAirborne && interlock.isArmed()) {
                            interlock.disarm();
                            printf("[SYS] GROUND: interlock DISARMED — takeoff blocked\n");
                        } else if (isAirborne) {
                            printf("[SYS] AIRBORNE: keeping interlock armed — drone flies on\n");
                        }
                    }
                }
            }
        }

        // --- Step 5: Flight data log (GB 46750-2025 5.1.8) ---
        if (nowMs - lastFlightLogTime >= FLIGHT_LOG_INTERVAL_S * 1000) {
            lastFlightLogTime = nowMs;
            uint8_t serialized[GB46750_MAX_PACKET];
            uint16_t len = gb46750_serialize(gbPacket, serialized, sizeof(serialized));
            if (len > 0) {
                flightLog.writeRecord(serialized, len, nowMs);
            }
        }

        // --- Step 6: Runtime self-test (GB 42590-2023 A.2.3.5.5) ---
        if (nowMs - lastSelfTestTime >= SELF_TEST_INTERVAL_MS) {
            lastSelfTestTime = nowMs;
            bool healthy = broadcaster.runtimeCheck();
            if (!healthy) {
                printf("[SELF-TEST] FAIL — attempting self-heal\n");
                auto result = broadcaster.attemptSelfHeal(gbPacket);
                bool isAirborne = (currentFlightData.opStatus == STATUS_AIRBORNE ||
                                   currentFlightData.opStatus == STATUS_EMERGENCY ||
                                   currentFlightData.opStatus == STATUS_FAIL_SAFE ||
                                   currentFlightData.opStatus == STATUS_FAIL_EMERG);

                if (result != BleRidBroadcaster::RecoveryResult::FAILED) {
                    printf("[SELF-TEST] Self-heal OK (%s)\n",
                           result == BleRidBroadcaster::RecoveryResult::RECOVERED
                               ? "recovered" : "degraded");
                    broadcastActive = isAirborne;
                    healthy = true;
                } else {
                    printf("[SELF-TEST] Self-heal FAILED\n");
                    statusLed.setState(LedState::FAULT);
                    if (isAirborne) {
                        // 空中: 只告警, 飞控自主飞行, 不拉闸
                        printf("[SELF-TEST] AIRBORNE: keeping interlock — drone continues\n");
                        broadcastActive = false;  // 广播已停, 不再尝试发送
                    } else {
                        // 地面: 模块真坏了, 禁止起飞
                        if (interlock.isArmed()) {
                            interlock.disarm();
                            printf("[SELF-TEST] GROUND: interlock DISARMED\n");
                        }
                    }
                }
            }
        }

        // LED 闪烁刷新 (每循环一次)
        statusLed.update();

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
