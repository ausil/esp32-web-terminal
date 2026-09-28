/* Fake httpd task + WS session/send capture for the ring tests. */
#include "esp_http_server.h"
#include <string.h>

#define FAKE_SENDS      128
#define FAKE_MAX_PAYLOAD 256   /* ring slot cap — frames never exceed it */
#define FAKE_WORK       64
#define FAKE_SESSIONS   8

typedef struct {
    int fd;
    uint8_t type;
    size_t len;
    uint8_t data[FAKE_MAX_PAYLOAD];
} sent_t;

static sent_t s_sends[FAKE_SENDS];
static int s_send_count;
static int s_sessions[FAKE_SESSIONS];
static int s_session_count;

typedef struct { void (*fn)(void *); void *arg; } work_t;
static work_t s_work[FAKE_WORK];
static int s_work_head, s_work_count;

void test_httpd_reset(void)
{
    memset(s_sends, 0, sizeof(s_sends));
    s_send_count = 0;
    s_session_count = 0;
    s_work_head = s_work_count = 0;
}

void test_httpd_ws_register(int fd)
{
    if (s_session_count < FAKE_SESSIONS) s_sessions[s_session_count++] = fd;
}

static bool fd_live(int fd)
{
    for (int i = 0; i < s_session_count; i++) {
        if (s_sessions[i] == fd) return true;
    }
    return false;
}

esp_err_t httpd_ws_send_frame_async(httpd_handle_t handle, int fd, httpd_ws_frame_t *pkt)
{
    (void)handle;
    if (!fd_live(fd)) return ESP_FAIL;      /* like a recycled/untracked socket */
    if (s_send_count >= FAKE_SENDS) return ESP_FAIL;
    sent_t *s = &s_sends[s_send_count++];
    s->fd = fd;
    s->type = pkt->type;
    s->len = pkt->len;
    if (pkt->len && pkt->payload) {
        memcpy(s->data, pkt->payload, pkt->len < FAKE_MAX_PAYLOAD ? pkt->len : FAKE_MAX_PAYLOAD);
    }
    return ESP_OK;
}

esp_err_t httpd_queue_work(httpd_handle_t handle, void (*work)(void *), void *arg)
{
    (void)handle;
    if (s_work_count >= FAKE_WORK) return ESP_FAIL;
    s_work[(s_work_head + s_work_count) % FAKE_WORK].fn = work;
    s_work[(s_work_head + s_work_count) % FAKE_WORK].arg = arg;
    s_work_count++;
    return ESP_OK;
}

int test_httpd_work_pending(httpd_handle_t handle)
{
    (void)handle;
    return s_work_count;
}

int test_httpd_run_work(httpd_handle_t handle)
{
    (void)handle;
    if (s_work_count == 0) return -1;
    work_t w = s_work[s_work_head];
    s_work_head = (s_work_head + 1) % FAKE_WORK;
    s_work_count--;
    w.fn(w.arg);
    return 0;
}

int test_httpd_send_count(void) { return s_send_count; }

int test_httpd_send_nth(int i, int *fd, uint8_t *type, size_t *len,
                        uint8_t *data, size_t data_cap)
{
    if (i < 0 || i >= s_send_count) return -1;
    if (fd) *fd = s_sends[i].fd;
    if (type) *type = s_sends[i].type;
    if (len) *len = s_sends[i].len;
    if (data && data_cap) {
        size_t n = s_sends[i].len < data_cap ? s_sends[i].len : data_cap;
        memcpy(data, s_sends[i].data, n);
    }
    return 0;
}
