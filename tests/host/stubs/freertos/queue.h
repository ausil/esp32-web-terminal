#pragma once
#include "freertos/FreeRTOS.h"

static inline QueueHandle_t xQueueCreate(UBaseType_t len, UBaseType_t item)
{ (void)len; (void)item; static int q; return (void *)&q; }
static inline BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t t)
{ (void)q; (void)item; (void)t; return pdTRUE; }
static inline BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t t)
{ (void)q; (void)item; (void)t; return pdFALSE; }
static inline void vQueueDelete(QueueHandle_t q) { (void)q; }
