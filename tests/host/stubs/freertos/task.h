#pragma once
#include "freertos/FreeRTOS.h"

/* Test control for production code that blocks on task notifications. */
extern int g_notify_take_result;  /* pdTRUE once the "library task" signals */

static inline BaseType_t xTaskCreate(void (*fn)(void *), const char *name,
                                     uint32_t stack, void *arg,
                                     UBaseType_t prio, TaskHandle_t *out)
{
    (void)fn; (void)name; (void)stack; (void)arg; (void)prio;
    if (out) *out = (void *)1;
    return pdPASS;
}
static inline void xTaskNotifyGive(TaskHandle_t t) { (void)t; g_notify_take_result = pdTRUE; }
static inline uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t t)
{ (void)clear; (void)t; return (uint32_t)g_notify_take_result; }
static inline TaskHandle_t xTaskGetCurrentTaskHandle(void) { return NULL; }
