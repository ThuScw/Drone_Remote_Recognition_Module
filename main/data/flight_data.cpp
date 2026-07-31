#include "flight_data.h"
#include "mavlink_parser.h"
#include "mavlink_tx.h"
#include "config.h"
#include <inttypes.h>
#include "esp_timer.h"
#include "esp_log.h"

#include "usb/usb_host.h"
#include "usb/cdc_acm_host.h"

static const char* TAG = "FLIGHT_DATA";

// ======================== FlightDataSource ========================
// Encapsulates USB host + MAVLink parser state.
// USB callbacks use static trampolines with void* user_arg → this.

class FlightDataSource {
public:
    void getFlightData(FlightData& fd, uint64_t nowMs);
    bool sendToFC(const uint8_t* data, size_t len);
    bool sendArmDisarm(bool arm);

private:
    // --- USB callback trampolines (C ABI → member functions) ---
    static bool  usbDataCb(const uint8_t* data, size_t data_len, void* arg);
    static void  usbEventCb(const cdc_acm_host_dev_event_data_t* event, void* user_ctx);
    static void  usbHostTask(void* arg);

    // --- Internal methods ---
    void onUsbData(const uint8_t* data, size_t data_len, uint64_t nowMs);
    void onUsbEvent(const cdc_acm_host_dev_event_data_t* event);
    void runUsbHostLoop();
    void configureLineCoding(cdc_acm_dev_hdl_t dev);
    esp_err_t tryOpenUsbDevice();
    void initUsbHost();
    bool tryUsbRecovery(uint64_t nowMs);

    // --- State (formerly file-static globals) ---
    MavlinkParser      _parser;
    portMUX_TYPE       _parserMux = portMUX_INITIALIZER_UNLOCKED;
    bool               _usbHostInitialized = false;
    TaskHandle_t       _usbHostTaskHandle = nullptr;
    cdc_acm_dev_hdl_t  _cdcDev = nullptr;
    bool               _deviceConnected = false;
    bool               _deviceReady = false;
    uint32_t           _recoveryCount = 0;
    uint64_t           _lastRecoveryMs = 0;
};

// Single module-level instance — public free functions delegate to it.
static FlightDataSource s_source;

// ======================== Public free-function API ========================

void getFlightData(FlightData& fd, uint64_t nowMs) {
    s_source.getFlightData(fd, nowMs);
}

bool flightData_sendToFC(const uint8_t* data, size_t len) {
    return s_source.sendToFC(data, len);
}

bool flightData_sendArmDisarm(bool arm) {
    return s_source.sendArmDisarm(arm);
}

// ======================== USB callback trampolines ========================

bool FlightDataSource::usbDataCb(const uint8_t* data, size_t data_len, void* arg) {
    auto* self = static_cast<FlightDataSource*>(arg);
    if (!data || data_len == 0) return true;
    uint64_t nowMs = esp_timer_get_time() / 1000;
    self->onUsbData(data, data_len, nowMs);
    return true;
}

void FlightDataSource::usbEventCb(const cdc_acm_host_dev_event_data_t* event, void* user_ctx) {
    auto* self = static_cast<FlightDataSource*>(user_ctx);
    self->onUsbEvent(event);
}

void FlightDataSource::usbHostTask(void* arg) {
    auto* self = static_cast<FlightDataSource*>(arg);
    self->runUsbHostLoop();
}

// ======================== USB event handlers ========================

void FlightDataSource::onUsbData(const uint8_t* data, size_t data_len, uint64_t nowMs) {
    portENTER_CRITICAL_SAFE(&_parserMux);
    for (size_t i = 0; i < data_len; i++) {
        mavlink_parseByte(_parser, data[i], nowMs);
    }
    portEXIT_CRITICAL_SAFE(&_parserMux);

#if CONFIG_RID_VERBOSE_LOG
    if (data_len > 0) {
        ESP_LOGD(TAG, "USB RX: %zu bytes", data_len);
    }
#endif
}

void FlightDataSource::onUsbEvent(const cdc_acm_host_dev_event_data_t* event) {
    switch (event->type) {
        case CDC_ACM_HOST_ERROR:
            ESP_LOGE(TAG, "USB CDC-ACM error: %d", event->data.error);
            break;

        case CDC_ACM_HOST_DEVICE_DISCONNECTED:
            ESP_LOGW(TAG, "USB device disconnected");
            _cdcDev = nullptr;
            cdc_acm_host_close(event->data.cdc_hdl);
            portENTER_CRITICAL_SAFE(&_parserMux);
            mavlink_init(_parser);
            portEXIT_CRITICAL_SAFE(&_parserMux);
            _deviceConnected = false;
            _deviceReady = false;
            break;

        case CDC_ACM_HOST_SERIAL_STATE:
            ESP_LOGD(TAG, "Serial state: 0x%04X", event->data.serial_state.val);
            break;

        default:
            ESP_LOGD(TAG, "Unknown USB event: %d", event->type);
            break;
    }
}

// ======================== USB Host task ========================

void FlightDataSource::runUsbHostLoop() {
    ESP_LOGI(TAG, "USB Host task started");

    while (true) {
        uint32_t event_flags;
        esp_err_t ret = usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "USB Host event handling failed: %s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            ESP_LOGD(TAG, "No clients, cleaning up");
            usb_host_device_free_all();
        }

        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            ESP_LOGD(TAG, "All devices freed");
        }
    }
}

// ======================== USB device management ========================

void FlightDataSource::configureLineCoding(cdc_acm_dev_hdl_t dev) {
    cdc_acm_line_coding_t line_coding = {};
    esp_err_t ret = cdc_acm_host_line_coding_get(dev, &line_coding);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get line coding: %s", esp_err_to_name(ret));
        return;
    }

    line_coding.dwDTERate = FC_USB_BAUD_RATE;
    line_coding.bDataBits = FC_USB_DATA_BITS;
    line_coding.bParityType = FC_USB_PARITY;
    line_coding.bCharFormat = (FC_USB_STOP_BITS == 2) ? 2 : 0;

    ret = cdc_acm_host_line_coding_set(dev, &line_coding);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Line coding: %" PRIu32 " baud, %d%c%d",
                 line_coding.dwDTERate,
                 line_coding.bDataBits,
                 "NOE"[line_coding.bParityType],
                 line_coding.bCharFormat == 0 ? 1 : 2);
    } else {
        ESP_LOGW(TAG, "Failed to set line coding: %s", esp_err_to_name(ret));
    }
}

esp_err_t FlightDataSource::tryOpenUsbDevice() {
    if (_cdcDev != nullptr) {
        return ESP_OK;
    }

    cdc_acm_host_device_config_t dev_config = {};
    dev_config.connection_timeout_ms = 1000;
#if MAVLINK_TX_ENABLED
    dev_config.out_buffer_size = 512;
#else
    dev_config.out_buffer_size = 0;    // 只读模式 — 不从 USB 向飞控发送任何数据
#endif
    dev_config.in_buffer_size = 512;
    dev_config.user_arg = this;
    dev_config.event_cb = usbEventCb;
    dev_config.data_cb = usbDataCb;

#if FC_USB_VID != 0
    ESP_LOGI(TAG, "Trying to open USB device (VID=0x%04X, PID=0x%04X)",
             FC_USB_VID, FC_USB_PID);

    esp_err_t ret = cdc_acm_host_open(FC_USB_VID, FC_USB_PID, 0, &dev_config, &_cdcDev);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "USB device opened (VID=0x%04X, PID=0x%04X)",
                 FC_USB_VID, FC_USB_PID);
        cdc_acm_host_desc_print(_cdcDev);
        configureLineCoding(_cdcDev);
        // 显式清除 DTR/RTS 控制线状态
        // 飞控的 USB 口通常是烧录/配置口, DTR 可能连接到 MCU 的 BOOT0/NRST 引脚
        // 断言 DTR 可能触发飞控复位或进入 bootloader 模式，导致飞行中失控
        // 即使 driver 隐式设置了 DTR, 此处显式清除确保安全
        cdc_acm_host_set_control_line_state(_cdcDev, false, false);
        ESP_LOGI(TAG, "USB device configured (DTR/RTS cleared for FC safety) — waiting for MAVLink data...");
        _parser.consecutiveCrcErrors = 0;
        _deviceConnected = true;
        _deviceReady = true;
        return ESP_OK;
    } else if (ret == ESP_ERR_NOT_FOUND) {
        ESP_LOGD(TAG, "Specified device not found, will retry...");
        return ESP_ERR_NOT_FOUND;
    } else {
        ESP_LOGE(TAG, "Failed to open USB device: %s", esp_err_to_name(ret));
        return ret;
    }
#else
    ESP_LOGE(TAG, "FC_USB_VID is 0 — auto-detect mode not yet implemented; set VID/PID in config.h");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

// ======================== USB Host init ========================

void FlightDataSource::initUsbHost() {
    if (_usbHostInitialized) return;

    ESP_LOGI(TAG, "Initializing USB Host for flight controller data");

    usb_host_config_t host_config = {};
    host_config.skip_phy_setup = false;
    host_config.intr_flags = ESP_INTR_FLAG_LOWMED;

    esp_err_t ret = usb_host_install(&host_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "USB Host install failed: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "USB Host driver installed");

    BaseType_t task_created = xTaskCreate(
        usbHostTask,
        "usb_host",
        USB_HOST_TASK_STACK,
        this,
        USB_HOST_TASK_PRIO,
        &_usbHostTaskHandle
    );

    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create USB Host task");
        usb_host_uninstall();
        return;
    }

    ret = cdc_acm_host_install(NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "CDC-ACM host install failed: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "CDC-ACM host driver installed");

    mavlink_init(_parser);

    _usbHostInitialized = true;
    ESP_LOGI(TAG, "USB Host initialized. Waiting for flight controller connection...");
}

// ======================== USB recovery ========================

bool FlightDataSource::tryUsbRecovery(uint64_t nowMs) {
    if (nowMs - _lastRecoveryMs < USB_RECOVERY_COOLDOWN_MS) {
        return false;
    }
    _lastRecoveryMs = nowMs;
    _recoveryCount++;

    ESP_LOGW(TAG, "USB recovery #%lu triggered — %lu consecutive CRC errors",
             (unsigned long)_recoveryCount,
             (unsigned long)_parser.consecutiveCrcErrors);

    cdc_acm_dev_hdl_t dev_to_close = _cdcDev;
    _cdcDev = nullptr;
    if (dev_to_close != nullptr) {
        esp_err_t ret = cdc_acm_host_close(dev_to_close);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "CDC-ACM device closed for recovery");
        } else {
            ESP_LOGW(TAG, "CDC-ACM close returned: %s", esp_err_to_name(ret));
        }
    }

    portENTER_CRITICAL_SAFE(&_parserMux);
    mavlink_init(_parser);
    portEXIT_CRITICAL_SAFE(&_parserMux);
    ESP_LOGI(TAG, "MAVLink parser reset");

    _deviceConnected = false;
    _deviceReady = false;

    ESP_LOGI(TAG, "Recovery complete — will attempt reconnect on next cycle");
    return true;
}

// ======================== FlightDataSource public methods ========================

void FlightDataSource::getFlightData(FlightData& fd, uint64_t nowMs) {
    if (!_usbHostInitialized) {
        initUsbHost();
    }

    if (_usbHostInitialized && !_deviceReady) {
        static uint64_t lastRetryMs = 0;
        if (nowMs - lastRetryMs > 2000) {
            lastRetryMs = nowMs;
            tryOpenUsbDevice();
        }
    }

    // --- Critical section: read _parser state atomically ---
    portENTER_CRITICAL_SAFE(&_parserMux);
    bool needsRecovery = _deviceReady && mavlink_needsRecovery(_parser, nowMs, MAVLINK_CONSECUTIVE_CRC_LIMIT);
    bool isStale = mavlink_isDataStale(_parser, nowMs, FC_DATA_TIMEOUT_MS);
    bool hasData = mavlink_fillFlightData(_parser, fd, nowMs);
    portEXIT_CRITICAL_SAFE(&_parserMux);

    if (needsRecovery) {
        ESP_LOGW(TAG, "CRC storm detected — triggering USB recovery");
        tryUsbRecovery(nowMs);
    }

    if (isStale) {
        fd.freshness = FRESH_STALE;

#if CONFIG_RID_VERBOSE_LOG
        if (_deviceReady) {
            portENTER_CRITICAL_SAFE(&_parserMux);
            uint64_t lastPos = _parser.lastPositionMs;
            uint64_t lastGps = _parser.lastGpsMs;
            portEXIT_CRITICAL_SAFE(&_parserMux);
            ESP_LOGW(TAG, "Data STALE: lastPos=%llu lastGps=%llu",
                     (unsigned long long)lastPos, (unsigned long long)lastGps);
        }
#endif
    }

    if (!hasData) {
        fd.lat = 0;
        fd.lon = 0;
        fd.geoAlt = 0;
        fd.baroAlt = 0;
        fd.heightAgl = 0;
        fd.speed = 0;
        fd.heading = 0;
        fd.vspeed = 0;
        // 不覆盖 opStatus — 保留上次已知状态
        // fd 在 main.cpp 中零初始化 (opStatus=STATUS_GROUND)
        // 飞行中数据短暂丢失时，保留上次 fillFlightData 设置的空中状态
        fd.opLat = 0.0f;
        fd.opLon = 0.0f;
        fd.opAlt = 0.0f;
        fd.validMask = 0;
        fd.freshness = FRESH_INVALID;
        fd.validationFlags = 0xFFFFFFFF;

#if CONFIG_RID_VERBOSE_LOG
        if (_deviceReady) {
            ESP_LOGW(TAG, "No valid data: fix=%d armed=%d",
                     _parser.gpsFixType, _parser.armed);
        }
#endif
    }

    static uint64_t lastStatusMs = 0;
    if (nowMs - lastStatusMs > 5000) {
        lastStatusMs = nowMs;

        if (!_deviceReady) {
            ESP_LOGW(TAG, "USB device not connected, waiting...");
        }

        char statusBuf[200];
        portENTER_CRITICAL_SAFE(&_parserMux);
        mavlink_getStatus(_parser, statusBuf, sizeof(statusBuf));
        uint32_t crcErrs = _parser.consecutiveCrcErrors;
        portEXIT_CRITICAL_SAFE(&_parserMux);
        ESP_LOGI(TAG, "%s crc_storm=%lu recovery=%lu",
                 statusBuf, (unsigned long)crcErrs,
                 (unsigned long)_recoveryCount);
    }
}

bool FlightDataSource::sendToFC(const uint8_t* data, size_t len) {
    if (!data || len == 0) return false;
    if (!_cdcDev || !_deviceReady) {
        ESP_LOGE(TAG, "TX: CDC device not ready");
        return false;
    }

    esp_err_t ret = cdc_acm_host_data_tx_blocking(_cdcDev, data, len, 100);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "TX to FC failed: %s", esp_err_to_name(ret));
        return false;
    }
    ESP_LOGI(TAG, "TX: %u bytes sent to FC", (unsigned)len);
    return true;
}

bool FlightDataSource::sendArmDisarm(bool arm) {
    uint8_t frame[64];
    uint16_t frameLen = mavlink_build_arm_disarm(frame, sizeof(frame), 255, 190, arm, false);
    if (frameLen == 0) {
        ESP_LOGE(TAG, "Failed to build arm/disarm frame");
        return false;
    }
    ESP_LOGI(TAG, "Sending MAVLink %s command to FC", arm ? "ARM" : "DISARM");
    return sendToFC(frame, frameLen);
}
