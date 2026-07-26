#include "flight_log.h"
#include "config.h"
#include <string.h>
#include <esp_log.h>
#include <esp_partition.h>
#include <esp_timer.h>

static const char* TAG = "FLOG";

// Record layout (96 bytes):
//   [0..3]   Magic "RIDL" LE
//   [4..5]   CRC16 LE  (over bytes 6..95)
//   [6..13]  Timestamp uint64 LE (ms)
//   [14..15] DataLen  uint16 LE
//   [16..95] Payload (80 bytes, zero-padded)

FlightLog::~FlightLog() {
    if (_taskHandle) {
        vTaskDelete(_taskHandle);
        _taskHandle = nullptr;
    }
    if (_queue) {
        vQueueDelete(_queue);
        _queue = nullptr;
    }
}

bool FlightLog::init() {
    const esp_partition_t* part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, FLIGHT_LOG_PARTITION);

    if (!part) {
        ESP_LOGE(TAG, "Partition '%s' not found — check partitions.csv", FLIGHT_LOG_PARTITION);
        return false;
    }

    esp_err_t rc = wl_mount(part, &_wlHandle);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "wl_mount failed: %d", rc);
        return false;
    }

    _partitionSize = wl_size(_wlHandle);

    // 扫描分区找到最后一条有效记录，确定写入位置
    uint32_t offset = 0;
    uint32_t count  = 0;
    uint8_t  buf[kRecordSize];

    while (offset + kRecordSize <= _partitionSize) {
        rc = wl_read(_wlHandle, offset, buf, kRecordSize);
        if (rc != ESP_OK) break;

        uint32_t magic = (uint32_t)buf[0]
                       | ((uint32_t)buf[1] << 8)
                       | ((uint32_t)buf[2] << 16)
                       | ((uint32_t)buf[3] << 24);

        if (magic == 0xFFFFFFFF) break;  // 擦除态

        if (magic != kMagic) break;       // 未知数据

        uint16_t storedCrc = (uint16_t)buf[4] | ((uint16_t)buf[5] << 8);
        uint16_t calcCrc  = crc16(buf + 6, kRecordSize - 6);
        if (storedCrc != calcCrc) {
            ESP_LOGW(TAG, "CRC mismatch at offset %lu — resuming write here", (unsigned long)offset);
            break;
        }

        count++;
        offset += kRecordSize;
    }

    _recordCount = count;
    _writeOffset = offset;

    float maxHours = (float)(_partitionSize / kRecordSize)
                     * (float)FLIGHT_LOG_INTERVAL_S / 3600.0f;

    printf("[FLOG] Partition '%s': %lu KB, %lu valid records, next offset=%lu\n",
           FLIGHT_LOG_PARTITION,
           (unsigned long)(_partitionSize / 1024),
           (unsigned long)count,
           (unsigned long)offset);
    printf("[FLOG] Capacity: ~%.0f flight hours at %ds interval\n",
           maxHours, FLIGHT_LOG_INTERVAL_S);

    if (maxHours < 120.0f) {
        printf("[FLOG] WARNING: Capacity < 120h — use larger flash chip or partition\n");
    }

    // 创建异步写入队列
    _queue = xQueueCreate(FLIGHT_LOG_QUEUE_DEPTH, sizeof(LogItem));
    if (!_queue) {
        ESP_LOGE(TAG, "Failed to create log queue");
        return false;
    }

    BaseType_t ret = xTaskCreate(logTaskFunc, "flight_log",
                                  FLIGHT_LOG_TASK_STACK, this,
                                  FLIGHT_LOG_TASK_PRIO, &_taskHandle);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create log task");
        vQueueDelete(_queue);
        _queue = nullptr;
        return false;
    }

    ESP_LOGI(TAG, "Flight log initialized (async, wear-leveled)");
    return true;
}

bool FlightLog::enqueueRecord(const uint8_t* data, uint16_t len, uint64_t timestampMs) {
    if (!_queue || len > kMaxDataLen) return false;

    LogItem item;
    item.len = len;
    item.timestampMs = timestampMs;
    memset(item.data, 0, sizeof(item.data));
    memcpy(item.data, data, len);

    if (xQueueSend(_queue, &item, pdMS_TO_TICKS(FLIGHT_LOG_WRITE_TIMEOUT_MS)) != pdTRUE) {
        // 队列满 — 丢弃最旧一条, 覆盖写入 (非阻塞广播)
        LogItem discard;
        if (xQueueReceive(_queue, &discard, 0) == pdTRUE) {
            xQueueSend(_queue, &item, 0);
        }
        ESP_LOGW(TAG, "Queue full — dropped oldest record");
        return false;
    }
    return true;
}

void FlightLog::logTaskFunc(void* param) {
    FlightLog* self = static_cast<FlightLog*>(param);
    self->logTaskLoop();
}

void FlightLog::logTaskLoop() {
    LogItem item;
    while (1) {
        if (xQueueReceive(_queue, &item, portMAX_DELAY) == pdTRUE) {
            writeRecord(item.data, item.len, item.timestampMs);
        }
    }
}

uint16_t FlightLog::writeRecord(const uint8_t* data, uint16_t len, uint64_t timestampMs) {
    if (_wlHandle == WL_INVALID_HANDLE || len > kMaxDataLen) return 0;

    if (_writeOffset + kRecordSize > _partitionSize) {
        _writeOffset = 0;
        ESP_LOGI(TAG, "Log wrap — overwriting oldest records");
    }

    uint8_t record[kRecordSize];
    memset(record, 0, sizeof(record));

    record[0] = (kMagic >>  0) & 0xFF;
    record[1] = (kMagic >>  8) & 0xFF;
    record[2] = (kMagic >> 16) & 0xFF;
    record[3] = (kMagic >> 24) & 0xFF;

    for (int i = 0; i < 8; i++) {
        record[6 + i] = (timestampMs >> (i * 8)) & 0xFF;
    }
    record[14] = len & 0xFF;
    record[15] = (len >> 8) & 0xFF;
    memcpy(record + 16, data, len);

    uint16_t crc = crc16(record + 6, kRecordSize - 6);
    record[4] = crc & 0xFF;
    record[5] = (crc >> 8) & 0xFF;

    esp_err_t rc = wl_erase_range(_wlHandle, _writeOffset, kRecordSize);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "wl_erase_range failed at %lu: %d", (unsigned long)_writeOffset, rc);
        return 0;
    }

    rc = wl_write(_wlHandle, _writeOffset, record, kRecordSize);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "wl_write failed at %lu: %d", (unsigned long)_writeOffset, rc);
        return 0;
    }

    _writeOffset += kRecordSize;
    _recordCount++;
    _lastWriteMs = timestampMs;

    return kRecordSize;
}

float FlightLog::estimateRemainingHours() const {
    if (_wlHandle == WL_INVALID_HANDLE) return 0.0f;

    uint32_t remainingBytes = _partitionSize - _writeOffset;
    uint32_t remainingRecords = remainingBytes / kRecordSize;
    return (float)remainingRecords * (float)FLIGHT_LOG_INTERVAL_S / 3600.0f;
}

uint16_t FlightLog::crc16(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++) {
            if (crc & 0x8000) {
                crc = (crc << 1) ^ 0x1021;
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}
