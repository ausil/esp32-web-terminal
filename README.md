# ESP32 Web Terminal

A web-based serial terminal for single-board computers (Raspberry Pi, BeagleBone, etc.) using an ESP32 as a WiFi-to-UART bridge. Access your SBC's serial console from any browser over your local network — no USB cable or SSH needed.

Supported boards: **ESP32-C6-DevKitC-1** (8MB), **ESP32-C3 Super Mini** (4MB), and **XIAO ESP32S3** (8MB).

## Features

- **Browser-based terminal** — full terminal emulator using [xterm.js](https://xtermjs.org/), accessible from any device with a browser
- **WiFi connectivity** — connect via your existing WiFi network (STA mode) or the ESP32's own access point (AP mode)
- **Smart WiFi management** — AP disabled when STA is connected; auto-fallback to AP if STA drops; reconnect watchdog retries every 30s
- **HTTPS + WebSocket** — TLS encrypted using a self-signed ECC P-256 certificate
- **Authentication** — session-based login with salted SHA-256 password hashing, rate limiting, and automatic lockout
- **Remote power control** — reset or power-cycle your SBC via GPIO-driven relay/MOSFET
- **Multi-port serial** — UART and USB CDC-ACM host support (ESP32-S3); switch between ports from the toolbar
- **USB-to-serial support** — connect to SBCs that expose their console via USB (EspressoBin, etc.) using standard CDC-ACM, CH34x, CP210x, or FTDI adapters (ESP32-S3)
- **Configurable baud rate** — change serial speed on the fly from the toolbar (9600 to 1,500,000)
- **OTA firmware update** — upload new firmware from the browser with automatic rollback if the new firmware fails to boot
- **Device identification** — configurable device name shown in UI, browser tab, DHCP hostname, and mDNS
- **mDNS** — access your device at `<device-name>.local` (e.g., `pi-rack-1.local`)
- **NTP time sync** — automatic time from DHCP-provided NTP server, manual server override available
- **Timezone support** — configurable timezone with preset selections or custom POSIX TZ string
- **Persistent configuration** — all settings stored in NVS flash, survive reboots and OTA updates
- **Session log download** — save terminal scrollback to a timestamped `.log` file
- **System info** — firmware version, chip info, heap usage, uptime visible in Settings
- **Self-contained firmware** — vanilla JS frontend embedded in the binary, no SPIFFS or external hosting needed

## Hardware

### Supported Boards

| Board | Flash | Notes |
|-------|-------|-------|
| ESP32-C6-DevKitC-1 | 8MB | Primary target, WiFi 6 |
| ESP32-C3 Super Mini | 4MB | Compact, low-cost option |
| XIAO ESP32S3 | 8MB | USB Host support for USB-to-serial devices |

### Pin Assignments

Pins are configured per target at compile time.

| Function | ESP32-C6 | ESP32-C3 | ESP32-S3 (XIAO) | Connect to |
|----------|----------|----------|-----------------|------------|
| UART TX  | GPIO10   | GPIO0    | GPIO1 (D0)      | SBC RX     |
| UART RX  | GPIO11   | GPIO1    | GPIO2 (D1)      | SBC TX     |
| SBC Reset | GPIO22  | GPIO6    | GPIO4 (D3)      | Reset pin (active-low, normally high-Z) |
| SBC Power | GPIO23  | GPIO7    | GPIO5 (D4)      | Relay/MOSFET gate |
| USB Host | —        | —        | USB-C            | USB-to-serial device (via OTG adapter) |
| GND      | GND      | GND      | GND              | SBC GND    |

> **Note:** ESP32 GPIOs are 3.3V. Use a level shifter if your SBC has 5V logic levels. Power control requires an external relay or MOSFET.

#### USB Host on ESP32-S3

The XIAO ESP32S3's USB-C port doubles as a USB Host when used with an OTG adapter. Connect a USB-to-serial adapter (CH340, CP2102, FTDI, or standard CDC-ACM device) to access SBCs that expose their console via USB. The firmware auto-detects the device and supports hot-plug.

Since the USB-C port is shared between programming and USB Host mode, use UART0 (D6/D7) with an external USB-to-serial adapter for initial flashing, or flash via USB-C first and use OTA updates afterward.

## Getting Started

### Prerequisites

- [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/) v5.5 or later
- OpenSSL (for TLS certificate generation)

### Build and Flash

1. **Generate TLS certificates** (first time only):

   ```bash
   cd certs && ./generate_cert.sh && cd ..
   ```

2. **Set target and build**:

   ```bash
   idf.py set-target esp32c6   # or esp32c3 or esp32s3
   idf.py build
   ```

3. **Flash and monitor**:

   ```bash
   idf.py -p /dev/ttyUSB0 flash monitor
   ```

> **Switching targets** or changing `sdkconfig.defaults` requires a full clean rebuild:
> ```bash
> rm sdkconfig && idf.py fullclean && idf.py set-target esp32c6 && idf.py build
> ```

### First-Time Setup

1. Connect to the ESP32's WiFi access point:
   - **SSID:** `ESP-Terminal-XXXX` (last 4 hex digits of MAC)
   - **Password:** `esp32term`

2. Open `https://192.168.4.1` in your browser and accept the self-signed certificate warning.

3. Log in with the default credentials:
   - **Username:** `admin`
   - **Password:** `admin`

4. You'll be prompted to change the default password on first login.

5. Open **Settings** to configure your device name, WiFi network, and timezone.

Once WiFi is configured, the AP is disabled and the device is accessible at `https://<device-name>.local` or its DHCP-assigned IP.

## Usage

### Terminal

Once logged in, you'll see a full terminal connected to your SBC's serial port. Type directly to send input; output appears in real time via WebSocket.

### Toolbar

- **Device name** — shown at the left
- **Port** — dropdown to switch between serial ports (shown only on multi-port devices like ESP32-S3)
- **Baud rate** — dropdown to change serial speed (takes effect immediately, per-port)
- **Reset SBC** — sends a reset pulse via GPIO
- **Power** — toggles SBC power via GPIO
- **Firmware version** — shown in the toolbar
- **Save Log** — download terminal scrollback as a `.log` file
- **Settings** — device name, WiFi, NTP, timezone, password, power-on default, system info, OTA update
- **Logout** — ends your session

### WiFi Modes

| Mode   | When                                  | Access URL                |
|--------|---------------------------------------|--------------------------|
| AP     | No STA credentials configured         | `https://192.168.4.1`    |
| AP+STA | Connecting to STA or STA failed       | Both AP and STA IPs      |
| STA    | Successfully connected to WiFi        | `https://<IP>` or `https://<name>.local` |

When STA is connected, the AP is disabled to avoid broadcasting an open network. If STA drops, the AP re-enables automatically and a watchdog retries STA every 30 seconds. You can also manually disconnect WiFi from Settings to return to AP mode.

### Multiple Devices

Each ESP32 Web Terminal can be given a unique device name in Settings. The name is used for:
- Browser tab title and toolbar header
- DHCP client hostname (visible on your router)
- mDNS hostname (`<name>.local`)

### OTA Updates

Upload new firmware via **Settings > Firmware Update**. The device reboots after upload. If the new firmware fails to start (crash, hang), the bootloader automatically rolls back to the previous working version.

## REST API

All API endpoints require authentication via session cookie (obtained from `/api/login`).

| Method | Endpoint       | Description                          | Body                                             |
|--------|---------------|--------------------------------------|--------------------------------------------------|
| POST   | `/api/login`   | Authenticate, returns session token  | `{"username": "...", "password": "..."}`         |
| POST   | `/api/logout`  | Invalidate session                   | —                                                |
| GET    | `/api/config`  | Get current configuration            | —                                                |
| POST   | `/api/config`  | Update configuration                 | JSON with fields below                           |
| POST   | `/api/reset`   | Trigger SBC reset via GPIO           | —                                                |
| POST   | `/api/power`   | Toggle or set SBC power              | `{"power": true}` or empty for toggle            |
| POST   | `/api/ota`     | Upload firmware binary               | Raw binary body                                  |
| GET    | `/api/sysinfo` | System info (chip, heap, uptime)     | —                                                |
| GET    | `/ws`          | WebSocket for terminal data          | Binary frames; `?port=N` to select serial port   |

### Config Fields

All fields are optional in POST requests; include only what you want to change:

| Field             | Type    | Description                    |
|-------------------|---------|--------------------------------|
| `baud_rate`       | number  | Serial baud rate               |
| `port`            | number  | Target port index (default 0)  |
| `sta_ssid`        | string  | WiFi network SSID              |
| `sta_pass`        | string  | WiFi network password          |
| `new_password`    | string  | New login password             |
| `device_name`     | string  | Device identifier              |
| `ntp_server`      | string  | NTP server (empty = use DHCP)  |
| `timezone`        | string  | POSIX TZ string (e.g., `EST5EDT,M3.2.0,M11.1.0`) |
| `wifi_disconnect` | boolean | Disconnect STA, switch to AP   |
| `power_on_default`| boolean | Power on SBC at boot           |

## Security

- **TLS** — all HTTP and WebSocket traffic encrypted with a self-signed ECC P-256 certificate embedded in firmware
- **Authentication** — salted SHA-256 password hashing stored in NVS; plain-text passwords are never stored
- **Session management** — up to 4 concurrent sessions with 1-hour timeout, using 32-byte random tokens
- **Rate limiting** — 5 failed login attempts triggers a 5-minute lockout
- **Default credentials** — `admin`/`admin` with forced password change on first login
- **CORS** — API responses restrict cross-origin access
- **Paste throttling** — large pastes chunked (64 bytes / 10ms) to prevent UART buffer overflow
- **Navigation guard** — browser warns before closing an active terminal session
- **Watchdog** — task watchdog (10s timeout) reboots the device on hang
- **OTA rollback** — bootloader automatically reverts bad firmware

## Project Structure

```
main/
  main.c            Entry point, NTP init, OTA verification
  config.c/h        NVS persistent configuration
  wifi_manager.c/h  AP + STA WiFi, mDNS, reconnect watchdog
  auth.c/h          Session auth, salted SHA-256, rate limiting
  serial_port.c/h   Port abstraction (UART + USB CDC-ACM)
  uart_bridge.c/h   UART backend for serial port layer
  usb_cdc_bridge.c/h USB Host CDC-ACM backend (ESP32-S3 only)
  web_server.c/h    HTTPS server, REST API, WebSocket, OTA
  gpio_control.c/h  SBC reset and power control
frontend/
  index.html        Single-page terminal UI (all CSS/JS inlined)
  terminal.js       WebSocket client, login flow, toolbar (reference)
  style.css         Dark theme styling (reference)
certs/
  generate_cert.sh  Generates self-signed ECC P-256 cert
```

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for build instructions, coding guidelines, and how to submit changes.

## License

[MIT](LICENSE)
