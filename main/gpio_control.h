// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dennis Gilmore

#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#if CONFIG_IDF_TARGET_ESP32C3
#define GPIO_SBC_RESET   6
#define GPIO_SBC_POWER   7
#elif CONFIG_IDF_TARGET_ESP32S3
#define GPIO_SBC_RESET   4
#define GPIO_SBC_POWER   5
#else // ESP32-C6
#define GPIO_SBC_RESET   22
#define GPIO_SBC_POWER   23
#endif

typedef struct {
    bool power_on;
    int64_t last_reset_time;   // microseconds since boot
    int64_t last_power_toggle; // microseconds since boot
} gpio_status_t;

esp_err_t gpio_control_init(void);
esp_err_t gpio_sbc_reset(void);
esp_err_t gpio_sbc_power_on(void);
esp_err_t gpio_sbc_power_off(void);
esp_err_t gpio_sbc_power_toggle(void);
gpio_status_t gpio_get_status(void);
