#include "esp_timer.h"
#include "esp_random.h"
#include "freertos/task.h"
#include <stdint.h>

static int64_t s_now_us = 0;

int64_t esp_timer_get_time(void) { return s_now_us; }
void test_clock_set_us(int64_t us) { s_now_us = us; }
void test_clock_advance_us(int64_t us) { s_now_us += us; }

int g_notify_take_result = 0;

/* Deterministic token/salt filler: a 32-byte window stepped by +0x11 per
 * call, so salt and token bytes are known to tests (needed to check the
 * hand-rolled PBKDF2 against fixed hashlib vectors) while tokens still
 * differ between sessions. */
static uint8_t s_random_state[32];
static bool s_random_init = false;

void esp_fill_random(void *buf, size_t len)
{
    if (!s_random_init) {
        test_random_reset();
        s_random_init = true;
    }
    uint8_t *out = buf;
    for (size_t i = 0; i < len; i++) {
        s_random_state[i % sizeof(s_random_state)] += 0x11;
        out[i] = s_random_state[i % sizeof(s_random_state)];
    }
}

void test_random_reset(void)
{
    for (size_t i = 0; i < sizeof(s_random_state); i++) s_random_state[i] = (uint8_t)i;
    s_random_init = true;
}
