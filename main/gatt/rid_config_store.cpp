#include "rid_config_store.h"
#include "config.h"

#include <string.h>
#include <esp_log.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

static const char* TAG = "RID_CFG";

// NVS namespace + keys
static const char* NS       = "rid_cfg";
static const char* KEY_UAS  = "uas_id";
static const char* KEY_RN   = "realname";
static const char* KEY_OPC  = "op_cat";
static const char* KEY_UAC  = "ua_class";

bool RidConfigStore::init() {
    _mutex = xSemaphoreCreateMutex();
    if (!_mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return false;
    }

    // 占位默认值 (config.h) — 首次启动无 NVS 记录时的广播内容
    memset(&_cfg, 0, sizeof(_cfg));
    strncpy(_cfg.uasId, UAS_ID, RID_CONFIG_UAS_ID_LEN);
    strncpy(_cfg.realNameId, REALNAME_ID, RID_CONFIG_REALNAME_LEN);
    _cfg.opCategory = OP_CATEGORY;
    _cfg.uaClass    = UA_CLASS;

    // 尝试从 NVS 加载上次持久化配置
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "No saved config — using placeholder defaults (UNCONFIGURED)");
        return true;
    }

    bool allOk = true;
    size_t len;

    len = sizeof(_cfg.uasId);
    if (nvs_get_str(h, KEY_UAS, _cfg.uasId, &len) != ESP_OK) allOk = false;
    len = sizeof(_cfg.realNameId);
    if (nvs_get_str(h, KEY_RN, _cfg.realNameId, &len) != ESP_OK) allOk = false;

    uint8_t v;
    if (nvs_get_u8(h, KEY_OPC, &v) != ESP_OK) allOk = false; else _cfg.opCategory = v;
    if (nvs_get_u8(h, KEY_UAC, &v) != ESP_OK) allOk = false; else _cfg.uaClass = v;
    nvs_close(h);

    if (allOk) {
        _state = RID_STATE_CONFIGURED;
        ESP_LOGI(TAG, "Loaded saved config — UAS_ID=%.20s, realname=%.8s, op_cat=%d, ua_class=%d",
                 _cfg.uasId, _cfg.realNameId, _cfg.opCategory, _cfg.uaClass);
    } else {
        ESP_LOGW(TAG, "Partial saved config — falling back to placeholders");
        // 部分记录缺失时回到占位值, 但保留已读出的合法字段 (尽量不丢)
        _state = RID_STATE_UNCONFIGURED;
    }
    return true;
}

void RidConfigStore::get(RidConfig& out) const {
    if (_mutex) xSemaphoreTake((SemaphoreHandle_t)_mutex, portMAX_DELAY);
    out = _cfg;
    if (_mutex) xSemaphoreGive((SemaphoreHandle_t)_mutex);
}

RidConfigState RidConfigStore::state() const {
    RidConfigState s;
    if (_mutex) xSemaphoreTake((SemaphoreHandle_t)_mutex, portMAX_DELAY);
    s = _state;
    if (_mutex) xSemaphoreGive((SemaphoreHandle_t)_mutex);
    return s;
}

bool RidConfigStore::isConfigured() const {
    return state() != RID_STATE_UNCONFIGURED;
}

void RidConfigStore::setState(RidConfigState s) {
    if (_mutex) xSemaphoreTake((SemaphoreHandle_t)_mutex, portMAX_DELAY);
    _state = s;
    if (_mutex) xSemaphoreGive((SemaphoreHandle_t)_mutex);
}

bool RidConfigStore::setField(RidConfigField field, const uint8_t* data, uint16_t len) {
    if (!data) return false;

    // 空中状态写锁定 (GB 46750-2025: 起飞后配置字段不可更改)
    if (state() == RID_STATE_AIRBORNE) {
        ESP_LOGW(TAG, "Write rejected — AIRBORNE (write-locked)");
        return false;
    }

    // 校验 + 暂存 (不在持锁状态下做 I/O)
    RidConfig next;
    {
        if (_mutex) xSemaphoreTake((SemaphoreHandle_t)_mutex, portMAX_DELAY);
        next = _cfg;
        if (_mutex) xSemaphoreGive((SemaphoreHandle_t)_mutex);
    }

    switch (field) {
    case RidConfigField::UAS_ID:
        if (!ridConfigValidateUasId((const char*)data, len)) {
            ESP_LOGW(TAG, "UAS_ID invalid — rejected");
            return false;
        }
        memcpy(next.uasId, data, RID_CONFIG_UAS_ID_LEN);
        next.uasId[RID_CONFIG_UAS_ID_LEN] = '\0';
        break;
    case RidConfigField::REALNAME_ID:
        if (!ridConfigValidateRealName((const char*)data, len)) {
            ESP_LOGW(TAG, "REALNAME_ID invalid — rejected");
            return false;
        }
        memcpy(next.realNameId, data, RID_CONFIG_REALNAME_LEN);
        next.realNameId[RID_CONFIG_REALNAME_LEN] = '\0';
        break;
    case RidConfigField::OP_CATEGORY:
        if (len != 1 || !ridConfigValidateOpCategory(data[0])) {
            ESP_LOGW(TAG, "OP_CATEGORY invalid — rejected");
            return false;
        }
        next.opCategory = data[0];
        break;
    case RidConfigField::UA_CLASS:
        if (len != 1 || !ridConfigValidateUaClass(data[0])) {
            ESP_LOGW(TAG, "UA_CLASS invalid — rejected");
            return false;
        }
        next.uaClass = data[0];
        break;
    default:
        return false;
    }

    // 提交内存 + 持久化 NVS
    {
        if (_mutex) xSemaphoreTake((SemaphoreHandle_t)_mutex, portMAX_DELAY);
        _cfg = next;
        if (_mutex) xSemaphoreGive((SemaphoreHandle_t)_mutex);
    }

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, KEY_UAS, _cfg.uasId);
        nvs_set_str(h, KEY_RN, _cfg.realNameId);
        nvs_set_u8(h, KEY_OPC, _cfg.opCategory);
        nvs_set_u8(h, KEY_UAC, _cfg.uaClass);
        nvs_commit(h);
        nvs_close(h);
    }

    // 任一字段成功写入即视为已配置
    setState(RID_STATE_CONFIGURED);
    ESP_LOGI(TAG, "Field updated (%d) — UAS_ID=%.20s, realname=%.8s, op_cat=%d, ua_class=%d",
             (int)field, _cfg.uasId, _cfg.realNameId, _cfg.opCategory, _cfg.uaClass);
    return true;
}
