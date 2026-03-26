// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dennis Gilmore

#pragma once

#include "esp_err.h"

esp_err_t web_server_init(void);
esp_err_t web_server_start(void);
void web_server_stop(void);

// Broadcast data to all connected WebSocket clients (called from UART RX callback)
void web_server_ws_broadcast(const uint8_t *data, size_t len);
