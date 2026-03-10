# ESP32 Web Terminal

A web-based serial terminal for single-board computers (Raspberry Pi, BeagleBone, etc.) using an ESP32-C6 as a WiFi-to-UART bridge. Access your SBC's serial console from any browser over your local network — no USB cable or SSH needed.

## Features

- **Browser-based terminal** — full terminal emulator using [xterm.js](https://xtermjs.org/), accessible from any device with a browser
- **WiFi connectivity** — connect via your existing WiFi network (STA mode) or the ESP32's own access point (AP mode)
- **Smart WiFi management** — AP disabled when STA is connected; auto-fallback to AP if STA drops; reconnect watchdog retries every 30s
- **HTTPS + WebSocket** — all traffic encrypted with TLS using a self-signed ECC P-256 certificate
- **Authentication** — session-based login with salted SHA-256 password hashing, rate limiting, and automatic lockout
- **Remote power control** — reset or power-cycle your SBC via GPIO-driven relay/MOSFET
- **Configurable baud rate** — change serial speed on the fly from the toolbar (9600–1,500,000)
- **OTA firmware update** — upload new firmware from the browser with automatic rollback if the new firmware fails to boot
- **Device identification** — configurable device name shown in UI, browser tab, DHCP hostname, and mDNS
- **mDNS** — access your device at `<device-name>.local` (e.g., `Pi-Rack-1.local`)
- **NTP time sync** — automatic time from DHCP-provided NTP server, manual server override available
- **Timezone support** — configurable timezone with preset selections or custom POSIX TZ string
- **Persistent configuration** — all settings stored in NVS flash
- **Session log download** — save terminal scrollback to a timestamped `.log` file
- **System info** — firmware version, chip info, heap usage, uptime visible in Settings
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
   - **SSID:** `ESP-Terminal-XXXX` (last 4 hex digits of MAC)
   - **Password:** `esp32term`

2. Open `https://192.168.4.1` in your browser (accept the self-signed certificate warning).

3. Log in with the default credentials:
   - **Username:** `admin`
   - **Password:** `admin`

4. You'll be prompted to change the default password on first login.

5. Open **Settings** to configure:
   - **Device name** — identifies this unit (shown in UI, DHCP, mDNS)
   - **WiFi** — enter your network SSID/password to join your LAN
   - **NTP server** — leave empty for DHCP auto-discovery, or set a specific server

Once WiFi is configured, the AP is disabled and the device is accessible at `https://<device-name>.local` or its DHCP-assigned IP.

## Usage

### Terminal

Once logged in, you'll see a full terminal connected to your SBC's serial port. Type directly to send input; output appears in real time via WebSocket.

### Toolbar

- **Device name** — shown at the left of the toolbar
- **Baud rate** — select from the dropdown to change serial speed (takes effect immediately)
- **Reset SBC** — sends a reset pulse via GPIO22
- **Power** — toggles SBC power via GPIO23
- **Firmware version** — shown in the toolbar
- **Save Log** — download terminal scrollback as a `.log` file
- **Settings** — device name, WiFi (connect/disconnect), NTP, timezone, password, power-on default, system info, OTA firmware update
- **Logout** — ends your session

### WiFi Modes

| Mode   | When                                  | Access URL                |
|--------|---------------------------------------|--------------------------|
| AP     | No STA credentials configured         | `https://192.168.4.1`    |
| AP+STA | Connecting to STA or STA failed       | Both AP and STA IPs      |
| STA    | Successfully connected to WiFi        | `https://<IP>` or `https://<name>.local` |

When STA is connected, the AP is disabled to avoid broadcasting. If STA drops, the AP re-enables automatically and a watchdog retries STA every 30 seconds. You can also manually disconnect WiFi from Settings to return to AP mode.

### Multiple Devices

Each ESP32 Web Terminal can be given a unique device name in Settings. The name is used for:
- Browser tab title and toolbar header
- DHCP client hostname (visible on your router)
- mDNS hostname (`<name>.local`)

### OTA Updates

Upload new firmware via **Settings > Firmware Update**. The device reboots after upload. If the new firmware fails to start (crash, hang), the bootloader automatically rolls back to the previous working version.

## REST API

All API endpoints require authentication via session cookie (obtained from `/api/login`).

| Method | Endpoint       | Description                          | Body (JSON)                                      |
|--------|---------------|--------------------------------------|--------------------------------------------------|
| POST   | `/api/login`   | Authenticate, returns session token  | `{"username": "...", "password": "..."}`         |
| POST   | `/api/logout`  | Invalidate session                   | —                                                |
| GET    | `/api/config`  | Get current configuration            | —                                                |
| POST   | `/api/config`  | Update configuration                 | See below                                        |
| POST   | `/api/reset`   | Trigger SBC reset via GPIO           | —                                                |
| POST   | `/api/power`   | Toggle or set SBC power              | `{"power": true}` or empty for toggle            |
| POST   | `/api/ota`     | Upload firmware binary               | Raw binary body                                  |
| GET    | `/api/sysinfo` | System info (chip, heap, uptime)     | —                                                |
| GET    | `/ws`          | WebSocket for terminal data          | Binary frames (serial data)                      |

### Config POST fields

All fields are optional; include only what you want to change:

| Field             | Type    | Description                    |
|-------------------|---------|--------------------------------|
| `baud_rate`       | number  | Serial baud rate               |
| `sta_ssid`        | string  | WiFi network SSID              |
| `sta_pass`        | string  | WiFi network password          |
| `new_password`    | string  | New login password             |
| `device_name`     | string  | Device identifier              |
| `ntp_server`      | string  | NTP server (empty = use DHCP)  |
| `timezone`        | string  | POSIX TZ string (e.g., `EST5EDT,M3.2.0,M11.1.0`) |
| `wifi_disconnect` | boolean | Set true to disconnect STA and switch to AP |
| `power_on_default`| boolean | Power on SBC at boot           |

### Config GET response

Returns current state including: `baud_rate`, `power_on`, `power_on_default`, `sta_ssid`, `sta_connected`, `sta_ip`, `ap_ip`, `wifi_mode`, `auth_initialized`, `device_name`, `ntp_server`, `ntp_active_server`, `ntp_synced`, `timezone`, `firmware_version`.

### Sysinfo GET response

Returns: `chip`, `cores`, `revision`, `firmware_version`, `idf_version`, `build_date`, `build_time`, `ota_partition`, `free_heap`, `min_free_heap`, `uptime`, `uptime_seconds`.

## Security

- **TLS** — all HTTP and WebSocket traffic is encrypted. The self-signed ECC P-256 certificate is embedded in firmware.
- **Authentication** — passwords are stored as salted SHA-256 hashes in NVS. Plain-text passwords are never stored.
- **Session management** — up to 4 concurrent sessions, each with a 1-hour timeout. Sessions use 32-byte random tokens.
- **Rate limiting** — after 5 failed login attempts, login is locked out for 5 minutes.
- **Default credentials** — `admin`/`admin` with forced password change on first login.
- **CORS** — API responses restrict cross-origin access.
- **Paste throttling** — large pastes are chunked (64 bytes at 10ms intervals) to avoid UART buffer overflow.
- **Navigation guard** — browser warns before closing an active terminal session.
- **Watchdog** — task watchdog (10s timeout) reboots the device if the system hangs.
- **OTA rollback** — bad firmware is automatically reverted by the bootloader.

## Project Structure

```
main/
  main.c            Entry point, NTP init, OTA verification
  config.c/h        NVS persistent configuration
  wifi_manager.c/h  AP + STA WiFi, mDNS, reconnect watchdog
  auth.c/h          Session auth, salted SHA-256, rate limiting
  uart_bridge.c/h   UART ↔ WebSocket bridge
  web_server.c/h    HTTPS server, REST API, WebSocket, OTA
  gpio_control.c/h  SBC reset and power control
frontend/
  index.html        Single-page terminal UI (all CSS/JS inlined)
  terminal.js       WebSocket client, login flow, toolbar (reference)
  style.css         Dark theme styling (reference)
certs/
  generate_cert.sh  Generates self-signed ECC P-256 cert
```

## Partition Table

| Partition | Type | Size    | Purpose          |
|-----------|------|---------|------------------|
| nvs       | data | 24 KB   | Configuration    |
| otadata   | data | 8 KB    | OTA boot state   |
| ota_0     | app  | 1920 KB | Firmware slot A  |
| ota_1     | app  | 1920 KB | Firmware slot B  |
| storage   | data | 128 KB  | Reserved         |

## License

MIT
