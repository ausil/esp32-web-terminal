#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#define CONFIG_WIFI_SSID_MAX_LEN     32
#define CONFIG_WIFI_PASS_MAX_LEN     64
#define CONFIG_AUTH_USER_MAX_LEN     32
#define CONFIG_AUTH_HASH_LEN         32  // SHA-256
#define CONFIG_AUTH_SALT_LEN         16

#define CONFIG_DEFAULT_BAUD_RATE     115200
#define CONFIG_DEFAULT_AP_SSID       "ESP-Terminal"
#define CONFIG_DEFAULT_AP_PASS       "esp32term"
#define CONFIG_DEFAULT_AUTH_USER     "admin"
#define CONFIG_DEFAULT_AUTH_PASS     "admin"
#define CONFIG_DEVICE_NAME_MAX_LEN  32
#define CONFIG_DEFAULT_DEVICE_NAME  "ESP32 Terminal"
#define CONFIG_NTP_SERVER_MAX_LEN   64
#define CONFIG_TIMEZONE_MAX_LEN    40
#define CONFIG_DEFAULT_TIMEZONE    "UTC0"

typedef struct {
    char sta_ssid[CONFIG_WIFI_SSID_MAX_LEN + 1];
    char sta_pass[CONFIG_WIFI_PASS_MAX_LEN + 1];
    char ap_ssid[CONFIG_WIFI_SSID_MAX_LEN + 1];
    char ap_pass[CONFIG_WIFI_PASS_MAX_LEN + 1];
    uint32_t baud_rate;
    bool power_on_default;  // SBC power state at ESP32 boot
    char auth_user[CONFIG_AUTH_USER_MAX_LEN + 1];
    uint8_t auth_hash[CONFIG_AUTH_HASH_LEN];
    uint8_t auth_salt[CONFIG_AUTH_SALT_LEN];
    bool auth_initialized;  // true after first password change
    char device_name[CONFIG_DEVICE_NAME_MAX_LEN + 1];
    char ntp_server[CONFIG_NTP_SERVER_MAX_LEN + 1];  // empty = use DHCP
    char timezone[CONFIG_TIMEZONE_MAX_LEN + 1];     // POSIX TZ string
} app_config_t;

esp_err_t config_init(void);
app_config_t *config_get(void);
esp_err_t config_set_wifi_sta(const char *ssid, const char *password);
esp_err_t config_set_wifi_ap(const char *ssid, const char *password);
esp_err_t config_set_baud_rate(uint32_t baud_rate);
esp_err_t config_set_power_on_default(bool power_on);
esp_err_t config_set_auth(const char *username, const char *password);
esp_err_t config_set_device_name(const char *name);
esp_err_t config_set_ntp_server(const char *server);
esp_err_t config_set_timezone(const char *tz);
bool config_check_password(const char *password);
