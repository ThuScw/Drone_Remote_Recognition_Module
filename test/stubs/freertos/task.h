#pragma once
// Stub: tasks are never actually spawned on host.
#include "FreeRTOS.h"

inline BaseType_t xTaskCreate(TaskFunction_t pxTaskCode, const char* pcName,
                              uint32_t usStackDepth, void* pvParameters,
                              UBaseType_t uxPriority, TaskHandle_t* pxCreatedTask) {
    (void)pxTaskCode; (void)pcName; (void)usStackDepth;
    (void)pvParameters; (void)uxPriority;
    if (pxCreatedTask) *pxCreatedTask = (TaskHandle_t)1;
    return pdPASS;
}

inline void vTaskDelete(TaskHandle_t xTask) {
    (void)xTask;
}

inline void vTaskDelay(TickType_t xTicksToDelay) {
    (void)xTicksToDelay;
}

inline uint32_t uxTaskGetStackHighWaterMark(TaskHandle_t xTask) {
    (void)xTask;
    return 0x1000;  // plenty of headroom
}
