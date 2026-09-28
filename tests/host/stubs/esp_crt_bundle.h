#pragma once
#include "esp_err.h"

/* Only referenced as a function pointer in production config structs. */
esp_err_t esp_crt_bundle_attach(void *conf);
