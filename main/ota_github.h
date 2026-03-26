// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dennis Gilmore

#pragma once

#include "esp_err.h"
#include <stdbool.h>

typedef struct {
    bool update_available;
    char latest_version[32];
    char asset_url[256];
} ota_github_check_result_t;

// Check GitHub for a newer release
esp_err_t ota_github_check(ota_github_check_result_t *result);

// Download and apply update from asset URL, returns ESP_OK on success (caller should reboot)
esp_err_t ota_github_apply(const char *asset_url);
