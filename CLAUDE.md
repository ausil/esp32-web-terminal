# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

ESP32 Web Terminal — web-based serial terminal for SBCs (Raspberry Pi, etc.) via ESP32-C6-DevKitC-1 over WiFi.

## Build

Requires ESP-IDF v5.5.x. Before first build, generate TLS certs:

```bash
cd certs && ./generate_cert.sh && cd ..
```

Build and flash:

```bash
idf.py set-target esp32c6
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

## Architecture

```
main/
  main.c          - Entry point: init all subsystems
  config.c/h      - NVS persistent config (WiFi, baud, auth, GPIO defaults)
  wifi_manager.c/h - AP+STA WiFi with auto-fallback to AP
  auth.c/h        - Session-based auth, salted SHA-256, rate limiting
  uart_bridge.c/h - UART1 (GPIO10/11) ↔ WebSocket bridge, configurable baud
  web_server.c/h  - HTTPS server, REST API, WebSocket, embedded static files
  gpio_control.c/h - SBC reset (GPIO22) and power control (GPIO23)
frontend/
  index.html      - Single-page terminal UI (xterm.js from CDN)
  terminal.js     - WebSocket client, login, toolbar controls
  style.css       - Dark theme styling
certs/
  generate_cert.sh - Generates self-signed ECC P-256 cert for TLS
```

## Key APIs

- `POST /api/login` — authenticate, returns session token
- `POST /api/logout` — invalidate session
- `GET /api/config` — current config (baud, WiFi status, power state)
- `POST /api/config` — update config (baud_rate, sta_ssid/sta_pass, new_password)
- `POST /api/reset` — trigger SBC reset via GPIO
- `POST /api/power` — toggle SBC power via GPIO
- `GET /ws` — WebSocket for terminal data (requires auth cookie)

## Pin Assignments

| Function | GPIO | Notes |
|----------|------|-------|
| UART1 TX | GPIO10 | To SBC RX |
| UART1 RX | GPIO11 | From SBC TX |
| SBC Reset | GPIO22 | Active-low, normally high-Z |
| SBC Power | GPIO23 | Drives relay/MOSFET |

## Conventions

- C source uses ESP-IDF logging (`ESP_LOGI`, `ESP_LOGW`, `ESP_LOGE`)
- Frontend uses vanilla JS (no build step), xterm.js loaded from CDN
- All frontend/cert files embedded in firmware via `EMBED_TXTFILES`
- Default credentials: admin/admin (forced password change on first login)
