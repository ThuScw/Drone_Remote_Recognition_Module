#pragma once
// Stub: queues accept everything, never block, never run — the log task loop
// is never scheduled on host, so record writing goes through writeRecord directly.
#include "FreeRTOS.h"

inline QueueHandle_t xQueueCreate(UBaseType_t uxQueueLength, UBaseType_t uxItemSize) {
    (void)uxQueueLength;
    (void)uxItemSize;
    return (QueueHandle_t)1;   // non-null sentinel
}

inline BaseType_t xQueueSend(QueueHandle_t xQueue, const void* pvItemToQueue, TickType_t xTicksToWait) {
    (void)xQueue; (void)pvItemToQueue; (void)xTicksToWait;
    return pdTRUE;
}

inline BaseType_t xQueueReceive(QueueHandle_t xQueue, void* pvBuffer, TickType_t xTicksToWait) {
    (void)xQueue; (void)pvBuffer; (void)xTicksToWait;
    return pdTRUE;
}

inline void vQueueDelete(QueueHandle_t xQueue) {
    (void)xQueue;
}
