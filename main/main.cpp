// ======================== ESP32-S3 RID Broadcaster ========================
//
// GB 46750-2025 compliant remote identification module.
// Light show drone class (35cm, Class 1), BLE5 extended advertising.
//
// Architecture:
//   main.cpp           — thin orchestrator (init + loop only)
//   broadcast/         — all safety/broadcast logic (RIDBroadcastManager)
//   protocol/          — GB 46750-2025 packet encoding
//   broadcaster/       — BLE transport (NimBLE)
//   data/              — flight data source (USB Host CDC-ACM → MAVLink)
//   indicators/        — LED + flight controller interlock
//   logging/           — persistent flight data recorder

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_task_wdt.h>
#include <nvs_flash.h>

#include "config.h"
#include "broadcast_manager.h"
#include "ble_rid_broadcaster.h"
#include "flight_data.h"
#include "indicators.h"
#include "flight_log.h"

static const char* TAG = "SYS";

static BleRidBroadcaster    broadcaster;
static FlightLog            flightLog;
static RIDInterlock         interlock;
static StatusLed            statusLed;
static RIDBroadcastManager* manager = nullptr;

extern "C" void app_main(void) {
    // --- NVS ---
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    // --- Task Watchdog (GB 42590-2023 A.2.3.5.5) ---
    esp_task_wdt_config_t twdt_cfg = {
        .timeout_ms    = WATCHDOG_TIMEOUT_MS,
        .idle_core_mask = 0,
        .trigger_panic = true,
    };
    ret = esp_task_wdt_reconfigure(&twdt_cfg);
    if (ret == ESP_ERR_INVALID_STATE) {
        ret = esp_task_wdt_init(&twdt_cfg);
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "TWDT init/reconfig failed (rc=%d) — watchdog disabled", (int)ret);
    } else {
        esp_task_wdt_add(NULL);
        ESP_LOGI(TAG, "Task watchdog enabled (%ld ms timeout)", (long)WATCHDOG_TIMEOUT_MS);
    }

    ESP_LOGI(TAG, "=== ESP32-S3 RID Broadcaster — GB 46750-2025 ===");
    ESP_LOGI(TAG, "Light show drone | BLE5 Extended Advertising | USB Host CDC-ACM");

    // --- Subsystems ---
    if (!broadcaster.begin("ESP32S3_RID")) {
        ESP_LOGE(TAG, "FATAL: BLE init failed — halting");
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    if (!statusLed.init()) {
        ESP_LOGW(TAG, "Status LED init failed — continuing without visual indicator");
    }
    if (!interlock.init()) {
        ESP_LOGW(TAG, "Interlock init failed — continuing without FC interlock");
    }
#if MAVLINK_TX_ENABLED
    interlock.setSendCallback(flightData_sendArmDisarm);
    ESP_LOGI(TAG, "MAVLink TX enabled — arm/disarm commands will be sent over USB");
#else
    ESP_LOGI(TAG, "MAVLink TX disabled — using GPIO%d hardware interlock only", INTERLOCK_RID_OK_GPIO);
#endif

    if (!flightLog.init()) {
        ESP_LOGW(TAG, "Flight log init failed — continuing without persistent storage");
    }

    // --- Broadcast Manager (all safety + orchestration logic) ---
    manager = new RIDBroadcastManager(broadcaster, flightLog, statusLed, interlock);
    if (!manager->init()) {
        ESP_LOGE(TAG, "FATAL: Broadcast manager init failed — halting");
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    ESP_LOGI(TAG, "Ready. Monitor with nRF Connect.\n");

    // --- Main Loop ---
    FlightData fd = {};  // 零初始化: opStatus=STATUS_GROUND, freshness=FRESH_INVALID
    while (1) {
        esp_task_wdt_reset();

        uint64_t nowMs = (uint64_t)(esp_timer_get_time() / 1000);
        getFlightData(fd, nowMs);
        manager->update(fd, nowMs);
        statusLed.update();

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
