# Contributing to ESP32 Web Terminal

Thanks for your interest in contributing! This guide covers how to set up the project, what to keep in mind, and how to submit changes.

## Getting Started

### Prerequisites

- [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/) v5.5 or later
- One of the supported boards:
  - ESP32-C6-DevKitC-1 (8MB)
  - ESP32-C3 Super Mini (4MB)
- A Linux or macOS development environment

### Building

Source ESP-IDF, then generate TLS certificates (first time only):

```bash
source /path/to/esp-idf/export.sh
cd certs && ./generate_cert.sh && cd ..
```

Build and flash:

```bash
idf.py set-target esp32c6   # or esp32c3
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

If you change `sdkconfig.defaults` or switch targets, do a full clean rebuild:

```bash
rm sdkconfig && idf.py fullclean && idf.py set-target esp32c6 && idf.py build
```

## Project Structure

```
main/           C firmware source (ESP-IDF components)
frontend/       Single-page web UI (vanilla JS, no build step)
hardware/       PCB design docs and wiring guides
certs/          TLS certificate generation
```

See [CLAUDE.md](CLAUDE.md) for detailed architecture documentation.

## Guidelines

### General

- Keep changes focused. One feature or fix per pull request.
- Test on real hardware when possible. The ESP32's constrained environment (RAM, sockets, single-threaded httpd) means things that work in theory may not work in practice.
- Add SPDX license headers to new source files:
  ```c
  // SPDX-License-Identifier: MIT
  // Copyright (c) 2026 Dennis Gilmore
  ```

### C Firmware (`main/`)

- Use ESP-IDF logging macros (`ESP_LOGI`, `ESP_LOGW`, `ESP_LOGE`) instead of `printf`.
- Avoid enum names that clash with ESP-IDF (prefix with `WIFI_MGR_`, etc.).
- Pin assignments are target-conditional via `#if CONFIG_IDF_TARGET_ESP32C3` in headers. Support both C6 and C3 when adding new GPIO usage.
- Be mindful of resource limits:
  - `LWIP_MAX_SOCKETS=16`, `max_open_sockets=4`
  - `httpd stack_size=10240`
  - Task watchdog timeout is 10 seconds
- Use `mbedtls/md.h` for hashing (not `mbedtls/sha256.h` -- mbedtls v4.x).

### Frontend (`frontend/`)

- `index.html` is self-contained: all CSS, application JS, and the FitAddon are inlined. Do not add external resource dependencies.
- `terminal.js` and `style.css` are reference copies. The actual served content is what's inlined in `index.html`.
- The only external resource is `xterm.min.js`, served pre-gzipped and chunked at 4KB.
- Use vanilla JS only -- no frameworks, no build tools, no npm.

### Embedded Files

- Files embedded via `EMBED_TXTFILES` get a null terminator (use `strlen()` for length).
- Files embedded via `EMBED_FILES` have no null terminator (use the exact binary length).

## Submitting Changes

1. Fork the repository and create a branch from `main`.
2. Make your changes, following the guidelines above.
3. Verify the firmware builds cleanly for both targets:
   ```bash
   idf.py set-target esp32c6 && idf.py build
   rm sdkconfig && idf.py fullclean
   idf.py set-target esp32c3 && idf.py build
   ```
4. Open a pull request against `main` with a clear description of what changed and why.

## Reporting Issues

Open an issue on GitHub. Include:

- Which board you're using (C6 or C3)
- ESP-IDF version
- Steps to reproduce
- Serial monitor output if relevant

## License

By contributing, you agree that your contributions will be licensed under the [MIT License](LICENSE).
