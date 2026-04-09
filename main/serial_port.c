// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dennis Gilmore

#include "serial_port.h"
#include "uart_bridge.h"
#include "config.h"
#include "esp_log.h"

#if CONFIG_IDF_TARGET_ESP32S3
#include "usb_cdc_bridge.h"
#endif

static const char *TAG = "serial_port";

static serial_port_t s_ports[SERIAL_PORT_MAX];
static int s_port_count = 0;
static serial_rx_cb_t s_rx_cb = NULL;
static void *s_rx_cb_ctx = NULL;

void serial_port_rx_notify(int port_index, const uint8_t *data, size_t len)
{
    if (s_rx_cb) {
        s_rx_cb(port_index, data, len, s_rx_cb_ctx);
    }
}

esp_err_t serial_port_init_all(void)
{
    app_config_t *conf = config_get();

    // Port 0: UART1 (all targets)
    s_ports[0] = (serial_port_t){
        .type = SERIAL_TYPE_UART,
        .name = "UART",
        .available = true,
        .init = uart_bridge_init,
        .send = uart_bridge_send,
        .set_baud = uart_bridge_set_baud_rate,
        .start = uart_bridge_start,
    };
    s_port_count = 1;

#if CONFIG_IDF_TARGET_ESP32S3
    // Port 1: USB CDC-ACM
    s_ports[1] = (serial_port_t){
        .type = SERIAL_TYPE_USB_CDC,
        .name = "USB",
        .available = false,  // Set true when device connects
        .init = usb_cdc_init,
        .send = usb_cdc_send,
        .set_baud = usb_cdc_set_baud_rate,
        .start = usb_cdc_start,
    };
    s_port_count = 2;
#endif

    // Initialize all ports
    for (int i = 0; i < s_port_count; i++) {
        esp_err_t err = s_ports[i].init(i, conf->baud_rate[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Port %d (%s) init failed: %s", i, s_ports[i].name, esp_err_to_name(err));
            return err;
        }
        err = s_ports[i].start(i);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Port %d (%s) start failed: %s", i, s_ports[i].name, esp_err_to_name(err));
            return err;
        }
        ESP_LOGI(TAG, "Port %d (%s) ready, baud=%lu", i, s_ports[i].name, conf->baud_rate[i]);
    }

    return ESP_OK;
}

esp_err_t serial_port_send(int port_index, const uint8_t *data, size_t len)
{
    if (port_index < 0 || port_index >= s_port_count) return ESP_ERR_INVALID_ARG;
    return s_ports[port_index].send(port_index, data, len);
}

esp_err_t serial_port_set_baud_rate(int port_index, uint32_t baud_rate)
{
    if (port_index < 0 || port_index >= s_port_count) return ESP_ERR_INVALID_ARG;
    return s_ports[port_index].set_baud(port_index, baud_rate);
}

void serial_port_set_rx_callback(serial_rx_cb_t cb, void *ctx)
{
    s_rx_cb = cb;
    s_rx_cb_ctx = ctx;
}

int serial_port_count(void)
{
    return s_port_count;
}

const serial_port_t *serial_port_get(int port_index)
{
    if (port_index < 0 || port_index >= s_port_count) return NULL;
    return &s_ports[port_index];
}
