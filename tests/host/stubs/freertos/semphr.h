#pragma once
#include "freertos/FreeRTOS.h"

/* No-ops: mutex take/give in production code has to stay balanced, and that's
 * exercised by the fact that the code runs to completion on one thread. */
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    static int dummy;
    return (void *)&dummy;
}
static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t t)
{ (void)s; (void)t; return pdTRUE; }
static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t s)
{ (void)s; return pdTRUE; }
static inline void vSemaphoreDelete(SemaphoreHandle_t s) { (void)s; }
