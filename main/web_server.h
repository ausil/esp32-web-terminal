// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dennis Gilmore

#pragma once

#include "esp_err.h"

esp_err_t web_server_init(void);
esp_err_t web_server_start(void);
void web_server_stop(void);

// Broadcast data to WebSocket clients connected to a specific serial port
void web_server_ws_broadcast(int port_index, const uint8_t *data, size_t len);

// Broadcast a text message to all WebSocket clients (for status events)
void web_server_ws_broadcast_text(const char *text);
