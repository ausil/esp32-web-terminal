// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dennis Gilmore

#include "config.h"
#include "wifi_manager.h"
#include "auth.h"
#include "serial_port.h"
#include "web_server.h"
#include "gpio_control.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include <time.h>

static const char *TAG = "main";

// NTP status — exposed via web_server
static char s_ntp_server[80] = "";
static bool s_ntp_synced = false;

const char *ntp_get_server(void) { return s_ntp_server; }
bool ntp_is_synced(void) { return s_ntp_synced; }

// Serial RX callback: forward data to WebSocket clients on the same port
static void serial_to_ws_callback(int port_index, const uint8_t *data, size_t len, void *ctx)
{
    web_server_ws_broadcast(port_index, data, len);
}

static void ntp_sync_cb(struct timeval *tv)
{
    s_ntp_synced = true;

    // Check which server we're using
    const char *name = esp_sntp_getservername(0);
    if (name && strlen(name) > 0) {
        snprintf(s_ntp_server, sizeof(s_ntp_server), "%s", name);
    } else {
        const ip_addr_t *ip = esp_sntp_getserver(0);
        if (ip) {
            snprintf(s_ntp_server, sizeof(s_ntp_server), IPSTR, IP2STR(&ip->u_addr.ip4));
        }
    }

    time_t now = tv->tv_sec;
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    char strftime_buf[64];
    strftime(strftime_buf, sizeof(strftime_buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
    ESP_LOGI(TAG, "NTP synced: %s (server: %s)", strftime_buf, s_ntp_server);
}

static void ntp_init(void)
{
    app_config_t *conf = config_get();

    // Set timezone
    setenv("TZ", conf->timezone, 1);
    tzset();
    ESP_LOGI(TAG, "Timezone: %s", conf->timezone);

    if (strlen(conf->ntp_server) > 0) {
        // Manual NTP server configured — use it directly
        esp_sntp_config_t ntp_config = ESP_NETIF_SNTP_DEFAULT_CONFIG(conf->ntp_server);
        ntp_config.sync_cb = ntp_sync_cb;
        snprintf(s_ntp_server, sizeof(s_ntp_server), "%s (configured)", conf->ntp_server);
        ESP_LOGI(TAG, "NTP server: %s", conf->ntp_server);
        esp_netif_sntp_init(&ntp_config);
    } else {
        // DHCP NTP discovery: set pool.ntp.org on index 1, leave index 0 for DHCP
        esp_sntp_config_t ntp_config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        ntp_config.sync_cb = ntp_sync_cb;
        ntp_config.server_from_dhcp = true;
        ntp_config.renew_servers_after_new_IP = false;
        snprintf(s_ntp_server, sizeof(s_ntp_server), "awaiting DHCP (fallback: pool.ntp.org)");
        ESP_LOGI(TAG, "NTP: DHCP discovery enabled (fallback: pool.ntp.org)");
        esp_netif_sntp_init(&ntp_config);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32 Web Terminal starting...");

    // 1. Initialize persistent config (NVS)
    ESP_ERROR_CHECK(config_init());

    // 2. Initialize authentication
    ESP_ERROR_CHECK(auth_init());

    // 3. Initialize GPIO control (reset + power)
    ESP_ERROR_CHECK(gpio_control_init());

    // 4. Initialize NTP before WiFi so DHCP NTP discovery is ready
    //    when the DHCP lease arrives
    ntp_init();

    // 5. Initialize WiFi
    ESP_ERROR_CHECK(wifi_manager_init());

    // 6. Initialize serial ports (UART + USB on S3)
    serial_port_set_rx_callback(serial_to_ws_callback, NULL);
    ESP_ERROR_CHECK(serial_port_init_all());

    // 7. Start HTTPS + WebSocket server
    ESP_ERROR_CHECK(web_server_init());
    ESP_ERROR_CHECK(web_server_start());

    // 8. Mark OTA firmware as valid (rollback protection)
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            esp_ota_mark_app_valid_cancel_rollback();
            ESP_LOGI(TAG, "OTA firmware verified and confirmed");
        }
    }

    ESP_LOGI(TAG, "ESP32 Web Terminal ready");

    wifi_manager_status_t wifi = wifi_manager_get_status();
    if (wifi.sta_connected) {
        ESP_LOGI(TAG, "Access terminal at: https://%s/", wifi.sta_ip);
    }
    ESP_LOGI(TAG, "AP mode available at: https://%s/", wifi.ap_ip);
}
