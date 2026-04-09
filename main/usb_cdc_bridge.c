// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dennis Gilmore

#include "usb_cdc_bridge.h"
#include "serial_port.h"
#include "web_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "usb/usb_host.h"
#include "usb/cdc_acm_host.h"
#include "usb/vcp_ch34x.h"
#include "usb/vcp_cp210x.h"
#include "usb/vcp_ftdi.h"

static const char *TAG = "usb_cdc";

#define USB_HOST_PRIORITY  20
#define TX_TIMEOUT_MS      1000

static int s_port_index = -1;
static uint32_t s_baud_rate = 115200;
static cdc_acm_dev_hdl_t s_cdc_dev = NULL;
static QueueHandle_t s_event_queue = NULL;
static TaskHandle_t s_usb_lib_task = NULL;
static TaskHandle_t s_event_task = NULL;

typedef struct {
    enum {
        USB_EVT_CONNECTED,
        USB_EVT_DISCONNECTED,
    } id;
    uint16_t vid;
    uint16_t pid;
} usb_event_t;

// --- Callbacks (called from USB host context) ---

static bool handle_rx(const uint8_t *data, size_t data_len, void *arg)
{
    if (s_port_index >= 0) {
        serial_port_rx_notify(s_port_index, data, data_len);
    }
    return true;
}

static void handle_event(const cdc_acm_host_dev_event_data_t *event, void *user_ctx)
{
    switch (event->type) {
    case CDC_ACM_HOST_ERROR:
        ESP_LOGE(TAG, "CDC-ACM error: %d", event->data.error);
        break;
    case CDC_ACM_HOST_DEVICE_DISCONNECTED: {
        usb_event_t msg = { .id = USB_EVT_DISCONNECTED };
        xQueueSend(s_event_queue, &msg, 0);
        break;
    }
    case CDC_ACM_HOST_SERIAL_STATE:
        ESP_LOGD(TAG, "Serial state: 0x%04X", event->data.serial_state.val);
        break;
    default:
        break;
    }
}

static void new_dev_cb(usb_device_handle_t usb_dev)
{
    const usb_device_desc_t *desc;
    if (usb_host_get_device_descriptor(usb_dev, &desc) != ESP_OK) return;

    ESP_LOGI(TAG, "USB device connected: VID=0x%04X PID=0x%04X", desc->idVendor, desc->idProduct);
    usb_event_t msg = {
        .id = USB_EVT_CONNECTED,
        .vid = desc->idVendor,
        .pid = desc->idProduct,
    };
    xQueueSend(s_event_queue, &msg, 0);
}

// --- Device open helper (selects correct driver by VID) ---

static cdc_acm_dev_hdl_t open_device(uint16_t vid, uint16_t pid,
                                      const cdc_acm_host_device_config_t *cfg)
{
    cdc_acm_dev_hdl_t dev = NULL;
    esp_err_t err;

    switch (vid) {
    case FTDI_VID:
        err = ftdi_vcp_open(pid, 0, cfg, &dev);
        break;
    case NANJING_QINHENG_MICROE_VID:
        err = ch34x_vcp_open(pid, 0, cfg, &dev);
        break;
    case SILICON_LABS_VID:
        err = cp210x_vcp_open(pid, 0, cfg, &dev);
        break;
    default:
        err = cdc_acm_host_open(vid, pid, 0, cfg, &dev);
        break;
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open VID=0x%04X PID=0x%04X: %s", vid, pid, esp_err_to_name(err));
        return NULL;
    }
    return dev;
}

// --- USB Host library task ---

static void usb_lib_task(void *arg)
{
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LOWMED,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_config));

    const cdc_acm_host_driver_config_t driver_config = {
        .driver_task_stack_size = 4096,
        .driver_task_priority = USB_HOST_PRIORITY + 1,
        .xCoreID = 0,
        .new_dev_cb = new_dev_cb,
    };
    ESP_ERROR_CHECK(cdc_acm_host_install(&driver_config));

    xTaskNotifyGive(arg);

    while (1) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
    }
}

// --- Event processing task ---

static void usb_event_task(void *arg)
{
    cdc_acm_host_device_config_t dev_config = {
        .connection_timeout_ms = 5000,
        .out_buffer_size = 512,
        .in_buffer_size = 0,
        .user_arg = NULL,
        .event_cb = handle_event,
        .data_cb = handle_rx,
    };

    usb_event_t msg;
    while (1) {
        if (xQueueReceive(s_event_queue, &msg, portMAX_DELAY) != pdTRUE) continue;

        switch (msg.id) {
        case USB_EVT_CONNECTED: {
            if (s_cdc_dev != NULL) {
                ESP_LOGW(TAG, "Already have a device open, ignoring new connection");
                break;
            }

            cdc_acm_dev_hdl_t dev = open_device(msg.vid, msg.pid, &dev_config);
            if (dev == NULL) break;

            // Set line coding (baud rate)
            cdc_acm_line_coding_t coding = {
                .dwDTERate = s_baud_rate,
                .bCharFormat = 0,  // 1 stop bit
                .bParityType = 0,  // No parity
                .bDataBits = 8,
            };
            esp_err_t err = cdc_acm_host_line_coding_set(dev, &coding);
            if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED) {
                ESP_LOGW(TAG, "Failed to set line coding: %s", esp_err_to_name(err));
            }

            // Set DTR + RTS
            err = cdc_acm_host_set_control_line_state(dev, true, true);
            if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED) {
                ESP_LOGW(TAG, "Failed to set control line state: %s", esp_err_to_name(err));
            }

            s_cdc_dev = dev;

            // Update port availability
            serial_port_t *sp = (serial_port_t *)serial_port_get(s_port_index);
            if (sp) sp->available = true;

            ESP_LOGI(TAG, "USB CDC device ready (baud=%lu)", s_baud_rate);
            web_server_ws_broadcast_text("{\"event\":\"usb_connected\"}");
            break;
        }

        case USB_EVT_DISCONNECTED: {
            ESP_LOGI(TAG, "USB CDC device disconnected");
            if (s_cdc_dev) {
                cdc_acm_host_close(s_cdc_dev);
                s_cdc_dev = NULL;
            }

            serial_port_t *sp = (serial_port_t *)serial_port_get(s_port_index);
            if (sp) sp->available = false;

            web_server_ws_broadcast_text("{\"event\":\"usb_disconnected\"}");
            break;
        }
        }
    }
}

// --- Public API (serial_port vtable) ---

esp_err_t usb_cdc_init(int port_index, uint32_t baud_rate)
{
    s_port_index = port_index;
    s_baud_rate = baud_rate;

    s_event_queue = xQueueCreate(5, sizeof(usb_event_t));
    if (!s_event_queue) return ESP_ERR_NO_MEM;

    ESP_LOGI(TAG, "USB CDC bridge initialized (port %d, baud=%lu)", port_index, baud_rate);
    return ESP_OK;
}

esp_err_t usb_cdc_start(int port_index)
{
    // Start USB host library task
    BaseType_t ret = xTaskCreate(usb_lib_task, "usb_lib", 4096,
                                  xTaskGetCurrentTaskHandle(), USB_HOST_PRIORITY, &s_usb_lib_task);
    if (ret != pdPASS) return ESP_FAIL;

    // Wait for USB host + CDC-ACM driver to be installed
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));

    // Start event processing task
    ret = xTaskCreate(usb_event_task, "usb_evt", 4096, NULL, 10, &s_event_task);
    if (ret != pdPASS) return ESP_FAIL;

    ESP_LOGI(TAG, "USB CDC host started, waiting for devices...");
    return ESP_OK;
}

esp_err_t usb_cdc_send(int port_index, const uint8_t *data, size_t len)
{
    if (!s_cdc_dev) return ESP_ERR_INVALID_STATE;
    return cdc_acm_host_data_tx_blocking(s_cdc_dev, data, len, TX_TIMEOUT_MS);
}

esp_err_t usb_cdc_set_baud_rate(int port_index, uint32_t baud_rate)
{
    s_baud_rate = baud_rate;

    if (s_cdc_dev) {
        cdc_acm_line_coding_t coding = {
            .dwDTERate = baud_rate,
            .bCharFormat = 0,
            .bParityType = 0,
            .bDataBits = 8,
        };
        esp_err_t err = cdc_acm_host_line_coding_set(s_cdc_dev, &coding);
        if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGE(TAG, "Failed to set baud rate: %s", esp_err_to_name(err));
            return err;
        }
    }

    ESP_LOGI(TAG, "Baud rate set to %lu", baud_rate);
    return ESP_OK;
}

bool usb_cdc_is_connected(void)
{
    return s_cdc_dev != NULL;
}
