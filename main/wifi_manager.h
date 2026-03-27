// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dennis Gilmore

#pragma once

#include "esp_err.h"
#include <stdbool.h>

typedef enum {
    WIFI_MGR_MODE_AP,
    WIFI_MGR_MODE_STA,
    WIFI_MGR_MODE_AP_STA,
} wifi_manager_mode_t;

typedef struct {
    wifi_manager_mode_t mode;
    bool sta_connected;
    char sta_ip[16];
    char ap_ip[16];
} wifi_manager_status_t;

esp_err_t wifi_manager_init(void);
esp_err_t wifi_manager_connect_sta(const char *ssid, const char *password);
esp_err_t wifi_manager_disconnect_sta(void);
esp_err_t wifi_manager_update_ap(const char *ssid, const char *password);
void wifi_manager_update_hostname(const char *device_name);
wifi_manager_status_t wifi_manager_get_status(void);

typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t authmode;  // 0=open, else secured
} wifi_scan_result_t;

// Scan for nearby APs. Caller must free(*results) when done.
// Returns number of APs found, or -1 on error.
int wifi_manager_scan(wifi_scan_result_t **results);
