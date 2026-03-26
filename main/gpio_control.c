// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dennis Gilmore

#include "gpio_control.h"
#include "config.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "gpio_ctrl";

#define RESET_PULSE_MS      100
#define MIN_POWER_TOGGLE_MS 2000  // Minimum interval between power toggles

static gpio_status_t s_status = {0};

esp_err_t gpio_control_init(void)
{
    // Reset pin: start as input (high-Z), the SBC has a pull-up
    gpio_config_t reset_conf = {
        .pin_bit_mask = (1ULL << GPIO_SBC_RESET),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&reset_conf));

    // Power pin: output, initially set based on config
    gpio_config_t power_conf = {
        .pin_bit_mask = (1ULL << GPIO_SBC_POWER),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&power_conf));

    app_config_t *conf = config_get();
    if (conf->power_on_default) {
        gpio_set_level(GPIO_SBC_POWER, 1);
        s_status.power_on = true;
    } else {
        gpio_set_level(GPIO_SBC_POWER, 0);
        s_status.power_on = false;
    }

    s_status.last_reset_time = 0;
    s_status.last_power_toggle = 0;

    ESP_LOGI(TAG, "GPIO initialized: power=%s", s_status.power_on ? "ON" : "OFF");
    return ESP_OK;
}

esp_err_t gpio_sbc_reset(void)
{
    ESP_LOGI(TAG, "Resetting SBC (pulse low %dms)", RESET_PULSE_MS);

    // Switch to output, drive low
    gpio_set_direction(GPIO_SBC_RESET, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_SBC_RESET, 0);

    vTaskDelay(pdMS_TO_TICKS(RESET_PULSE_MS));

    // Release back to high-Z (input)
    gpio_set_direction(GPIO_SBC_RESET, GPIO_MODE_INPUT);

    s_status.last_reset_time = esp_timer_get_time();
    ESP_LOGI(TAG, "SBC reset complete");
    return ESP_OK;
}

esp_err_t gpio_sbc_power_on(void)
{
    gpio_set_level(GPIO_SBC_POWER, 1);
    s_status.power_on = true;
    s_status.last_power_toggle = esp_timer_get_time();
    ESP_LOGI(TAG, "SBC power ON");
    return ESP_OK;
}

esp_err_t gpio_sbc_power_off(void)
{
    gpio_set_level(GPIO_SBC_POWER, 0);
    s_status.power_on = false;
    s_status.last_power_toggle = esp_timer_get_time();
    ESP_LOGI(TAG, "SBC power OFF");
    return ESP_OK;
}

esp_err_t gpio_sbc_power_toggle(void)
{
    int64_t now = esp_timer_get_time();
    if (s_status.last_power_toggle > 0 &&
        (now - s_status.last_power_toggle) < (MIN_POWER_TOGGLE_MS * 1000)) {
        ESP_LOGW(TAG, "Power toggle too fast, ignoring");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_status.power_on) {
        return gpio_sbc_power_off();
    } else {
        return gpio_sbc_power_on();
    }
}

gpio_status_t gpio_get_status(void)
{
    return s_status;
}
