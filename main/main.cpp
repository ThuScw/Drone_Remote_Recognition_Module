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

static const char* TAG = "SYS";

static BleRidBroadcaster broadcaster;
static GB46750Packet      gbPacket;
static FlightData         currentFlightData;

static uint64_t lastBroadcastTime  = 0;
static uint64_t lastDataUpdateTime = 0;
static uint64_t lastSelfTestTime   = 0;
static uint8_t  prevStatus         = 0xFF;
static bool     broadcastActive    = false;

static const char* statusNames[] = {"UNRPT", "GND", "AIR", "EMERG", "FAILSAFE", "FAILEND"};

static void handleStatusTransition(uint8_t newStatus) {
    bool shouldBroadcast = (newStatus == STATUS_AIRBORNE ||
                            newStatus == STATUS_EMERGENCY);

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

    if (!broadcaster.begin("ESP32C5_RID")) {
        ESP_LOGE(TAG, "FATAL: BLE init failed. Halting.");
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    if (!broadcaster.selfTest()) {
        ESP_LOGE(TAG, "FATAL: Self-test failed. Halting.");
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

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
        }

        // --- Step 3: BLE recovery ---
        if (broadcaster.needsRecovery()) {
            printf("[SYS] !!! BLE recovery in progress !!!\n");
            broadcastActive = false;

            if (broadcaster.attemptRecovery()) {
                printf("[SYS] BLE sync restored — restarting broadcast\n");
                if (currentFlightData.opStatus == STATUS_AIRBORNE ||
                    currentFlightData.opStatus == STATUS_EMERGENCY) {
                    if (broadcaster.startBroadcast(gbPacket)) {
                        broadcastActive = true;
                        printf("[SYS] Broadcast recovered and restarted\n");
                    } else {
                        ESP_LOGE(TAG, "Recovery restart broadcast failed");
                    }
                }
            }
        }

        // --- Step 4: Broadcast ---
        if (broadcastActive && (nowMs - lastBroadcastTime >= BROADCAST_INTERVAL_MS)) {
            lastBroadcastTime = nowMs;

            printf("[TX] Status=%s Alt=%.1fm Spd=%.1fm/s Hdg=%.0f\n",
                   statusNames[currentFlightData.opStatus],
                   currentFlightData.geoAlt, currentFlightData.speed,
                   currentFlightData.heading);

            uint8_t serialized[GB46750_MAX_PACKET];
            uint16_t len = gb46750_serialize(gbPacket, serialized, sizeof(serialized));
            printf("[TX] Packet (%d bytes): ", len);
            for (uint16_t i = 0; i < len; i++) {
                printf("%02X ", serialized[i]);
            }
            printf("\n");

            if (!broadcaster.updateBroadcastData(gbPacket)) {
                uint8_t fails = broadcaster.getUpdateFailures();
                if (fails >= 3) {
                    printf("[SYS] WARNING: %d consecutive broadcast update failures\n",
                           fails);
                }
            }
        }

        // --- Step 5: Runtime self-test (GB 42590-2023 A.2.3.5.5) ---
        if (nowMs - lastSelfTestTime >= SELF_TEST_INTERVAL_MS) {
            lastSelfTestTime = nowMs;
            broadcaster.runtimeCheck();
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
