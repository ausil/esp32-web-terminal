#include "config.h"
#include "wifi_manager.h"
#include "auth.h"
#include "uart_bridge.h"
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

// UART RX callback: forward data to all WebSocket clients
static void uart_to_ws_callback(const uint8_t *data, size_t len, void *ctx)
{
    web_server_ws_broadcast(data, len);
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
    esp_sntp_config_t ntp_config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    ntp_config.sync_cb = ntp_sync_cb;

    if (strlen(conf->ntp_server) > 0) {
        ntp_config.servers[0] = conf->ntp_server;
        snprintf(s_ntp_server, sizeof(s_ntp_server), "%s (configured)", conf->ntp_server);
        ESP_LOGI(TAG, "NTP server: %s", conf->ntp_server);
    } else {
        ntp_config.server_from_dhcp = true;
        ntp_config.renew_servers_after_new_IP = true;
        snprintf(s_ntp_server, sizeof(s_ntp_server), "pool.ntp.org (awaiting DHCP)");
        ESP_LOGI(TAG, "NTP: using DHCP (fallback: pool.ntp.org)");
    }

    esp_netif_sntp_init(&ntp_config);
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

    // 4. Initialize WiFi
    ESP_ERROR_CHECK(wifi_manager_init());

    // 5. Initialize NTP
    ntp_init();

    // 6. Initialize UART bridge with configured baud rate
    app_config_t *conf = config_get();
    ESP_ERROR_CHECK(uart_bridge_init(conf->baud_rate));
    uart_bridge_set_rx_callback(uart_to_ws_callback, NULL);
    ESP_ERROR_CHECK(uart_bridge_start());

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
