// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dennis Gilmore

#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

// Pin assignments per target (UART1 only; S3 has single UART port)
#if CONFIG_IDF_TARGET_ESP32C3
#define UART_BRIDGE_TX_PIN      0
#define UART_BRIDGE_RX_PIN      1
#elif CONFIG_IDF_TARGET_ESP32S3
#define UART_BRIDGE_TX_PIN      1
#define UART_BRIDGE_RX_PIN      2
#else // ESP32-C6
#define UART_BRIDGE_TX_PIN      10
#define UART_BRIDGE_RX_PIN      11
#endif

#define UART_BRIDGE_BUF_SIZE    (1024)     // UART driver RX buffer
#define UART_BRIDGE_RX_RING_SIZE (8192)    // Ring buffer for WebSocket TX

// serial_port vtable functions (port_index maps to UART instance)
esp_err_t uart_bridge_init(int port_index, uint32_t baud_rate);
esp_err_t uart_bridge_set_baud_rate(int port_index, uint32_t baud_rate);
esp_err_t uart_bridge_send(int port_index, const uint8_t *data, size_t len);
esp_err_t uart_bridge_start(int port_index);
