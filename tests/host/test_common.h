/* Shared bits for the host suites. TEST_ESP_OK comes from esp_idf's unity
 * fixture layer, which the vendored core Unity doesn't include. Also pulls
 * in the test-control hooks every suite reaches for (fake clock, deterministic
 * random, fake NVS) so suites don't each re-include the stub headers. */
#pragma once
#include "unity.h"
#include "esp_err.h"
#include "esp_timer.h"    /* test_clock_set_us / test_clock_advance_us */
#include "esp_random.h"   /* test_random_reset */
#include "nvs.h"          /* test_nvs_clear */

#define TEST_ESP_OK(expr) \
    TEST_ASSERT_EQUAL_INT_MESSAGE(ESP_OK, (expr), #expr " != ESP_OK")
