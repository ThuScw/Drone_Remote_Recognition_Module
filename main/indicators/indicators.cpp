#include "indicators.h"
#include <esp_log.h>
#include <esp_timer.h>
#include <driver/gpio.h>

static const char* TAG = "IND";

// ======================== RIDInterlock ========================

bool RIDInterlock::init() {
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << INTERLOCK_RID_OK_GPIO;
    cfg.mode         = GPIO_MODE_OUTPUT;
    cfg.pull_up_en   = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_ENABLE;
    cfg.intr_type    = GPIO_INTR_DISABLE;
    esp_err_t rc = gpio_config(&cfg);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "Interlock GPIO config failed: %d", rc);
        return false;
    }
    disarm();
    ESP_LOGI(TAG, "Interlock GPIO%d init OK (default: DISARMED)", INTERLOCK_RID_OK_GPIO);
    return true;
}

void RIDInterlock::arm() {
    _armed = true;
    gpio_set_level(INTERLOCK_RID_OK_GPIO, INTERLOCK_ACTIVE_LEVEL ? 1 : 0);
    ESP_LOGI(TAG, "Interlock: ARMED — flight controller may take off");
}

void RIDInterlock::disarm() {
    _armed = false;
    gpio_set_level(INTERLOCK_RID_OK_GPIO, INTERLOCK_ACTIVE_LEVEL ? 0 : 1);
    ESP_LOGW(TAG, "Interlock: DISARMED — flight controller should prevent takeoff");
}

// ======================== StatusLed (WS2812B via RMT) ========================

bool StatusLed::init() {
    led_strip_config_t strip_cfg = {};
    strip_cfg.strip_gpio_num = STATUS_LED_GPIO;
    strip_cfg.max_leds = STATUS_LED_NUM_LEDS;
    strip_cfg.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;

    led_strip_rmt_config_t rmt_cfg = {};
    rmt_cfg.clk_src = RMT_CLK_SRC_DEFAULT;
    rmt_cfg.resolution_hz = 10 * 1000 * 1000;  // 10 MHz
    rmt_cfg.flags.with_dma = false;

    esp_err_t rc = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &_handle);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "LED strip init failed: %d", rc);
        return false;
    }

    setOff();
    ESP_LOGI(TAG, "WS2812 LED GPIO%d init OK", STATUS_LED_GPIO);
    return true;
}

void StatusLed::clear() {
    if (_handle) {
        setOff();
        led_strip_del(_handle);
        _handle = nullptr;
    }
}

void StatusLed::setState(LedState state) {
    if (state != _state) {
        _state = state;
        _ledOn = false;
        _lastToggleMs = 0;
    }
}

void StatusLed::update() {
    if (!_handle) return;

    uint64_t nowMs = (uint64_t)(esp_timer_get_time() / 1000);

    switch (_state) {
    case LedState::OFF:
        if (_ledOn) setOff();
        return;

    case LedState::FAULT:
        if (!_ledOn) setColor(255, 0, 0);  // 红色常亮
        return;

    case LedState::BROADCASTING:
        // 蓝色快闪: 200ms 开 / 200ms 关 → 2.5Hz
        if (nowMs - _lastToggleMs >= 200) {
            _lastToggleMs = nowMs;
            if (_ledOn) {
                setOff();
            } else {
                setColor(0, 0, 255);
            }
        }
        return;

    case LedState::STANDBY:
        // 绿色慢闪: 200ms 开 / 1800ms 关 → 0.5Hz
        if (!_ledOn && nowMs - _lastToggleMs >= 1800) {
            _lastToggleMs = nowMs;
            setColor(0, 255, 0);
        } else if (_ledOn && nowMs - _lastToggleMs >= 200) {
            _lastToggleMs = nowMs;
            setOff();
        }
        return;
    }
}

void StatusLed::setColor(uint8_t r, uint8_t g, uint8_t b) {
    _ledOn = true;
    led_strip_set_pixel(_handle, 0, r, g, b);
    led_strip_refresh(_handle);
}

void StatusLed::setOff() {
    _ledOn = false;
    led_strip_set_pixel(_handle, 0, 0, 0, 0);
    led_strip_refresh(_handle);
}
