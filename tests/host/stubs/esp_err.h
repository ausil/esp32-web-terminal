/* Host-test stubs — minimal stand-ins so production sources compile against
 * plain gcc. Kept deliberately small: if production code starts using an IDF
 * API that isn't here, the host build fails and tells us. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef int esp_err_t;

#define ESP_OK                 0
#define ESP_FAIL               -1
#define ESP_ERR_NO_MEM         0x101
#define ESP_ERR_INVALID_ARG    0x102
#define ESP_ERR_INVALID_STATE  0x103
#define ESP_ERR_NOT_FOUND      0x105
#define ESP_ERR_NOT_SUPPORTED  0x106
#define ESP_ERR_INVALID_SIZE   0x10F
#define ESP_ERR_TIMEOUT        0x107
#define ESP_ERR_WIFI_CONN      0x3003

const char *esp_err_to_name(esp_err_t err);

#define ESP_ERROR_CHECK(x) do { (void)(x); } while (0)
