#pragma once
#include <stdint.h>

/* Fake monotonic clock: tests move it to drive session expiry and the WS
 * revalidation throttle without sleeping. */
int64_t esp_timer_get_time(void);
void test_clock_set_us(int64_t us);
void test_clock_advance_us(int64_t us);
