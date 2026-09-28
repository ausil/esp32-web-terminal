/* The serial→WS TX ring, compiled from main/web_server.c itself
 * (see extract_ring.py). Drives enqueue from "producer" calls and the flush
 * via the fake httpd task queue, asserting routing, chunking, overflow and
 * revocation behavior. */
#include "unity.h"
#include "test_common.h"
#include "esp_http_server.h"
#include "esp_timer.h"

/* Provided by the extracted ring TU; ws_add_client is static in production,
 * the bridge exposes the real handshake registration (last_auth_us init). */
extern bool test_ws_add_client(int fd, int port_index, const char *token);
extern void test_ws_reset(void);
extern void web_server_ws_close_all(const char *why);
extern void web_server_ws_broadcast(int port_index, const uint8_t *data, size_t len);
extern void web_server_ws_broadcast_text(const char *text);

void setUp(void)
{
    test_httpd_reset();
    test_ws_reset();   /* ring statics persist between tests otherwise */
}

void tearDown(void) {}

/* Enqueue one frame from a producer context. */
static void produce_binary(int port, const uint8_t *data, size_t len)
{
    web_server_ws_broadcast(port, data, len);
}

/* Drain every queued work item (the fake httpd task catching up). */
static void drain(void)
{
    while (test_httpd_run_work(NULL) == 0) { }
}

static void fd_assert(int idx, int want_fd, uint8_t want_type, const void *data, size_t len)
{
    int fd; uint8_t type; size_t flen; uint8_t buf[256];
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, test_httpd_send_nth(idx, &fd, &type, &flen, buf, sizeof(buf)),
                                  "frame index out of range");
    TEST_ASSERT_EQUAL_INT(want_fd, fd);
    TEST_ASSERT_EQUAL_UINT8(want_type, type);
    TEST_ASSERT_EQUAL_UINT(len, flen);
    if (len) TEST_ASSERT_EQUAL_MEMORY(data, buf, len);  /* Unity rejects len 0 */
}

void test_ring_binary_routes_by_port(void)
{
    test_httpd_ws_register(11);
    test_httpd_ws_register(12);
    test_ws_add_client(11, 0, "tok11");
    test_ws_add_client(12, 1, "tok12");

    uint8_t a[] = {0x01, 0x02, 0x03};
    produce_binary(0, a, sizeof(a));
    TEST_ASSERT_EQUAL_INT(1, test_httpd_work_pending(NULL));  /* one kick */
    drain();
    TEST_ASSERT_EQUAL_INT(1, test_httpd_send_count());
    fd_assert(0, 11, HTTPD_WS_TYPE_BINARY, a, sizeof(a));

    uint8_t b[] = {0xAA};
    produce_binary(1, b, sizeof(b));
    drain();
    TEST_ASSERT_EQUAL_INT(2, test_httpd_send_count());
    fd_assert(1, 12, HTTPD_WS_TYPE_BINARY, b, sizeof(b));
}

void test_ring_text_event_reaches_all(void)
{
    test_httpd_ws_register(11);
    test_httpd_ws_register(12);
    test_ws_add_client(11, 0, "tok11");
    test_ws_add_client(12, 1, "tok12");

    web_server_ws_broadcast_text("{\"event\":\"usb_connected\"}");
    drain();
    TEST_ASSERT_EQUAL_INT(2, test_httpd_send_count());
    fd_assert(0, 11, HTTPD_WS_TYPE_TEXT, "{\"event\":\"usb_connected\"}", 25);
    fd_assert(1, 12, HTTPD_WS_TYPE_TEXT, "{\"event\":\"usb_connected\"}", 25);
}

void test_ring_kick_coalesces(void)
{
    test_httpd_ws_register(11);
    test_ws_add_client(11, 0, "tok");
    uint8_t one[] = {1};
    for (int i = 0; i < 5; i++) produce_binary(0, one, 1);
    /* Five enqueues before the task runs must post a single wake-up. */
    TEST_ASSERT_EQUAL_INT(1, test_httpd_work_pending(NULL));
    drain();
    TEST_ASSERT_EQUAL_INT(5, test_httpd_send_count());
}

void test_ring_large_payload_splits(void)
{
    test_httpd_ws_register(11);
    test_ws_add_client(11, 0, "tok");
    uint8_t big[300];
    for (int i = 0; i < 300; i++) big[i] = (uint8_t)i;
    produce_binary(0, big, sizeof(big));
    drain();
    /* 256 + 44 bytes, concatenated equal to the input */
    TEST_ASSERT_EQUAL_INT(2, test_httpd_send_count());
    uint8_t acc[300];
    int fd; uint8_t type; size_t len;
    fd_assert(0, 11, HTTPD_WS_TYPE_BINARY, big, 256);
    fd_assert(1, 11, HTTPD_WS_TYPE_BINARY, big + 256, 44);
    (void)acc; (void)fd; (void)type; (void)len;
}

void test_ring_overflow_drops_and_recovers(void)
{
    test_httpd_ws_register(11);
    test_ws_add_client(11, 0, "tok");
    uint8_t one[] = {7};
    /* Ring is 32 slots; the flush never runs between enqueues, so #33 must
     * drop rather than block or corrupt (busy httpd task scenario). */
    for (int i = 0; i < 32; i++) produce_binary(0, one, 1);
    TEST_ASSERT_EQUAL_INT(1, test_httpd_work_pending(NULL));
    produce_binary(0, one, 1);          /* #33: dropped, no crash */
    drain();
    TEST_ASSERT_EQUAL_INT(32, test_httpd_send_count());
    /* ...and the ring works again afterward */
    produce_binary(0, one, 1);
    drain();
    TEST_ASSERT_EQUAL_INT(33, test_httpd_send_count());
}

/* Revocation: a client whose session died must stop receiving on the next
 * push (throttled revalidation) and get a close frame. */
void test_ring_revokes_dead_sessions(void)
{
    test_httpd_ws_register(11);
    TEST_ASSERT_TRUE(test_ws_add_client(11, 0, "BAD"));   /* BAD = invalid token */
    /* Past the 1s revalidation throttle so the push path actually checks. */
    test_clock_advance_us(1100000);
    uint8_t one[] = {1};
    produce_binary(0, one, 1);
    drain();
    /* First push evicts (close frame) and skips the payload... */
    TEST_ASSERT_EQUAL_INT(1, test_httpd_send_count());
    fd_assert(0, 11, HTTPD_WS_TYPE_CLOSE, NULL, 0);
    /* ...later pushes go nowhere. */
    test_clock_advance_us(1100000);
    produce_binary(0, one, 1);
    drain();
    TEST_ASSERT_EQUAL_INT(1, test_httpd_send_count());
}

/* Send failures (fd not a live session — recycled socket) mark the client
 * inactive so the ring stops pointing at it. */
void test_ring_send_failure_deactivates(void)
{
    test_ws_add_client(11, 0, "tok");   /* fd 11 never registered as live session */
    uint8_t one[] = {1};
    produce_binary(0, one, 1);
    drain();
    TEST_ASSERT_EQUAL_INT(0, test_httpd_send_count());  /* all sends failed */
    /* Client is gone now; enqueue still succeeds (no wedge), sends still 0 */
    produce_binary(0, one, 1);
    drain();
    TEST_ASSERT_EQUAL_INT(0, test_httpd_send_count());
}

/* Password-change teardown: every active client gets a close frame. */
void test_close_all_sends_close_frames(void)
{
    test_httpd_ws_register(11);
    test_httpd_ws_register(12);
    test_ws_add_client(11, 0, "tok11");
    test_ws_add_client(12, 1, "tok12");
    web_server_ws_close_all("credentials changed");
    TEST_ASSERT_EQUAL_INT(2, test_httpd_send_count());
    fd_assert(0, 11, HTTPD_WS_TYPE_CLOSE, NULL, 0);
    fd_assert(1, 12, HTTPD_WS_TYPE_CLOSE, NULL, 0);
    /* after close_all, broadcasts reach nobody */
    uint8_t one[] = {1};
    produce_binary(0, one, 1);
    drain();
    TEST_ASSERT_EQUAL_INT(2, test_httpd_send_count());
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_ring_binary_routes_by_port);
    RUN_TEST(test_ring_text_event_reaches_all);
    RUN_TEST(test_ring_kick_coalesces);
    RUN_TEST(test_ring_large_payload_splits);
    RUN_TEST(test_ring_overflow_drops_and_recovers);
    RUN_TEST(test_ring_revokes_dead_sessions);
    RUN_TEST(test_ring_send_failure_deactivates);
    RUN_TEST(test_close_all_sends_close_frames);
    return UNITY_END();
}
