#pragma once
// Minimal FreeRTOS type/constant stubs — host tests run single-threaded.
#include <stdint.h>

typedef uint32_t       TickType_t;
typedef int            BaseType_t;
typedef unsigned int   UBaseType_t;
typedef uint32_t       StackType_t;
typedef uint32_t       portMUX_TYPE;

#define portMUX_INITIALIZER_UNLOCKED 0

// Critical sections are no-ops on host (single-threaded).
#define portENTER_CRITICAL_SAFE(mux) do { (void)(mux); } while (0)
#define portEXIT_CRITICAL_SAFE(mux)  do { (void)(mux); } while (0)

#define pdTRUE    ((BaseType_t)1)
#define pdFALSE   ((BaseType_t)0)
#define pdPASS    ((BaseType_t)1)
#define pdFAIL    ((BaseType_t)0)
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))

typedef void* QueueHandle_t;
typedef void* TaskHandle_t;
typedef void* SemaphoreHandle_t;
typedef void (*TaskFunction_t)(void*);
