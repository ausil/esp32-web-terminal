// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dennis Gilmore

#include "uart_bridge.h"
#include "serial_port.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "uart_bridge";

// UART1 is the only UART bridge instance on all targets
#define UART_BRIDGE_PORT  UART_NUM_1

static uint32_t s_baud_rate = 115200;
static TaskHandle_t s_rx_task_handle = NULL;
static int s_port_index = 0;

static void uart_rx_task(void *arg)
{
    int port_index = (int)(intptr_t)arg;
    uint8_t buf[256];

    while (1) {
        int len = uart_read_bytes(UART_BRIDGE_PORT, buf, sizeof(buf), pdMS_TO_TICKS(50));
        if (len > 0) {
            serial_port_rx_notify(port_index, buf, len);
        }
    }
}

esp_err_t uart_bridge_init(int port_index, uint32_t baud_rate)
{
    s_baud_rate = baud_rate;
    s_port_index = port_index;

    uart_config_t uart_config = {
        .baud_rate = (int)baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(UART_BRIDGE_PORT, UART_BRIDGE_BUF_SIZE * 2,
                                         UART_BRIDGE_BUF_SIZE, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_BRIDGE_PORT, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_BRIDGE_PORT, UART_BRIDGE_TX_PIN, UART_BRIDGE_RX_PIN,
                                  UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    ESP_LOGI(TAG, "UART%d initialized: baud=%lu, TX=GPIO%d, RX=GPIO%d",
             UART_BRIDGE_PORT, baud_rate, UART_BRIDGE_TX_PIN, UART_BRIDGE_RX_PIN);
    return ESP_OK;
}

esp_err_t uart_bridge_set_baud_rate(int port_index, uint32_t baud_rate)
{
    esp_err_t err = uart_set_baudrate(UART_BRIDGE_PORT, baud_rate);
    if (err == ESP_OK) {
        s_baud_rate = baud_rate;
        ESP_LOGI(TAG, "Baud rate changed to %lu", baud_rate);
    } else {
        ESP_LOGE(TAG, "Failed to set baud rate: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t uart_bridge_send(int port_index, const uint8_t *data, size_t len)
{
    int written = uart_write_bytes(UART_BRIDGE_PORT, data, len);
    if (written < 0) {
        ESP_LOGE(TAG, "UART write failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t uart_bridge_start(int port_index)
{
    if (s_rx_task_handle != NULL) {
        ESP_LOGW(TAG, "RX task already running");
        return ESP_OK;
    }

    BaseType_t ret = xTaskCreate(uart_rx_task, "uart_rx", 3072,
                                  (void *)(intptr_t)port_index, 10, &s_rx_task_handle);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create UART RX task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "UART RX task started");
    return ESP_OK;
}
