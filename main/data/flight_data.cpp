#include "flight_data.h"
#include "mavlink_parser.h"
#include "config.h"
#include <math.h>
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char* TAG = "FLIGHT_DATA";

// ================= MAVLink 解析任务 =================

static MavlinkParser s_parser;
static bool s_uartInitialized = false;
static TaskHandle_t s_parseTaskHandle = nullptr;

// UART 接收任务: 持续读取字节并送入 MAVLink 解析器
static void uart_rx_task(void* arg) {
    uint8_t rxbuf[256];

    ESP_LOGI(TAG, "UART%d RX task started (baud=%d, rx_gpio=%d)",
             FC_UART_PORT_NUM, FC_UART_BAUD_RATE, FC_UART_RX_GPIO);

    while (true) {
        int len = uart_read_bytes((uart_port_t)FC_UART_PORT_NUM, rxbuf, sizeof(rxbuf), pdMS_TO_TICKS(100));
        if (len > 0) {
            uint64_t nowMs = esp_timer_get_time() / 1000;
            for (int i = 0; i < len; i++) {
                mavlink_parseByte(s_parser, rxbuf[i], nowMs);
            }
        }
    }
}

// ================= 初始化 =================

static void init_uart() {
    if (s_uartInitialized) return;

    uart_port_t uart_num = (uart_port_t)FC_UART_PORT_NUM;
    int tx_gpio = (FC_UART_TX_GPIO >= 0) ? FC_UART_TX_GPIO : UART_PIN_NO_CHANGE;

    uart_config_t uart_config = {
        .baud_rate = FC_UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    // 安装 UART 驱动
    ESP_ERROR_CHECK(uart_driver_install(uart_num,
                                         FC_UART_RX_BUF_SIZE,
                                         FC_UART_TX_BUF_SIZE,
                                         0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(uart_num, &uart_config));

    // 设置引脚 (TX 不使用, 设为 UART_PIN_NO_CHANGE)
    ESP_ERROR_CHECK(uart_set_pin(uart_num,
                                  tx_gpio,              // TX (不连接)
                                  FC_UART_RX_GPIO,      // RX (接飞控 TELEM1 TX)
                                  UART_PIN_NO_CHANGE,   // RTS
                                  UART_PIN_NO_CHANGE)); // CTS

    // 初始化 MAVLink 解析器
    mavlink_init(s_parser);

    // 创建接收任务
    xTaskCreate(uart_rx_task, "mav_rx", MAVLINK_PARSER_STACK, NULL, 10, &s_parseTaskHandle);

    s_uartInitialized = true;
    ESP_LOGI(TAG, "UART%d initialized: baud=%d rx_gpio=%d",
             FC_UART_PORT_NUM, FC_UART_BAUD_RATE, FC_UART_RX_GPIO);
}

// ================= 公共接口 =================

void getFlightData(FlightData& fd, uint64_t nowMs) {
    // 首次调用时初始化 UART 和解析任务
    if (!s_uartInitialized) {
        init_uart();
    }

    // 检查是否有新数据
    if (mavlink_isDataStale(s_parser, nowMs, FC_DATA_TIMEOUT_MS)) {
        // 数据超时, 标记为 STALE
        fd.freshness = FRESH_STALE;

        // 仍然尝试填充数据 (可能没有新数据, 但保持上一次的值)
        #if CONFIG_RID_VERBOSE_LOG
        ESP_LOGW(TAG, "Data STALE: lastPos=%llu lastGps=%llu",
                 (unsigned long long)s_parser.lastPositionMs,
                 (unsigned long long)s_parser.lastGpsMs);
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
        ESP_LOGW(TAG, "No valid data: fix=%d armed=%d",
                 s_parser.gpsFixType, s_parser.armed);
        #endif
    }

    // 定期输出状态 (每 5 秒)
    static uint64_t lastStatusMs = 0;
    if (nowMs - lastStatusMs > 5000) {
        lastStatusMs = nowMs;
        char statusBuf[200];
        mavlink_getStatus(s_parser, statusBuf, sizeof(statusBuf));
        ESP_LOGI(TAG, "%s", statusBuf);
    }
}
