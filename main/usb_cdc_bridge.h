// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dennis Gilmore

#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// serial_port vtable functions
esp_err_t usb_cdc_init(int port_index, uint32_t baud_rate);
esp_err_t usb_cdc_send(int port_index, const uint8_t *data, size_t len);
esp_err_t usb_cdc_set_baud_rate(int port_index, uint32_t baud_rate);
esp_err_t usb_cdc_start(int port_index);

bool usb_cdc_is_connected(void);
