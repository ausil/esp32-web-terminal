#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "esp_err.h"

typedef int BaseType_t;
typedef unsigned int UBaseType_t;
typedef uint32_t TickType_t;
typedef void * SemaphoreHandle_t;
typedef void * QueueHandle_t;
typedef void * TaskHandle_t;
typedef void * EventGroupHandle_t;
typedef void * TimerHandle_t;
typedef uint32_t EventBits_t;

#define pdPASS         1
#define pdFAIL         0
#define pdTRUE         1
#define pdFALSE        0
#define portMAX_DELAY  0xffffffffU
#define BIT0           0x0001U
#define BIT1           0x0002U

typedef struct { int dummy; } portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED { 0 }
#define portTICK_PERIOD_MS 1
#define pdMS_TO_TICKS(ms)  (ms)
#define configMINIMAL_STACK_SIZE 2048

/* Ring-buffer critical sections in production code become no-ops here; host
 * tests drive the ring from one thread, which is exactly the interleaving we
 * need to test deterministically. */
#define taskENTER_CRITICAL(mux) ((void)(mux))
#define taskEXIT_CRITICAL(mux)  ((void)(mux))
#define vTaskDelay(ticks) ((void)(ticks))
