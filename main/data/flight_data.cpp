#include "flight_data.h"
#include "mavlink_parser.h"
#include "config.h"
#include <inttypes.h>
#include <math.h>
#include "esp_timer.h"
#include "esp_log.h"

// USB Host 头文件 (usb_host_cdc_acm v2.x)
#include "usb/usb_host.h"
#include "usb/cdc_acm_host.h"

static const char* TAG = "FLIGHT_DATA";

// ================= MAVLink 解析任务 =================

static MavlinkParser s_parser;
static bool s_usbHostInitialized = false;
static TaskHandle_t s_usbHostTaskHandle = nullptr;
static cdc_acm_dev_hdl_t s_cdc_dev = nullptr;

// USB 设备连接状态
static volatile bool s_deviceConnected = false;
static volatile bool s_deviceReady = false;

// ================= USB Host 回调函数 =================

// USB CDC-ACM 数据接收回调 (v2 API: 返回 bool)
// 当从飞控收到数据时调用，将数据送入 MAVLink 解析器
static bool usb_data_callback(const uint8_t* data, size_t data_len, void* arg) {
    if (data == nullptr || data_len == 0) return true;

    uint64_t nowMs = esp_timer_get_time() / 1000;

    // 逐字节送入 MAVLink 解析器
    for (size_t i = 0; i < data_len; i++) {
        mavlink_parseByte(s_parser, data[i], nowMs);
    }

    #if CONFIG_RID_VERBOSE_LOG
    if (data_len > 0) {
        ESP_LOGD(TAG, "USB RX: %zu bytes", data_len);
    }
    #endif

    return true;
}

// USB CDC-ACM 事件回调 (v2 API)
// 处理设备连接/断开/错误等事件
static void usb_event_callback(const cdc_acm_host_dev_event_data_t* event, void* user_ctx) {
    switch (event->type) {
        case CDC_ACM_HOST_ERROR:
            ESP_LOGE(TAG, "USB CDC-ACM error: %d", event->data.error);
            break;

        case CDC_ACM_HOST_DEVICE_DISCONNECTED:
            ESP_LOGW(TAG, "USB device disconnected");
            cdc_acm_host_close(event->data.cdc_hdl);
            s_deviceConnected = false;
            s_deviceReady = false;
            s_cdc_dev = nullptr;
            break;

        case CDC_ACM_HOST_SERIAL_STATE:
            // 串口状态变化 (DTR, RTS, 等)
            ESP_LOGD(TAG, "Serial state: 0x%04X", event->data.serial_state.val);
            break;

        default:
            ESP_LOGD(TAG, "Unknown USB event: %d", event->type);
            break;
    }
}

// USB Host 事件处理任务
// 持续处理 USB Host 库事件
static void usb_host_task(void* arg) {
    ESP_LOGI(TAG, "USB Host task started");

    while (true) {
        // 处理 USB Host 事件
        uint32_t event_flags;
        esp_err_t ret = usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "USB Host event handling failed: %s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // 检查是否需要释放设备
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            ESP_LOGD(TAG, "No clients, cleaning up");
            usb_host_device_free_all();
        }

        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            ESP_LOGD(TAG, "All devices freed");
        }
    }
}

// 配置串口参数 (波特率、数据位、校验位、停止位)
static void configure_line_coding(cdc_acm_dev_hdl_t dev) {
    cdc_acm_line_coding_t line_coding = {};
    esp_err_t ret = cdc_acm_host_line_coding_get(dev, &line_coding);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get line coding: %s", esp_err_to_name(ret));
        return;
    }

    line_coding.dwDTERate = FC_USB_BAUD_RATE;
    line_coding.bDataBits = FC_USB_DATA_BITS;
    line_coding.bParityType = FC_USB_PARITY;   // 0=None, 1=Odd, 2=Even
    line_coding.bCharFormat = (FC_USB_STOP_BITS == 2) ? 2 : 0;  // 0=1stop, 2=2stop

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

// 打开 USB 设备 (v2 API)
static esp_err_t try_open_usb_device() {
    if (s_cdc_dev != nullptr) {
        return ESP_OK;  // 已打开
    }

    // v2 设备配置 (不再包含波特率等参数, 通过 line_coding 设置)
    cdc_acm_host_device_config_t dev_config = {};
    dev_config.connection_timeout_ms = 1000;
    dev_config.out_buffer_size = 512;
    dev_config.in_buffer_size = 512;
    dev_config.user_arg = nullptr;
    dev_config.event_cb = usb_event_callback;
    dev_config.data_cb = usb_data_callback;

    #if FC_USB_VID != 0
    // 模式 1: 指定 VID/PID
    ESP_LOGI(TAG, "Trying to open USB device (VID=0x%04X, PID=0x%04X)",
             FC_USB_VID, FC_USB_PID);

    esp_err_t ret = cdc_acm_host_open(FC_USB_VID, FC_USB_PID, 0, &dev_config, &s_cdc_dev);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "✓ USB device opened (VID=0x%04X, PID=0x%04X)",
                 FC_USB_VID, FC_USB_PID);
        cdc_acm_host_desc_print(s_cdc_dev);
        configure_line_coding(s_cdc_dev);
        // 置位 DTR 告知飞控 Host 已就绪，飞控才会开始输出 MAVLink 数据
        cdc_acm_host_set_control_line_state(s_cdc_dev, true, false);
        ESP_LOGI(TAG, "DTR asserted — waiting for MAVLink data...");
        s_deviceConnected = true;
        s_deviceReady = true;
        return ESP_OK;
    } else if (ret == ESP_ERR_NOT_FOUND) {
        ESP_LOGD(TAG, "Specified device not found, will retry...");
        return ESP_ERR_NOT_FOUND;
    } else {
        ESP_LOGE(TAG, "Failed to open USB device: %s", esp_err_to_name(ret));
        return ret;
    }

    #else
    // 模式 2: 自动检测任意 CDC-ACM 设备 (即插即用)
    // 注意: 自动检测模式需要 USB Host client API，与指定 VID/PID 模式不同。
    // 当前请使用指定 VID/PID 模式 (在 config.h 中设置 FC_USB_VID)。
    #error "Auto-detect mode (FC_USB_VID=0) requires USB Host client API. Specify your FC VID/PID in config.h."
    #endif
}

// ================= 初始化 =================

static void init_usb_host() {
    if (s_usbHostInitialized) return;

    ESP_LOGI(TAG, "Initializing USB Host for flight controller data");

    // 1. 安装 USB Host 驱动
    usb_host_config_t host_config = {};
    host_config.skip_phy_setup = false;
    host_config.intr_flags = ESP_INTR_FLAG_LOWMED;

    esp_err_t ret = usb_host_install(&host_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "USB Host install failed: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "USB Host driver installed");

    // 2. 创建 USB Host 事件处理任务
    BaseType_t task_created = xTaskCreate(
        usb_host_task,
        "usb_host",
        USB_HOST_TASK_STACK,
        NULL,
        USB_HOST_TASK_PRIO,
        &s_usbHostTaskHandle
    );

    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create USB Host task");
        usb_host_uninstall();
        return;
    }

    // 3. 安装 CDC-ACM 驱动
    ret = cdc_acm_host_install(NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "CDC-ACM host install failed: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "CDC-ACM host driver installed");

    // 4. 初始化 MAVLink 解析器
    mavlink_init(s_parser);

    s_usbHostInitialized = true;
    ESP_LOGI(TAG, "USB Host initialized. Waiting for flight controller connection...");
}

// ================= 公共接口 =================

void getFlightData(FlightData& fd, uint64_t nowMs) {
    // 首次调用时初始化 USB Host
    if (!s_usbHostInitialized) {
        init_usb_host();
    }

    // 如果 USB 已初始化但设备未连接，尝试连接
    if (s_usbHostInitialized && !s_deviceReady) {
        // 每 2 秒尝试一次连接
        static uint64_t lastRetryMs = 0;
        if (nowMs - lastRetryMs > 2000) {
            lastRetryMs = nowMs;
            try_open_usb_device();
        }
    }

    // 检查是否有新数据
    if (mavlink_isDataStale(s_parser, nowMs, FC_DATA_TIMEOUT_MS)) {
        // 数据超时, 标记为 STALE
        fd.freshness = FRESH_STALE;

        #if CONFIG_RID_VERBOSE_LOG
        if (s_deviceReady) {
            ESP_LOGW(TAG, "Data STALE: lastPos=%llu lastGps=%llu",
                     (unsigned long long)s_parser.lastPositionMs,
                     (unsigned long long)s_parser.lastGpsMs);
        }
        #endif
    }

    // 尝试从 MAVLink 解析器填充数据
    bool hasData = mavlink_fillFlightData(s_parser, fd, nowMs);

    if (!hasData) {
        // 无有效数据 (GPS 未锁定等), 使用默认值
        fd.lat = 0;
        fd.lon = 0;
        fd.geoAlt = 0;
        fd.baroAlt = 0;
        fd.heightAgl = 0;
        fd.speed = 0;
        fd.heading = 0;
        fd.vspeed = 0;
        fd.opStatus = STATUS_GROUND;
        fd.opLat = MOCK_OP_LAT;
        fd.opLon = MOCK_OP_LON;
        fd.opAlt = MOCK_OP_ALT;
        fd.validMask = 0;  // 所有字段无效
        fd.freshness = FRESH_INVALID;
        fd.validationFlags = 0xFFFFFFFF;

        #if CONFIG_RID_VERBOSE_LOG
        if (s_deviceReady) {
            ESP_LOGW(TAG, "No valid data: fix=%d armed=%d",
                     s_parser.gpsFixType, s_parser.armed);
        }
        #endif
    }

    // 定期输出状态 (每 5 秒)
    static uint64_t lastStatusMs = 0;
    if (nowMs - lastStatusMs > 5000) {
        lastStatusMs = nowMs;

        // 输出 USB 连接状态
        if (!s_deviceReady) {
            ESP_LOGW(TAG, "USB device not connected, waiting...");
        }

        // 输出 MAVLink 解析状态
        char statusBuf[200];
        mavlink_getStatus(s_parser, statusBuf, sizeof(statusBuf));
        ESP_LOGI(TAG, "%s", statusBuf);
    }
}
