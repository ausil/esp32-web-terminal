#pragma once
#include <stddef.h>
#include <stdint.h>

/* Deterministic-by-default filler: bytes advance through a per-call counter
 * so tokens differ but are reproducible across runs. */
void esp_fill_random(void *buf, size_t len);
void test_random_reset(void);
