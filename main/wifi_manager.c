// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dennis Gilmore

#include "wifi_manager.h"
#include "config.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "mdns.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "wifi_mgr";

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
#define MAX_STA_RETRIES     5

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_count = 0;
static wifi_manager_status_t s_status;
static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif = NULL;
static TimerHandle_t s_reconnect_timer = NULL;
static bool s_scanning = false;  // suppress disconnect handling during scan

#define RECONNECT_INTERVAL_MS  30000  // Retry STA every 30s when on AP fallback

static esp_err_t start_ap(const char *ssid, const char *password);
static void sanitize_hostname(const char *name, char *out, size_t out_len);
static void reconnect_timer_cb(TimerHandle_t timer);

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (!s_scanning) {
            esp_wifi_connect();
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_scanning) {
            ESP_LOGI(TAG, "STA disconnect during scan, ignoring");
            return;
        }
        s_status.sta_connected = false;
        memset(s_status.sta_ip, 0, sizeof(s_status.sta_ip));
        if (s_retry_count < MAX_STA_RETRIES) {
            esp_wifi_connect();
            s_retry_count++;
            ESP_LOGI(TAG, "Retrying STA connection (%d/%d)", s_retry_count, MAX_STA_RETRIES);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            ESP_LOGW(TAG, "STA connection failed after %d retries", MAX_STA_RETRIES);

            // Re-enable AP as fallback
            if (s_status.mode == WIFI_MGR_MODE_STA) {
                ESP_LOGI(TAG, "STA lost, re-enabling AP");
                esp_wifi_set_mode(WIFI_MODE_APSTA);
                s_status.mode = WIFI_MGR_MODE_AP_STA;

                app_config_t *conf = config_get();
                uint8_t mac[6];
                esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
                char ap_ssid[40];
                snprintf(ap_ssid, sizeof(ap_ssid), "%s-%02X%02X", conf->ap_ssid, mac[4], mac[5]);
                start_ap(ap_ssid, conf->ap_pass);
            }

            // Start watchdog to periodically retry STA
            if (s_reconnect_timer && strlen(config_get()->sta_ssid) > 0) {
                xTimerStart(s_reconnect_timer, 0);
                ESP_LOGI(TAG, "STA reconnect watchdog started (every %ds)", RECONNECT_INTERVAL_MS / 1000);
            }
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        snprintf(s_status.sta_ip, sizeof(s_status.sta_ip), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "STA connected, IP: %s", s_status.sta_ip);
        s_status.sta_connected = true;
        s_retry_count = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

        // Stop reconnect watchdog
        if (s_reconnect_timer) {
            xTimerStop(s_reconnect_timer, 0);
        }

        // Disable AP now that STA is connected
        if (s_status.mode == WIFI_MGR_MODE_AP_STA) {
            ESP_LOGI(TAG, "STA connected, disabling AP");
            esp_wifi_set_mode(WIFI_MODE_STA);
            s_status.mode = WIFI_MGR_MODE_STA;
            memset(s_status.ap_ip, 0, sizeof(s_status.ap_ip));
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
        ESP_LOGI(TAG, "AP: station " MACSTR " connected", MAC2STR(event->mac));
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *)event_data;
        ESP_LOGI(TAG, "AP: station " MACSTR " disconnected", MAC2STR(event->mac));
    }
}

static esp_err_t start_ap(const char *ssid, const char *password)
{
    wifi_config_t ap_config = {
        .ap = {
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = { .required = false },
        },
    };
    strncpy((char *)ap_config.ap.ssid, ssid, sizeof(ap_config.ap.ssid));
    ap_config.ap.ssid_len = strlen(ssid);
    strncpy((char *)ap_config.ap.password, password, sizeof(ap_config.ap.password));

    if (strlen(password) < 8) {
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
        ESP_LOGW(TAG, "AP password too short, using open auth");
    }

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));

    // Get AP IP
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(s_ap_netif, &ip_info) == ESP_OK) {
        snprintf(s_status.ap_ip, sizeof(s_status.ap_ip), IPSTR, IP2STR(&ip_info.ip));
    }

    ESP_LOGI(TAG, "AP started: SSID='%s', IP=%s", ssid, s_status.ap_ip);
    return ESP_OK;
}

static void sanitize_hostname(const char *name, char *out, size_t out_len)
{
    size_t j = 0;
    for (size_t i = 0; name[i] && j < out_len - 1; i++) {
        char c = name[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
            out[j++] = c;
        } else if (c == ' ' || c == '_' || c == '.') {
            if (j > 0 && out[j - 1] != '-') out[j++] = '-';
        }
    }
    // Trim trailing hyphen
    while (j > 0 && out[j - 1] == '-') j--;
    out[j] = '\0';
}

void wifi_manager_update_hostname(const char *device_name)
{
    char hostname[33];
    sanitize_hostname(device_name, hostname, sizeof(hostname));
    if (strlen(hostname) == 0) {
        strncpy(hostname, "esp32-terminal", sizeof(hostname));
    }
    if (s_sta_netif) {
        esp_netif_set_hostname(s_sta_netif, hostname);
        ESP_LOGI(TAG, "DHCP hostname set to '%s'", hostname);
    }
    // Update mDNS hostname
    mdns_hostname_set(hostname);
    mdns_instance_name_set(device_name);
}

static void reconnect_timer_cb(TimerHandle_t timer)
{
    if (s_status.sta_connected) {
        xTimerStop(timer, 0);
        return;
    }
    ESP_LOGI(TAG, "Watchdog: retrying STA connection...");
    s_retry_count = 0;
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    esp_wifi_connect();
}

static void mdns_init_service(const char *hostname)
{
    char sanitized[33];
    sanitize_hostname(hostname, sanitized, sizeof(sanitized));
    if (strlen(sanitized) == 0) {
        strncpy(sanitized, "esp32-terminal", sizeof(sanitized));
    }

    mdns_init();
    mdns_hostname_set(sanitized);
    mdns_instance_name_set(hostname);
    mdns_service_add(hostname, "_http", "_tcp", 443, NULL, 0);
    ESP_LOGI(TAG, "mDNS started: %s.local", sanitized);
}

esp_err_t wifi_manager_init(void)
{
    s_wifi_event_group = xEventGroupCreate();
    memset(&s_status, 0, sizeof(s_status));

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_ap_netif = esp_netif_create_default_wifi_ap();
    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                         &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                         &event_handler, NULL, NULL));

    app_config_t *conf = config_get();

    // Set DHCP hostname from device name
    wifi_manager_update_hostname(conf->device_name);

    // Start mDNS
    mdns_init_service(conf->device_name);

    // Create reconnect watchdog timer (not started until needed)
    s_reconnect_timer = xTimerCreate("sta_reconnect", pdMS_TO_TICKS(RECONNECT_INTERVAL_MS),
                                      pdTRUE, NULL, reconnect_timer_cb);

    // Append MAC suffix to AP SSID for uniqueness
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char ap_ssid[40];
    snprintf(ap_ssid, sizeof(ap_ssid), "%s-%02X%02X", conf->ap_ssid, mac[4], mac[5]);

    bool has_sta_config = strlen(conf->sta_ssid) > 0;

    if (has_sta_config) {
        // Try AP+STA mode
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
        s_status.mode = WIFI_MGR_MODE_AP_STA;

        start_ap(ap_ssid, conf->ap_pass);

        wifi_config_t sta_config = {};
        strncpy((char *)sta_config.sta.ssid, conf->sta_ssid, sizeof(sta_config.sta.ssid));
        strncpy((char *)sta_config.sta.password, conf->sta_pass, sizeof(sta_config.sta.password));
        sta_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
        ESP_ERROR_CHECK(esp_wifi_start());

        ESP_LOGI(TAG, "Connecting to STA SSID='%s'...", conf->sta_ssid);

        EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                                WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                                pdFALSE, pdFALSE, pdMS_TO_TICKS(15000));

        if (bits & WIFI_CONNECTED_BIT) {
            ESP_LOGI(TAG, "STA connected, AP disabled");
        } else {
            ESP_LOGW(TAG, "STA connection failed, AP still active");
        }
    } else {
        // AP-only mode
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
        s_status.mode = WIFI_MGR_MODE_AP;
        start_ap(ap_ssid, conf->ap_pass);
        ESP_ERROR_CHECK(esp_wifi_start());
        ESP_LOGI(TAG, "Started in AP-only mode (no STA config)");
    }

    return ESP_OK;
}

esp_err_t wifi_manager_connect_sta(const char *ssid, const char *password)
{
    config_set_wifi_sta(ssid, password);

    // Ensure AP+STA mode so AP is available as fallback during connection
    if (s_status.mode != WIFI_MGR_MODE_AP_STA) {
        esp_wifi_stop();
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
        s_status.mode = WIFI_MGR_MODE_AP_STA;

        app_config_t *conf = config_get();
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
        char ap_ssid[40];
        snprintf(ap_ssid, sizeof(ap_ssid), "%s-%02X%02X", conf->ap_ssid, mac[4], mac[5]);
        start_ap(ap_ssid, conf->ap_pass);
    }

    wifi_config_t sta_config = {};
    strncpy((char *)sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid));
    strncpy((char *)sta_config.sta.password, password, sizeof(sta_config.sta.password));
    sta_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));

    s_retry_count = 0;
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Attempting STA connection to '%s'", ssid);
    return ESP_OK;
}

esp_err_t wifi_manager_disconnect_sta(void)
{
    // Stop reconnect watchdog
    if (s_reconnect_timer) {
        xTimerStop(s_reconnect_timer, 0);
    }

    // Clear STA config
    config_set_wifi_sta("", "");

    // Switch to AP-only
    esp_wifi_disconnect();
    esp_wifi_stop();
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    s_status.mode = WIFI_MGR_MODE_AP;
    s_status.sta_connected = false;
    memset(s_status.sta_ip, 0, sizeof(s_status.sta_ip));

    app_config_t *conf = config_get();
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char ap_ssid[40];
    snprintf(ap_ssid, sizeof(ap_ssid), "%s-%02X%02X", conf->ap_ssid, mac[4], mac[5]);
    start_ap(ap_ssid, conf->ap_pass);
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "STA disconnected, switched to AP-only mode");
    return ESP_OK;
}

esp_err_t wifi_manager_update_ap(const char *ssid, const char *password)
{
    // Persist new AP config
    esp_err_t err = config_set_wifi_ap(ssid, password);
    if (err != ESP_OK) {
        return err;
    }

    // Rebuild AP SSID with MAC suffix
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char ap_ssid[40];
    snprintf(ap_ssid, sizeof(ap_ssid), "%s-%02X%02X", ssid, mac[4], mac[5]);

    // Reconfigure the running AP
    return start_ap(ap_ssid, password);
}

wifi_manager_status_t wifi_manager_get_status(void)
{
    return s_status;
}

int wifi_manager_scan(wifi_scan_result_t **results)
{
    *results = NULL;

    // Suppress auto-connect and disconnect events during scan
    s_scanning = true;

    // Scanning requires STA interface; switch to APSTA if needed
    wifi_mode_t orig_mode;
    esp_wifi_get_mode(&orig_mode);
    bool switched = false;
    if (orig_mode == WIFI_MODE_AP) {
        esp_wifi_set_mode(WIFI_MODE_APSTA);
        switched = true;
        vTaskDelay(pdMS_TO_TICKS(100));  // let STA interface initialize
    }

    // Disconnect STA if connecting/connected — ESP-IDF won't scan while STA is busy
    if (orig_mode == WIFI_MODE_STA || orig_mode == WIFI_MODE_APSTA) {
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(100));  // let disconnect settle
    }

    wifi_scan_config_t scan_config = {
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active = { .min = 120, .max = 500 },
    };

    esp_err_t err = esp_wifi_scan_start(&scan_config, true);  // blocking

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi scan failed: %s", esp_err_to_name(err));
        if (switched) {
            esp_wifi_set_mode(orig_mode);
        }
        // Reconnect STA if we disconnected it
        if (strlen(config_get()->sta_ssid) > 0) {
            s_scanning = false;
            esp_wifi_connect();
        } else {
            s_scanning = false;
        }
        return -1;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count == 0) {
        esp_wifi_scan_get_ap_records(&ap_count, NULL);  // clear scan results
        if (switched) esp_wifi_set_mode(orig_mode);
        if (strlen(config_get()->sta_ssid) > 0) {
            s_scanning = false;
            esp_wifi_connect();
        } else {
            s_scanning = false;
        }
        return 0;
    }

    // Cap at 20 results
    if (ap_count > 20) ap_count = 20;

    wifi_ap_record_t *ap_records = malloc(sizeof(wifi_ap_record_t) * ap_count);
    if (!ap_records) {
        esp_wifi_scan_get_ap_records(&ap_count, NULL);
        if (switched) esp_wifi_set_mode(orig_mode);
        return -1;
    }

    esp_wifi_scan_get_ap_records(&ap_count, ap_records);

    // Deduplicate by SSID, keeping strongest signal
    wifi_scan_result_t *res = malloc(sizeof(wifi_scan_result_t) * ap_count);
    if (!res) {
        free(ap_records);
        if (switched) esp_wifi_set_mode(orig_mode);
        return -1;
    }

    int count = 0;
    for (int i = 0; i < ap_count; i++) {
        if (ap_records[i].ssid[0] == '\0') continue;  // skip hidden

        // Check for duplicate SSID
        bool dup = false;
        for (int j = 0; j < count; j++) {
            if (strcmp(res[j].ssid, (char *)ap_records[i].ssid) == 0) {
                if (ap_records[i].rssi > res[j].rssi) {
                    res[j].rssi = ap_records[i].rssi;
                }
                dup = true;
                break;
            }
        }
        if (!dup) {
            strncpy(res[count].ssid, (char *)ap_records[i].ssid, sizeof(res[count].ssid) - 1);
            res[count].ssid[sizeof(res[count].ssid) - 1] = '\0';
            res[count].rssi = ap_records[i].rssi;
            res[count].authmode = (ap_records[i].authmode != WIFI_AUTH_OPEN) ? 1 : 0;
            count++;
        }
    }

    free(ap_records);

    // Restore original WiFi mode
    if (switched) {
        esp_wifi_set_mode(orig_mode);
    }

    // Reconnect STA if we disconnected it for scanning
    if (strlen(config_get()->sta_ssid) > 0) {
        ESP_LOGI(TAG, "Reconnecting STA after scan");
        s_scanning = false;
        esp_wifi_connect();
    } else {
        s_scanning = false;
    }

    *results = res;
    ESP_LOGI(TAG, "WiFi scan found %d unique APs", count);
    return count;
}
