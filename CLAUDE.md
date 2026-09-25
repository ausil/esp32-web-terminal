# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

ESP32 Web Terminal — web-based serial terminal for SBCs (Raspberry Pi, etc.) via ESP32 over WiFi.
Supported boards: ESP32-C6-DevKitC-1 (8MB), ESP32-C3 Super Mini (4MB), XIAO ESP32S3 (8MB).

## Build

Requires ESP-IDF v5.5+. Before first build, generate TLS certs:

```bash
cd certs && ./generate_cert.sh && cd ..
```

Build and flash (choose target):

```bash
idf.py set-target esp32c6   # or esp32c3 or esp32s3
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

If sdkconfig.defaults changes or switching targets, do a full clean rebuild:

```bash
rm sdkconfig && idf.py fullclean && idf.py set-target esp32c6 && idf.py build  # or esp32c3 or esp32s3
```

## Architecture

```
main/
  main.c            - Entry point: init all subsystems, NTP setup, OTA rollback verification
  config.c/h        - NVS persistent config (WiFi, baud, auth, device name, NTP, GPIO defaults)
  wifi_manager.c/h  - AP+STA WiFi with auto-fallback, mDNS, DHCP hostname, reconnect watchdog
  auth.c/h          - Session-based auth, PBKDF2-HMAC-SHA256, rate limiting
  serial_port.c/h   - Port abstraction layer (UART + USB CDC-ACM), multi-port dispatch
  uart_bridge.c/h   - UART1 ↔ WebSocket bridge, configurable baud (pins vary by target)
  usb_cdc_bridge.c/h - USB Host CDC-ACM bridge (ESP32-S3 only), hot-plug, VCP drivers
  web_server.c/h    - HTTPS server, REST API, WebSocket (per-port routing), OTA, sysinfo
  ota_github.c/h    - GitHub release check + firmware download over HTTPS
  gpio_control.c/h  - SBC reset and power control (pins vary by target)
hardware/
  DESIGN.md         - C6 HAT PCB design
  WIRING-C3-MINI.md - C3 Super Mini wiring guide
frontend/
  index.html        - Single-page terminal UI (all CSS, app JS, FitAddon inlined).
                      THE ONLY SERVED COPY - behaviour changes go here
  terminal.js       - WebSocket client, login, toolbar controls (reference copy: not
                      embedded, not served, lags index.html - never edit this expecting effect)
  style.css         - Dark theme styling (reference copy: same caveats as terminal.js)
  lib/xterm.min.js.gz - Served pre-gzipped via EMBED_FILES
certs/
  generate_cert.sh  - Generates self-signed ECC P-256 cert for TLS
tools/
  factory_flash.py  - Standalone factory flasher: downloads factory image from the
                      vX.Y.Z-factory companion prerelease (bootloader+partitions+app,
                      merged via idf.py merge-bin in CI), flashes at 0x0; optional
                      --config pre-seeds NVS (webterm namespace)
```

## Key APIs

- `POST /api/login` — authenticate, returns session token
- `POST /api/logout` — invalidate session
- `GET /api/token` — re-issue a token for a cookie-authenticated session (used for WebSocket auth)
- `GET /api/config` — current config (baud, WiFi, power, device name, NTP status, `auth_initialized`, ports array)
- `POST /api/config` — update config (baud_rate/port, ap_ssid+ap_pass, sta_ssid+sta_pass, new_password+current_password+username, device_name, ntp_server, timezone, wifi_disconnect, power_on_default). WiFi/AP credentials are read as pairs; a lone half of a pair is ignored
- `POST /api/reset` — trigger SBC reset via GPIO
- `POST /api/power` — toggle SBC power via GPIO (`{"power": bool}` to set explicitly)
- `GET /api/sysinfo` — system info (chip, firmware, heap, uptime)
- `GET /api/wifi/scan` — scan for nearby networks
- `GET /api/ota/check` / `POST /api/ota/github` — check for / install a GitHub release
- `POST /api/ota` — upload firmware binary for OTA update
- `POST /api/tls` — replace TLS cert+key (`{"cert": PEM, "key": PEM}`)
- `POST /api/reboot` — reboot the ESP32
- `GET /ws` — WebSocket for terminal data (requires auth cookie or ?token= query param, optional ?port=N for multi-port)

## Pin Assignments

Pins are target-conditional (`#if CONFIG_IDF_TARGET_ESP32C3` in headers).

| Function | ESP32-C6 | ESP32-C3 | ESP32-S3 (XIAO) | Notes |
|----------|----------|----------|-----------------|-------|
| UART1 TX | GPIO10   | GPIO0    | GPIO1 (D0)      | To SBC RX |
| UART1 RX | GPIO11   | GPIO1    | GPIO2 (D1)      | From SBC TX |
| SBC Reset | GPIO22  | GPIO6    | GPIO4 (D3)      | Active-low, normally high-Z |
| SBC Power | GPIO23  | GPIO7    | GPIO5 (D4)      | Drives relay/MOSFET |
| USB Host | —        | —        | USB-C (GPIO19/20) | CDC-ACM via OTG adapter |

## WiFi Behavior

- Boot with STA config: starts AP+STA, disables AP once STA connects
- STA drops: re-enables AP, reconnect watchdog retries every 30s
- No STA config: AP-only mode
- Device name sets DHCP hostname (sanitized: spaces→hyphens, special chars stripped)
- mDNS advertises `<sanitized-name>.local` with `_http._tcp` service

## Key Constraints

- ESP-IDF must be sourced before build
- cJSON, mdns are external deps (idf_component.yml); USB CDC-ACM + VCP drivers added conditionally for S3
- mbedtls v4.x: use `mbedtls/md.h` not `mbedtls/sha256.h`
- LWIP_MAX_SOCKETS=16, max_open_sockets=4
- httpd stack_size=10240
- Single-threaded httpd: can't serve multiple large files in parallel
- index.html is self-contained: ALL CSS, app JS, FitAddon inlined
- xterm.min.js served pre-gzipped (67KB), chunked 4KB
- xterm.min.js.gz via EMBED_FILES (no null terminator, exact binary length)
- index.html via EMBED_TXTFILES (adds null terminator, use strlen())
- WebSocket auth: token passed as ?token= query param; port selected via ?port=N (default 0)
- Multi-port: serial_port.h abstracts UART/USB; C3/C6 have 1 port, S3 has 2 (UART + USB CDC-ACM)
- USB CDC-ACM: supports standard CDC, CH34x, CP210x, FTDI via VCP drivers; hot-plug with frontend notifications
- Task watchdog: 10s timeout, panic (reboot) on hang
- OTA rollback: firmware marked valid at end of app_main after all subsystems init
- GitHub releases: keep the main release at 3 app bins; factory images go in the vX.Y.Z-factory companion prerelease, which stays out of
  releases/latest. The old reason for the limit — firmware ≤1.4.2 reads releases/latest into an 8KB buffer — no longer holds: that response has
  measured ~8.2KB since v1.5.1 (GitHub adds a per-asset `digest`), so ≤1.4.2 can't use Check-for-update at any asset count. Update those by uploading the .bin, or with tools/factory_flash.py
- NTP: DHCP server discovery with pool.ntp.org fallback, optional manual override
- Timezone: POSIX TZ string stored in NVS, applied via setenv("TZ")/tzset()
- CORS: API responses set Access-Control-Allow-Origin: null and X-Content-Type-Options: nosniff
- Paste throttle: large pastes chunked at 64 bytes / 10ms in frontend
- Setup mode: while `auth_initialized` is false (admin/admin never changed), `/ws`, `/api/token`,
  `/api/reset`, `/api/power`, `/api/ota*`, `/api/tls`, `/api/reboot`, `/api/wifi/scan` return 403 and
  `POST /api/config` accepts only new_password/current_password/username; `GET /api/config` and
  `/api/sysinfo` stay open so the UI can render and show the lock banner
- Password policy: firmware requires 8+ characters (MIN_PASSWORD_LEN) and a 1-32 char username
- Request bodies: read with `read_body()` in web_server.c, never a bare `httpd_req_recv()` —
  it may short-read, and every JSON handler would parse a truncated body as invalid JSON
- WS clients are tracked by socket fd but carry the session token they authenticated with, and
  are re-validated on the push path (throttled to 1/s). Never close() a tracked fd directly:
  esp_http_server has no cross-task teardown, and closing the number races with its reuse
- No `ESP_ERROR_CHECK` on any path reachable from an HTTP handler or the WiFi event loop — it
  aborts and reboots. Boot-time init in `wifi_manager_start()` is the only place it's acceptable
- Login counters are per address (AUTH_TRACKED_HOSTS table, sized against max_open_sockets)

## Conventions

- C source uses ESP-IDF logging (`ESP_LOGI`, `ESP_LOGW`, `ESP_LOGE`)
- Frontend uses vanilla JS (no build step)
- All frontend/cert files embedded in firmware via `EMBED_TXTFILES` / `EMBED_FILES`
- Default credentials: admin/admin; the password change is enforced server-side (see Setup mode)
- Avoid enum names clashing with ESP-IDF (use WIFI_MGR_MODE_AP etc)
