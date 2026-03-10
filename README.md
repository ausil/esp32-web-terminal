# ESP32 Web Terminal

A web-based serial terminal for single-board computers (Raspberry Pi, BeagleBone, etc.) using an ESP32-C6 as a WiFi-to-UART bridge. Access your SBC's serial console from any browser over your local network — no USB cable or SSH needed.

## Features

- **Browser-based terminal** — full terminal emulator using [xterm.js](https://xtermjs.org/), accessible from any device with a browser
- **WiFi connectivity** — connect via your existing WiFi network (STA mode) or the ESP32's own access point (AP mode)
- **HTTPS + WebSocket** — all traffic encrypted with TLS using a self-signed ECC P-256 certificate
- **Authentication** — session-based login with salted SHA-256 password hashing, rate limiting, and automatic lockout
- **Remote power control** — reset or power-cycle your SBC via GPIO-driven relay/MOSFET
- **Configurable baud rate** — change serial speed on the fly from the toolbar (9600–1,500,000)
- **Persistent configuration** — WiFi credentials, baud rate, and auth settings stored in NVS flash
- **No external dependencies** — vanilla JS frontend embedded in firmware, no build tools or SPIFFS needed

## Hardware

### Requirements

- **ESP32-C6-DevKitC-1** (or any ESP32-C6 board)
- Level shifter if your SBC uses 5V logic (ESP32-C6 GPIOs are 3.3V)
- Relay or MOSFET for power control (optional)

### Wiring

| Function     | ESP32 GPIO | Connect to       |
|-------------|-----------|------------------|
| UART TX      | GPIO10    | SBC RX           |
| UART RX      | GPIO11    | SBC TX           |
| SBC Reset    | GPIO22    | SBC reset pin (active-low, normally high-Z) |
| SBC Power    | GPIO23    | Relay/MOSFET gate |
| GND          | GND       | SBC GND          |

## Getting Started

### Prerequisites

- [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c6/get-started/) v5.5 or later
- OpenSSL (for certificate generation)

### Build and Flash

1. **Generate TLS certificates** (first time only):

   ```bash
   cd certs && ./generate_cert.sh && cd ..
   ```

2. **Set target and build**:

   ```bash
   idf.py set-target esp32c6
   idf.py build
   ```

3. **Flash and monitor**:

   ```bash
   idf.py -p /dev/ttyUSB0 flash monitor
   ```

### First-Time Setup

1. Connect to the ESP32's WiFi access point:
   - **SSID:** `ESP-Terminal`
   - **Password:** `esp32term`

2. Open `https://192.168.4.1` in your browser (accept the self-signed certificate warning).

3. Log in with the default credentials:
   - **Username:** `admin`
   - **Password:** `admin`

4. You'll be prompted to change the default password on first login.

5. Optionally, configure your home WiFi credentials via the config API so the ESP32 joins your network (STA mode). The AP remains available as a fallback.

## Usage

### Terminal

Once logged in, you'll see a full terminal connected to your SBC's serial port. Type directly to send input; output appears in real time via WebSocket.

### Toolbar

- **Baud rate** — select from the dropdown to change serial speed (takes effect immediately)
- **Reset SBC** — sends a reset pulse via GPIO22
- **Power** — toggles SBC power via GPIO23
- **Logout** — ends your session

### WiFi Modes

| Mode   | When                                       | Access URL              |
|--------|--------------------------------------------|------------------------|
| AP     | No STA credentials configured (default)    | `https://192.168.4.1`  |
| AP+STA | STA credentials configured, connected      | `https://<STA IP>` or `https://192.168.4.1` |
| AP     | STA credentials configured, connection failed | `https://192.168.4.1` (auto-fallback) |

## REST API

All API endpoints require authentication via session cookie (obtained from `/api/login`).

| Method | Endpoint       | Description                          | Body (JSON)                                      |
|--------|---------------|--------------------------------------|--------------------------------------------------|
| POST   | `/api/login`   | Authenticate, returns session token  | `{"username": "...", "password": "..."}`         |
| POST   | `/api/logout`  | Invalidate session                   | —                                                |
| GET    | `/api/config`  | Get current configuration            | —                                                |
| POST   | `/api/config`  | Update configuration                 | `{"baud_rate": 115200}`, `{"sta_ssid": "...", "sta_pass": "..."}`, or `{"new_password": "..."}` |
| POST   | `/api/reset`   | Trigger SBC reset via GPIO           | —                                                |
| POST   | `/api/power`   | Toggle SBC power via GPIO            | —                                                |
| GET    | `/ws`          | WebSocket for terminal data          | Binary frames (serial data)                      |

## Security

- **TLS** — all HTTP and WebSocket traffic is encrypted. The self-signed ECC P-256 certificate is embedded in firmware.
- **Authentication** — passwords are stored as salted SHA-256 hashes in NVS. Plain-text passwords are never stored.
- **Session management** — up to 4 concurrent sessions, each with a 1-hour timeout. Sessions use 32-byte random tokens.
- **Rate limiting** — after 5 failed login attempts, login is locked out for 5 minutes.
- **Default credentials** — `admin`/`admin` with forced password change on first login.

## Project Structure

```
main/
  main.c            Entry point, initializes all subsystems
  config.c/h        NVS persistent configuration
  wifi_manager.c/h  AP + STA WiFi with auto-fallback
  auth.c/h          Session auth, salted SHA-256, rate limiting
  uart_bridge.c/h   UART ↔ WebSocket bridge
  web_server.c/h    HTTPS server, REST API, WebSocket
  gpio_control.c/h  SBC reset and power control
frontend/
  index.html        Single-page terminal UI
  terminal.js       WebSocket client, login flow, toolbar
  style.css         Dark theme styling
certs/
  generate_cert.sh  Generates self-signed ECC P-256 cert
```

## Partition Table

| Partition | Type | Size    |
|-----------|------|---------|
| nvs       | data | 24 KB   |
| phy_init  | data | 4 KB    |
| factory   | app  | 1920 KB |
| storage   | data | 64 KB   |

## License

MIT
