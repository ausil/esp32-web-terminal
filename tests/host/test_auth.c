/* auth.c — session lifecycle, per-IP lockout, token extraction (incl. the
 * xsession= cookie regression), monotonic-clock expiry. */
#include "unity.h"
#include "test_common.h"
#include "auth.h"
#include "config.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_timer.h"
#include "req_fake.h"
#include <stdlib.h>
#include <string.h>

static char *tok(const char *user, const char *pass, const char *ip)
{
    return auth_login(user, pass, ip);
}

void setUp(void)
{
    test_nvs_clear();
    test_random_reset();
    test_clock_set_us(0);
    TEST_ESP_OK(config_init());
    TEST_ESP_OK(auth_init());
}

void tearDown(void) {}

void test_login_success_wrong_password(void)
{
    char *t = tok("admin", "admin", "192.168.1.10");
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_EQUAL_UINT(64, strlen(t));
    TEST_ASSERT_TRUE(auth_validate_session(t));
    free(t);
    TEST_ASSERT_NULL(tok("admin", "nope", "192.168.1.10"));
    TEST_ASSERT_NULL(tok("root", "admin", "192.168.1.10"));
}

void test_logout(void)
{
    char *t = tok("admin", "admin", "10.0.0.1");
    TEST_ASSERT_TRUE(auth_validate_session(t));
    auth_logout(t);
    TEST_ASSERT_FALSE(auth_validate_session(t));
    free(t);
}

void test_invalidate_all_on_password_change(void)
{
    char *t1 = tok("admin", "admin", "10.0.0.1");
    char *t2 = tok("admin", "admin", "10.0.0.2");
    TEST_ASSERT_NOT_NULL(t1); TEST_ASSERT_NOT_NULL(t2);
    auth_invalidate_all_sessions();
    TEST_ASSERT_FALSE(auth_validate_session(t1));
    TEST_ASSERT_FALSE(auth_validate_session(t2));
    free(t1); free(t2);
}

/* Session table is 4 slots; the 5th login must evict the oldest, not fail.
 * Advance the clock between logins so "oldest" is deterministic. */
void test_session_eviction(void)
{
    char *toks[AUTH_MAX_SESSIONS + 1];
    for (int i = 0; i < AUTH_MAX_SESSIONS; i++) {
        test_clock_advance_us(1000000);
        toks[i] = tok("admin", "admin", "10.0.0.1");
        TEST_ASSERT_NOT_NULL(toks[i]);
    }
    test_clock_advance_us(1000000);
    toks[AUTH_MAX_SESSIONS] = tok("admin", "admin", "10.0.0.1");
    TEST_ASSERT_NOT_NULL(toks[AUTH_MAX_SESSIONS]);
    /* oldest evicted, the rest still valid */
    TEST_ASSERT_FALSE(auth_validate_session(toks[0]));
    for (int i = 1; i < AUTH_MAX_SESSIONS; i++) {
        TEST_ASSERT_TRUE(auth_validate_session(toks[i]));
    }
    TEST_ASSERT_TRUE(auth_validate_session(toks[AUTH_MAX_SESSIONS]));
    for (int i = 0; i <= AUTH_MAX_SESSIONS; i++) free(toks[i]);
}

/* Timeout uses the monotonic clock (esp_timer), NOT wall time: a 3601s jump
 * must expire, and an NTP-style wall-clock leap (we just keep using the fake
 * monotonic clock) must not expire sessions early. */
void test_session_expiry_monotonic(void)
{
    char *t = tok("admin", "admin", "10.0.0.1");
    test_clock_advance_us((int64_t)(AUTH_SESSION_TIMEOUT_S - 60) * 1000000);
    TEST_ASSERT_TRUE(auth_validate_session(t));
    test_clock_advance_us(120LL * 1000000);
    TEST_ASSERT_FALSE(auth_validate_session(t));
    free(t);
}

/* Lockout is per-address: attacker on .66 must not lock the owner out on .10
 * (a global counter turned the lockout into a DoS button, see auth.c). */
void test_lockout_per_ip(void)
{
    for (int i = 0; i < AUTH_MAX_FAILED; i++) {
        TEST_ASSERT_NULL(tok("admin", "bad", "192.168.1.66"));
    }
    TEST_ASSERT_TRUE(auth_is_locked_out("192.168.1.66"));
    TEST_ASSERT_NULL(tok("admin", "admin", "192.168.1.66"));  /* correct pass during lockout */
    TEST_ASSERT_FALSE(auth_is_locked_out("192.168.1.10"));
    char *t = tok("admin", "admin", "192.168.1.10");
    TEST_ASSERT_NOT_NULL(t);
    free(t);
}

void test_lockout_expires(void)
{
    for (int i = 0; i < AUTH_MAX_FAILED; i++) TEST_ASSERT_NULL(tok("admin", "bad", "1.2.3.4"));
    TEST_ASSERT_TRUE(auth_is_locked_out("1.2.3.4"));
    test_clock_advance_us((int64_t)(AUTH_LOCKOUT_S + 1) * 1000000);
    TEST_ASSERT_FALSE(auth_is_locked_out("1.2.3.4"));
    char *t = tok("admin", "admin", "1.2.3.4");
    TEST_ASSERT_NOT_NULL(t);
    free(t);
}

/* A success clears the failure counter, so a slow attacker (4 fails, success,
 * 4 fails, ...) never locks out. */
void test_success_clears_counter(void)
{
    for (int i = 0; i < AUTH_MAX_FAILED - 1; i++) TEST_ASSERT_NULL(tok("admin", "bad", "5.6.7.8"));
    char *t = tok("admin", "admin", "5.6.7.8");
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_FALSE(auth_is_locked_out("5.6.7.8"));
    free(t);
    for (int i = 0; i < AUTH_MAX_FAILED - 1; i++) TEST_ASSERT_NULL(tok("admin", "bad", "5.6.7.8"));
    TEST_ASSERT_FALSE(auth_is_locked_out("5.6.7.8"));
}

/* NULL address is accepted and shares one bucket (no crash). */
void test_null_ip_shared_bucket(void)
{
    for (int i = 0; i < AUTH_MAX_FAILED; i++) TEST_ASSERT_NULL(tok("admin", "bad", NULL));
    TEST_ASSERT_TRUE(auth_is_locked_out(NULL));
}

/* --- token extraction (real request accessors via req_fake) --- */

void test_cookie_extraction(void)
{
    httpd_req_t *r = test_req_make();
    test_req_set_cookie(r, "session=abc123; theme=dark");
    char *t = auth_get_token_from_request(r);
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_EQUAL_STRING("abc123", t);
    free(t);

    /* other cookies first, and session last */
    test_req_set_cookie(r, "theme=dark; session=xyz789");
    t = auth_get_token_from_request(r);
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_EQUAL_STRING("xyz789", t);
    free(t);
    test_req_free(r);
}

/* Regression: a bare strstr("session=") matched "xsession=" and grabbed the
 * wrong value. The parser must require the name to start the header or follow
 * a "; " separator. */
void test_xsession_cookie_not_mistaken(void)
{
    httpd_req_t *r = test_req_make();

    test_req_set_cookie(r, "xsession=WRONG");
    char *t = auth_get_token_from_request(r);
    TEST_ASSERT_NULL(t);   /* no session cookie here at all */

    test_req_set_cookie(r, "xsession=nope; session=right");
    t = auth_get_token_from_request(r);
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_EQUAL_STRING("right", t);
    free(t);

    /* session= as a substring of a path-ish cookie value is still not a name */
    test_req_set_cookie(r, "link=/foo?session=trap; other=1");
    t = auth_get_token_from_request(r);
    TEST_ASSERT_NULL(t);

    test_req_free(r);
}

void test_bearer_extraction(void)
{
    httpd_req_t *r = test_req_make();
    test_req_set_authorization(r, "Bearer deadbeef");
    char *t = auth_get_token_from_request(r);
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_EQUAL_STRING("deadbeef", t);
    free(t);
    /* wrong scheme must be ignored */
    test_req_set_authorization(r, "Basic deadbeef");
    TEST_ASSERT_NULL(auth_get_token_from_request(r));
    /* cookie wins when both present */
    test_req_set_cookie(r, "session=fromcookie");
    test_req_set_authorization(r, "Bearer fromheader");
    t = auth_get_token_from_request(r);
    TEST_ASSERT_EQUAL_STRING("fromcookie", t);
    free(t);
    test_req_free(r);
}

void test_check_request_roundtrip(void)
{
    char *t = tok("admin", "admin", "10.1.2.3");
    httpd_req_t *r = test_req_make();
    char cookie[128];
    snprintf(cookie, sizeof(cookie), "session=%s; Path=/; HttpOnly", t);
    test_req_set_cookie(r, cookie);
    TEST_ASSERT_TRUE(auth_check_request(r));
    auth_logout(t);
    TEST_ASSERT_FALSE(auth_check_request(r));
    test_req_free(r);
    free(t);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_login_success_wrong_password);
    RUN_TEST(test_logout);
    RUN_TEST(test_invalidate_all_on_password_change);
    RUN_TEST(test_session_eviction);
    RUN_TEST(test_session_expiry_monotonic);
    RUN_TEST(test_lockout_per_ip);
    RUN_TEST(test_lockout_expires);
    RUN_TEST(test_success_clears_counter);
    RUN_TEST(test_null_ip_shared_bucket);
    RUN_TEST(test_cookie_extraction);
    RUN_TEST(test_xsession_cookie_not_mistaken);
    RUN_TEST(test_bearer_extraction);
    RUN_TEST(test_check_request_roundtrip);
    return UNITY_END();
}
