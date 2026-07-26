#ifndef INDICATORS_H
#define INDICATORS_H

#include <stdint.h>
#include "led_strip.h"
#include "config.h"

// GB 46750-2025 5.1.5: 模块必须以可视化或声音形式通知操控员
enum class LedState {
    OFF,
    STANDBY,        // 绿色慢闪 — 地面待机
    BROADCASTING,   // 蓝色快闪 — 空中/紧急广播中
    FAULT           // 红色常亮 — 模块故障
};

// GB 46750-2025 5.1.7: 起飞前联锁 — 模块失效时禁止飞控起飞
class RIDInterlock {
public:
    bool init();
    void arm();
    void disarm();
    bool isArmed() const { return _armed; }

private:
    bool _armed = false;
};

// ESP32-C5-DevKitC-1 板载 WS2812B (GPIO27, RMT 驱动)
class StatusLed {
public:
    bool init();
    void setState(LedState state);
    void update();    // 主循环每次调用，内部管理闪烁定时
    void clear();     // 释放 RMT 资源

private:
    void setColor(uint8_t r, uint8_t g, uint8_t b);
    void setOff();

    led_strip_handle_t _handle = nullptr;
    LedState  _state = LedState::OFF;
    uint64_t  _lastToggleMs = 0;
    bool      _ledOn = false;
};

#endif
