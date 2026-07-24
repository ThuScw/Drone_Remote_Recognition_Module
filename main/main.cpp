#include <stdio.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <nvs_flash.h>
#include "config.h"
#include "rid_messages.h"
#include "ble_rid_broadcaster.h"

static const char* TAG = "SYS";

static BleRidBroadcaster broadcaster;
static GB46750Packet      gbPacket;
static FlightData         currentFlightData;

static uint64_t lastBroadcastTime   = 0;
static uint64_t lastDataUpdateTime  = 0;
static uint64_t lastSelfTestTime    = 0;
static uint8_t  prevStatus          = 0xFF;
static bool     broadcastActive     = false;

static const char* statusNames[] = {"UNRPT", "GND", "AIR", "EMERG", "FAILSAFE", "FAILEND"};

// --- Forward declarations ---
static void updateFlightData(void);
static void handleStatusTransition(uint8_t newStatus);

// --- ESP-IDF entry point ---
extern "C" void app_main(void) {
    // Initialize NVS (required by NimBLE for address storage)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    printf("\n============================================\n");
    printf("ESP32-C5 RID Broadcaster - GB 46750-2025\n");
    printf("Light Show Drone (35cm, Class 1)\n");
    printf("BLE5 Extended Advertising Mode\n");
    printf("============================================\n\n");

    if (!broadcaster.begin("ESP32C5_RID")) {
        ESP_LOGE(TAG, "FATAL: BLE init failed. Halting.");
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    if (!broadcaster.selfTest()) {
        ESP_LOGE(TAG, "FATAL: Self-test failed. Halting.");
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    // Initialize flight data with mock values
    memset(&currentFlightData, 0, sizeof(currentFlightData));
    currentFlightData.lat       = MOCK_LATITUDE;
    currentFlightData.lon       = MOCK_LONGITUDE;
    currentFlightData.geoAlt    = MOCK_GEO_ALT;
    currentFlightData.baroAlt   = MOCK_BARO_ALT;
    currentFlightData.heightAgl = MOCK_HEIGHT_AGL;
    currentFlightData.speed     = MOCK_SPEED;
    currentFlightData.heading   = MOCK_HEADING;
    currentFlightData.vspeed    = MOCK_VSPEED;
    currentFlightData.opStatus  = STATUS_AIRBORNE;
    currentFlightData.opLat     = MOCK_OP_LAT;
    currentFlightData.opLon     = MOCK_OP_LON;
    currentFlightData.opAlt     = MOCK_OP_ALT;

    uint64_t nowMs = (uint64_t)(esp_timer_get_time() / 1000);

    gb46750_buildPacket(gbPacket, currentFlightData, UAS_ID, REALNAME_ID,
                        OP_CATEGORY, UA_CLASS, OP_LOCATION_TYPE, COORD_SYS,
                        HORIZ_ACC, VERT_ACC, SPEED_ACC, TS_ACC, nowMs);

    printf("[SYS] Packet=%d bytes | Broadcast every %ld ms | Self-test every %ld ms\n",
           gbPacket.totalLen, (long)BROADCAST_INTERVAL_MS, (long)SELF_TEST_INTERVAL_MS);
    printf("[SYS] Ready. Monitor with nRF Connect.\n\n");

    lastDataUpdateTime = nowMs;
    lastBroadcastTime  = nowMs;
    lastSelfTestTime   = nowMs;

    // --- Main loop (replaces Arduino loop()) ---
    while (1) {
        nowMs = (uint64_t)(esp_timer_get_time() / 1000);

        // Data Update (independent of broadcast)
        if (nowMs - lastDataUpdateTime >= DATA_UPDATE_INTERVAL_MS) {
            lastDataUpdateTime = nowMs;
            updateFlightData();
            gb46750_buildPacket(gbPacket, currentFlightData, UAS_ID, REALNAME_ID,
                                OP_CATEGORY, UA_CLASS, OP_LOCATION_TYPE, COORD_SYS,
                                HORIZ_ACC, VERT_ACC, SPEED_ACC, TS_ACC, nowMs);
        }

        // Broadcast Working Interval (GB 42590-2023 A.2.2.3.3)
        if (currentFlightData.opStatus != prevStatus) {
            handleStatusTransition(currentFlightData.opStatus);
            prevStatus = currentFlightData.opStatus;
        }

        // Broadcast via BLE5 Extended Advertising
        if (broadcastActive && (nowMs - lastBroadcastTime >= BROADCAST_INTERVAL_MS)) {
            lastBroadcastTime = nowMs;
            broadcaster.updateAndBroadcast(gbPacket);

            printf("[TX] Status=%s | Alt=%.1fm | Spd=%.1fm/s | Hdg=%.0f\n",
                   statusNames[currentFlightData.opStatus],
                   currentFlightData.geoAlt, currentFlightData.speed,
                   currentFlightData.heading);
        }

        // Runtime Continuous Self-Test (GB 42590-2023 A.2.3.5.5)
        if (nowMs - lastSelfTestTime >= SELF_TEST_INTERVAL_MS) {
            lastSelfTestTime = nowMs;
            broadcaster.runtimeCheck();
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// --- Flight data update (30s phase rotation for demo) ---
static void updateFlightData(void) {
    uint64_t nowMs = (uint64_t)(esp_timer_get_time() / 1000);
    uint32_t cycle = (uint32_t)((nowMs / 30000) % 4);

    switch (cycle) {
        case 0: currentFlightData.opStatus = STATUS_GROUND;    break;
        case 1: currentFlightData.opStatus = STATUS_AIRBORNE;  break;
        case 2: currentFlightData.opStatus = STATUS_EMERGENCY; break;
        case 3: currentFlightData.opStatus = STATUS_AIRBORNE;  break;
    }

    currentFlightData.lat       = MOCK_LATITUDE;
    currentFlightData.lon       = MOCK_LONGITUDE;
    currentFlightData.geoAlt    = MOCK_GEO_ALT;
    currentFlightData.baroAlt   = MOCK_BARO_ALT;
    currentFlightData.heightAgl = MOCK_HEIGHT_AGL;
    currentFlightData.speed     = MOCK_SPEED;
    currentFlightData.heading   = MOCK_HEADING;
    currentFlightData.vspeed    = MOCK_VSPEED;
    currentFlightData.opLat     = MOCK_OP_LAT;
    currentFlightData.opLon     = MOCK_OP_LON;
    currentFlightData.opAlt     = MOCK_OP_ALT;
}

// --- Broadcast working interval state machine ---
static void handleStatusTransition(uint8_t newStatus) {
    // GB 42590-2023: broadcast during entire flight, stop on ground
    bool shouldBroadcast = (newStatus == STATUS_AIRBORNE ||
                            newStatus == STATUS_EMERGENCY);

    if (shouldBroadcast && !broadcastActive) {
        printf("[SYS] Broadcast START (airborne/emergency)\n");
        broadcastActive = true;
    } else if (!shouldBroadcast && broadcastActive) {
        printf("[SYS] Broadcast STOP (ground)\n");
        broadcaster.stopBroadcast();
        broadcastActive = false;
    }
}
