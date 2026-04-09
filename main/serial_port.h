// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dennis Gilmore

#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#if CONFIG_IDF_TARGET_ESP32S3
#define SERIAL_PORT_MAX  2   // UART1 + USB CDC-ACM
#else
#define SERIAL_PORT_MAX  1   // UART1 only
#endif

typedef enum {
    SERIAL_TYPE_UART,
    SERIAL_TYPE_USB_CDC,
} serial_type_t;

typedef void (*serial_rx_cb_t)(int port_index, const uint8_t *data, size_t len, void *ctx);

typedef struct {
    serial_type_t type;
    const char *name;       // "UART1", "USB"
    bool available;         // true if hardware present/connected

    // Vtable
    esp_err_t (*init)(int port_index, uint32_t baud_rate);
    esp_err_t (*send)(int port_index, const uint8_t *data, size_t len);
    esp_err_t (*set_baud)(int port_index, uint32_t baud_rate);
    esp_err_t (*start)(int port_index);
} serial_port_t;

esp_err_t serial_port_init_all(void);
esp_err_t serial_port_send(int port_index, const uint8_t *data, size_t len);
esp_err_t serial_port_set_baud_rate(int port_index, uint32_t baud_rate);
void serial_port_set_rx_callback(serial_rx_cb_t cb, void *ctx);
int serial_port_count(void);
const serial_port_t *serial_port_get(int port_index);

// Called by port backends to deliver RX data
void serial_port_rx_notify(int port_index, const uint8_t *data, size_t len);
