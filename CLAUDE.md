# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

ESP32 Web Terminal — web-based serial terminal for SBCs (Raspberry Pi, etc.) via ESP32 over WiFi.
Supported boards: ESP32-C6-DevKitC-1 (8MB), ESP32-C3 Super Mini (4MB).

## Build

Requires ESP-IDF v5.5+. Before first build, generate TLS certs:

```bash
cd certs && ./generate_cert.sh && cd ..
```

Build and flash (choose target):

```bash
idf.py set-target esp32c6   # or esp32c3
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

If sdkconfig.defaults changes or switching targets, do a full clean rebuild:

```bash
rm sdkconfig && idf.py fullclean && idf.py set-target esp32c6 && idf.py build  # or esp32c3
```

## Architecture

```
main/
  main.c            - Entry point: init all subsystems, NTP setup, OTA rollback verification
  config.c/h        - NVS persistent config (WiFi, baud, auth, device name, NTP, GPIO defaults)
  wifi_manager.c/h  - AP+STA WiFi with auto-fallback, mDNS, DHCP hostname, reconnect watchdog
  auth.c/h          - Session-based auth, salted SHA-256, rate limiting
  uart_bridge.c/h   - UART1 ↔ WebSocket bridge, configurable baud (pins vary by target)
  web_server.c/h    - HTTPS server, REST API, WebSocket, OTA, sysinfo, embedded static files
  gpio_control.c/h  - SBC reset and power control (pins vary by target)
hardware/
  DESIGN.md         - C6 HAT PCB design
  WIRING-C3-MINI.md - C3 Super Mini wiring guide
frontend/
  index.html        - Single-page terminal UI (all CSS, app JS, FitAddon inlined)
  terminal.js       - WebSocket client, login, toolbar controls (reference copy)
  style.css         - Dark theme styling (reference copy)
certs/
  generate_cert.sh  - Generates self-signed ECC P-256 cert for TLS
```

## Key APIs

- `POST /api/login` — authenticate, returns session token
- `POST /api/logout` — invalidate session
- `GET /api/config` — current config (baud, WiFi, power, device name, NTP status)
- `POST /api/config` — update config (baud_rate, sta_ssid/sta_pass, new_password, device_name, ntp_server, timezone, wifi_disconnect, power_on_default)
- `POST /api/reset` — trigger SBC reset via GPIO
- `POST /api/power` — toggle SBC power via GPIO
- `POST /api/ota` — upload firmware binary for OTA update
- `GET /api/sysinfo` — system info (chip, firmware, heap, uptime)
- `GET /ws` — WebSocket for terminal data (requires auth cookie or ?token= query param)

## Pin Assignments

Pins are target-conditional (`#if CONFIG_IDF_TARGET_ESP32C3` in headers).

| Function | ESP32-C6 | ESP32-C3 | Notes |
|----------|----------|----------|-------|
| UART1 TX | GPIO10   | GPIO0    | To SBC RX |
| UART1 RX | GPIO11   | GPIO1    | From SBC TX |
| SBC Reset | GPIO22  | GPIO6    | Active-low, normally high-Z |
| SBC Power | GPIO23  | GPIO7    | Drives relay/MOSFET |

## WiFi Behavior

- Boot with STA config: starts AP+STA, disables AP once STA connects
- STA drops: re-enables AP, reconnect watchdog retries every 30s
- No STA config: AP-only mode
- Device name sets DHCP hostname (sanitized: spaces→hyphens, special chars stripped)
- mDNS advertises `<sanitized-name>.local` with `_http._tcp` service

## Key Constraints

- ESP-IDF must be sourced before build
- cJSON is external dep (idf_component.yml), mdns is external dep
- mbedtls v4.x: use `mbedtls/md.h` not `mbedtls/sha256.h`
- LWIP_MAX_SOCKETS=16, max_open_sockets=4
- httpd stack_size=10240
- Single-threaded httpd: can't serve multiple large files in parallel
- index.html is self-contained: ALL CSS, app JS, FitAddon inlined
- xterm.min.js served pre-gzipped (67KB), chunked 4KB
- xterm.min.js.gz via EMBED_FILES (no null terminator, exact binary length)
- index.html via EMBED_TXTFILES (adds null terminator, use strlen())
- WebSocket auth: token passed as ?token= query param
- Task watchdog: 10s timeout, panic (reboot) on hang
- OTA rollback: firmware marked valid at end of app_main after all subsystems init
- NTP: DHCP server discovery with pool.ntp.org fallback, optional manual override
- Timezone: POSIX TZ string stored in NVS, applied via setenv("TZ")/tzset()
- CORS: API responses set Access-Control-Allow-Origin: null and X-Content-Type-Options: nosniff
- Paste throttle: large pastes chunked at 64 bytes / 10ms in frontend

## Conventions

- C source uses ESP-IDF logging (`ESP_LOGI`, `ESP_LOGW`, `ESP_LOGE`)
- Frontend uses vanilla JS (no build step)
- All frontend/cert files embedded in firmware via `EMBED_TXTFILES` / `EMBED_FILES`
- Default credentials: admin/admin (forced password change on first login)
- Avoid enum names clashing with ESP-IDF (use WIFI_MGR_MODE_AP etc)
