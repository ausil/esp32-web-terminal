// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dennis Gilmore

#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

#define UART_BRIDGE_PORT        1          // UART1

#if CONFIG_IDF_TARGET_ESP32C3
#define UART_BRIDGE_TX_PIN      0
#define UART_BRIDGE_RX_PIN      1
#else // ESP32-C6
#define UART_BRIDGE_TX_PIN      10
#define UART_BRIDGE_RX_PIN      11
#endif
#define UART_BRIDGE_BUF_SIZE    (1024)     // UART driver RX buffer
#define UART_BRIDGE_RX_RING_SIZE (8192)    // Ring buffer for WebSocket TX

// Callback for received UART data — called from the UART RX task.
typedef void (*uart_bridge_rx_cb_t)(const uint8_t *data, size_t len, void *ctx);

esp_err_t uart_bridge_init(uint32_t baud_rate);
esp_err_t uart_bridge_set_baud_rate(uint32_t baud_rate);
uint32_t uart_bridge_get_baud_rate(void);

// Send data to UART (from WebSocket → SBC)
esp_err_t uart_bridge_send(const uint8_t *data, size_t len);

// Register a callback for UART RX data (SBC → WebSocket)
void uart_bridge_set_rx_callback(uart_bridge_rx_cb_t cb, void *ctx);

// Start the UART RX task
esp_err_t uart_bridge_start(void);
