#include "flight_data.h"
#include "mavlink_parser.h"
#include "config.h"
#include "fault_log.h"
#include <inttypes.h>
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_task_wdt.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "usb/usb_host.h"
#include "usb/cdc_acm_host.h"

static const char* TAG = "FLIGHT_DATA";

// ======================== FlightDataSource ========================
// Encapsulates USB host + MAVLink parser state.
// USB callbacks use static trampolines with void* user_arg → this.

class FlightDataSource {
public:
    FlightDataSource();
    void getFlightData(FlightData& fd, uint64_t nowMs);

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
    // volatile: 跨任务共享 (主循环 <-> USB host 任务), 防止编译器寄存器缓存导致读到过期状态
    volatile cdc_acm_dev_hdl_t _cdcDev = nullptr;
    volatile bool       _deviceConnected = false;
    volatile bool       _deviceReady = false;
    uint32_t           _recoveryCount = 0;
    uint64_t           _lastRecoveryMs = 0;

    // 设备生命周期互斥锁: 串行化 _cdcDev 句柄的取用/关闭。
    // 竞态来源: tryUsbRecovery (主循环任务) 与 DISCONNECT 事件回调 (USB host 任务)
    // 可能同时 close 同一句柄 → 双重 close / use-after-free。
    // 加锁后同一句柄只被关闭一次, 取用/关闭期间句柄不被释放。
    SemaphoreHandle_t  _devMutex = nullptr;
};

// Single module-level instance — public free functions delegate to it.
static FlightDataSource s_source;

// 静态单例构造: 创建设备生命周期互斥锁。
// FreeRTOS 堆是静态数组, 调度器启动前 pvPortMalloc 可用, 此处创建安全。
FlightDataSource::FlightDataSource() {
    _devMutex = xSemaphoreCreateMutex();
}

// ======================== Public free-function API ========================

void getFlightData(FlightData& fd, uint64_t nowMs) {
    s_source.getFlightData(fd, nowMs);
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
    // 注册任务看门狗: USB host 栈死锁时系统复位而非静默断流
    // (GB 42590-2023 A.2.3.5.5 运行自检)
    if (esp_task_wdt_add(NULL) != ESP_OK) {
        ESP_LOGW(TAG, "usb_host task not subscribed to TWDT");
    }
    self->runUsbHostLoop();
}

// ======================== USB event handlers ========================

void FlightDataSource::onUsbData(const uint8_t* data, size_t data_len, uint64_t nowMs) {
    portENTER_CRITICAL_SAFE(&_parserMux);
    for (size_t i = 0; i < data_len; i++) {
        mavlink_parseByte(_parser, data[i], nowMs);
    }
    // 锁内只读暂存的 CRC 失败详情并清零; 打印 (ESP_LOGW 内部取锁) 移到锁外
    uint8_t  crcFailPending = _parser.crcFailPending;
    bool     crcFailIsV2    = _parser.crcFailIsV2;
    uint32_t crcFailMsgId   = _parser.crcFailMsgId;
    uint16_t crcFailRecv    = _parser.crcFailRecv;
    uint16_t crcFailCalc    = _parser.crcFailCalc;
    _parser.crcFailPending  = 0;
    portEXIT_CRITICAL_SAFE(&_parserMux);

#if CONFIG_RID_VERBOSE_LOG
    if (crcFailPending) {
        ESP_LOGW(TAG, "CRC fail: %s msgid=%lu recv=0x%04X calc=0x%04X",
                 crcFailIsV2 ? "v2" : "v1",
                 (unsigned long)crcFailMsgId, crcFailRecv, crcFailCalc);
    }
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

        case CDC_ACM_HOST_DEVICE_DISCONNECTED: {
            ESP_LOGW(TAG, "USB device disconnected");
            // 与 tryUsbRecovery 互斥: 若恢复流程已先关闭并置空句柄, 此处不再重复 close
            cdc_acm_dev_hdl_t dev_to_close = nullptr;
            if (_devMutex) xSemaphoreTake(_devMutex, portMAX_DELAY);
            if (_cdcDev != nullptr) {
                dev_to_close = _cdcDev;
                _cdcDev = nullptr;
            }
            if (_devMutex) xSemaphoreGive(_devMutex);
            if (dev_to_close != nullptr) {
                cdc_acm_host_close(dev_to_close);
            }
            portENTER_CRITICAL_SAFE(&_parserMux);
            mavlink_init(_parser);
            portEXIT_CRITICAL_SAFE(&_parserMux);
            _deviceConnected = false;
            _deviceReady = false;
            break;
        }

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
        // 有限超时而非 portMAX_DELAY: 空闲时也要定期喂狗。
        // 20ms 轮询远小于 CDC 输入环形缓冲(512B @115200baud≈44ms 填满),
        // 不会造成数据堆积; 事件到达时 handle_events 会立即唤醒, 无额外延迟。
        uint32_t event_flags;
        esp_err_t ret = usb_host_lib_handle_events(pdMS_TO_TICKS(20), &event_flags);
        if (ret != ESP_OK) {
            if (ret != ESP_ERR_TIMEOUT) {
                ESP_LOGE(TAG, "USB Host event handling failed: %s", esp_err_to_name(ret));
            }
            esp_task_wdt_reset();
            continue;
        }

        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            ESP_LOGD(TAG, "No clients, cleaning up");
            usb_host_device_free_all();
        }

        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            ESP_LOGD(TAG, "All devices freed");
        }

        esp_task_wdt_reset();
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
    // 只读模式 — 不从 USB 向飞控发送任何数据。out_buffer_size=0 在 CDC-ACM
    // 驱动层禁止 TX, 从物理层面杜绝任何数据反向注入飞控 (联锁移除后无 TX 需求)。
    dev_config.out_buffer_size = 0;
    dev_config.in_buffer_size = 512;
    dev_config.user_arg = this;
    dev_config.event_cb = usbEventCb;
    dev_config.data_cb = usbDataCb;

#if FC_USB_VID != 0
    ESP_LOGI(TAG, "Trying to open USB device (VID=0x%04X, PID=0x%04X)",
             FC_USB_VID, FC_USB_PID);

    // 用局部变量接收 open 结果 (不能对 volatile _cdcDev 取地址)
    cdc_acm_dev_hdl_t newDev = nullptr;
    esp_err_t ret = cdc_acm_host_open(FC_USB_VID, FC_USB_PID, 0, &dev_config, &newDev);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "USB device opened (VID=0x%04X, PID=0x%04X)",
                 FC_USB_VID, FC_USB_PID);
        cdc_acm_host_desc_print(newDev);
        configureLineCoding(newDev);
        // 显式清除 DTR/RTS 控制线状态
        // 飞控的 USB 口通常是烧录/配置口, DTR 可能连接到 MCU 的 BOOT0/NRST 引脚
        // 断言 DTR 可能触发飞控复位或进入 bootloader 模式，导致飞行中失控
        // 即使 driver 隐式设置了 DTR, 此处显式清除确保安全
        cdc_acm_host_set_control_line_state(newDev, false, false);
        ESP_LOGI(TAG, "USB device configured (DTR/RTS cleared for FC safety) — waiting for MAVLink data...");
        // 配置完成后才发布句柄 (互斥锁内), 避免与 DISCONNECT/recovery 竞态
        if (_devMutex) xSemaphoreTake(_devMutex, portMAX_DELAY);
        _cdcDev = newDev;
        _deviceConnected = true;
        _deviceReady = true;
        if (_devMutex) xSemaphoreGive(_devMutex);
        _parser.consecutiveCrcErrors = 0;
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
    faultLogRecord(FAULT_CRC_STORM, nowMs);

    ESP_LOGW(TAG, "USB recovery #%lu triggered — %lu consecutive CRC errors",
             (unsigned long)_recoveryCount,
             (unsigned long)_parser.consecutiveCrcErrors);

    // 与 DISCONNECT 事件互斥: 同一句柄只关闭一次, 避免双重 close / use-after-free
    cdc_acm_dev_hdl_t dev_to_close = nullptr;
    if (_devMutex) xSemaphoreTake(_devMutex, portMAX_DELAY);
    if (_cdcDev != nullptr) {
        dev_to_close = _cdcDev;
        _cdcDev = nullptr;
    }
    if (_devMutex) xSemaphoreGive(_devMutex);
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

    // --- Critical section: snapshot parser state atomically ---
    // 锁内只做结构体快照拷贝 (~400B memcpy, 微秒级)。浮点运算 (mavlink_fillFlightData
    // 内含 sqrtf/atan2f)、snprintf (mavlink_getStatus)、ESP_LOG 全部移到锁外:
    // ESP32-S3 双核, portENTER_CRITICAL 关本核中断并自旋等待另一核, 锁内做耗时/
    // 取锁操作 (ESP_LOG 内部也取锁) 有死锁与调度抖动风险。
    MavlinkParser snap;
    portENTER_CRITICAL_SAFE(&_parserMux);
    snap = _parser;
    portEXIT_CRITICAL_SAFE(&_parserMux);

    bool needsRecovery = _deviceReady && mavlink_needsRecovery(snap, nowMs, MAVLINK_CONSECUTIVE_CRC_LIMIT);
    bool isStale = mavlink_isDataStale(snap, nowMs, FC_DATA_TIMEOUT_MS);
    bool hasData = mavlink_fillFlightData(snap, fd, nowMs);

    if (needsRecovery) {
        ESP_LOGW(TAG, "CRC storm detected — triggering USB recovery");
        tryUsbRecovery(nowMs);
    }

    if (isStale) {
        fd.freshness = FRESH_STALE;

#if CONFIG_RID_VERBOSE_LOG
        if (_deviceReady) {
            ESP_LOGW(TAG, "Data STALE: lastPos=%llu lastGps=%llu",
                     (unsigned long long)snap.lastPositionMs,
                     (unsigned long long)snap.lastGpsMs);
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
                     snap.gpsFixType, snap.armed);
        }
#endif
    }

    static uint64_t lastStatusMs = 0;
    if (nowMs - lastStatusMs > 5000) {
        lastStatusMs = nowMs;

        if (!_deviceReady) {
            ESP_LOGW(TAG, "USB device not connected, waiting...");
        }

        // snprintf (mavlink_getStatus) 在锁外 — 锁内已通过 snap 取得一致快照
        char statusBuf[200];
        mavlink_getStatus(snap, statusBuf, sizeof(statusBuf));
        ESP_LOGI(TAG, "%s crc_storm=%lu recovery=%lu",
                 statusBuf, (unsigned long)snap.consecutiveCrcErrors,
                 (unsigned long)_recoveryCount);
    }
}
