#!/usr/bin/env python3
"""Extract the WS TX ring from main/web_server.c into a standalone TU.

The ring (slot struct, state, ws_enqueue, ws_flush_work) lives in web_server.c
because that is where production uses it. Host tests exercise the *real*
source — if web_server.c ever loses the markers or renames the functions,
this script fails loudly rather than silently testing a stale copy.

Emitted TU defines the production symbols directly (they are file-static in
web_server.c, so the test links its own copy) and expects test scaffolding
headers (test_ring_shim.h) to provide s_server, s_ws_mutex, s_ws_clients,
MAX_WS_CLIENTS, ws_revalidate_locked and TAG before this content.
"""
import os
import re
import sys

SRC = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                   "..", "..", "main", "web_server.c")
START = "/* --- Serial→WS TX ring ---"
END = "void web_server_ws_broadcast(int port_index,"

# Ring constants that sit ABOVE the extraction window. Emit them before the
# shim include so the test tracks production values; a rename here fails the
# build loudly instead of silently testing against a stale copy.
CLIP_DEFINES = ("MAX_WS_CLIENTS", "WS_AUTH_RECHECK_US")

def main():
    with open(SRC) as f:
        text = f.read()
    i = text.index(START)
    j = text.index(END)
    ring = text[i:j]
    for name in CLIP_DEFINES:
        m = re.search(r"^#define\s+%s\s+\S+.*$" % name, text, re.M)
        if not m:
            sys.exit("extract_ring.py: #define %s not found in %s "
                     "(renamed or moved?)" % (name, SRC))
        print(m.group(0))
    print("#include \"test_ring_shim.h\"")
    print(ring)
    # The two public wrappers sit below the extraction window in production;
    # reproduce them so the extracted TU covers the same entrypoints.
    print(r'''
void web_server_ws_broadcast(int port_index, const uint8_t *data, size_t len)
{
    ws_enqueue(data, len, port_index);
}

void web_server_ws_broadcast_text(const char *text)
{
    ws_enqueue((const uint8_t *)text, strlen(text), -1);
}

/* Test bridge: ws_add_client is static in production; tests need the real
 * handshake registration path (it initializes last_auth_us). */
bool test_ws_add_client(int fd, int port_index, const char *token)
{
    return ws_add_client(fd, port_index, token);
}

/* Test bridge: zero every piece of ring/client state between tests. These
 * are file-statics in the extracted TU — only code compiled alongside them
 * can do this. */
void test_ws_reset(void)
{
    memset(s_ws_clients, 0, sizeof(s_ws_clients));
    taskENTER_CRITICAL(&s_tx_mux);
    s_tx_head = s_tx_tail = 0;
    s_tx_kick_pending = false;
    s_tx_dropped = 0;
    taskEXIT_CRITICAL(&s_tx_mux);
}
''')

if __name__ == "__main__":
    sys.exit(main())
