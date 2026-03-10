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
void wifi_manager_update_hostname(const char *device_name);
wifi_manager_status_t wifi_manager_get_status(void);
