# Changelog

Notable changes per release. This log starts at 1.6.0 — earlier releases are
described by their [GitHub release notes](https://github.com/ausil/esp32-web-terminal/releases)
and git history.

## 1.6.2

Fixes a 1.6.1 regression that could turn an OTA update into a rollback on
some ESP32-S3 boards. If you updated to 1.6.1 successfully, this release
changes nothing else for you.

### Fixed

- **An ESP32-S3 whose USB host stack fails to install now boots with the
  UART bridge instead of aborting.** 1.6.1 made the USB CDC bridge refuse
  to start when the host library never signalled ready — correct behaviour
  for a half-initialised USB host, wrong behaviour for a port that is
  optional by design: the failure propagated to an `ESP_ERROR_CHECK` in
  `app_main`, which panicked before the new image could mark itself valid,
  so OTA'd boards rebooted, panicked, and rolled back (observed on one S3
  rev 2; `usb_host_install` hangs on that board). The device now logs the
  failure, the USB port reports absent, and the terminal, GPIO and web UI
  work. `ESP_ERROR_CHECK` was also removed from the two install calls
  inside the USB library task, which could panic-reboot the same way.

## 1.6.1

A hardening and reliability release on top of 1.6.0: the fixes below close
several ways a device could misbehave or drop its terminal under load, and
the firmware now ships with a host test suite that CI requires to pass
before any release.

### Fixed

- **WebSocket frames are now sent only from the httpd task.** The serial
  bridge pushed frames straight onto sockets from the UART/USB tasks, which
  `esp_http_server` does not support: it has no per-socket send lock, so a
  push racing session teardown could write onto a recycled socket descriptor
  — corrupting another client's stream or worse. Serial data now flows
  through a bounded ring that the httpd task drains; when the task is busy
  (an OTA upload, for example) frames are dropped with a counter instead of
  blocking the bridge. Sessions that die while a terminal is open are also
  cut off reliably now, since `close()` on a tracked descriptor no longer
  races the server's own teardown.
- **Session cookies are matched by exact cookie name.** A cookie named
  `xsession` (or any name ending in `session`) could satisfy the `session`
  lookup, handing its holder the authenticated cookie's session.
- **USB CDC hot-unplug no longer crashes the bridge.** The CDC device handle
  is now guarded across check-use pairs, so yanking a device mid-`send()`
  or mid-baud-change cannot free a handle another path is using. The
  bridge task also refuses to start if its library task never signals
  ready, instead of running against half-initialised USB host state.
- **Re-saving STA credentials from AP+STA mode no longer fails.** With STA
  configured but disconnected, applying the same credentials called
  `esp_wifi_start()` on an already-running stack and reported invalid
  credentials; it now calls `esp_wifi_connect()` and accepts the
  already-connecting error.
- **A present-but-invalid body on `POST /api/power` returns `400`.** It
  used to fall through to toggling the relay, so a malformed request could
  flip the SBC's power; an explicitly absent body still means toggle.
- **The captive-portal 404 redirects to the host the client actually
  reached**, instead of a fixed address, so phones probing `captive.apple.com`
  land on the device's page however they found it.
- **WiFi scans no longer leak driver memory** (the AP list is cleared after
  every scan), and the scan-suppression and NTP-sync flags can no longer be
  cached in registers across tasks.

### Testing

- New host unit-test harness under `tests/host/`: the config crypto is
  checked against known PBKDF2 vectors and an independent SHA-256 oracle,
  auth covers sessions, per-IP lockout and the cookie parser, OTA covers
  semver and asset selection, and the WS TX ring is compiled from
  `web_server.c` itself so the tests cannot drift from production. A pytest
  suite covers `tools/factory_flash.py`'s NVS pre-seeding, including a hash
  vector shared with the C suite.
- CI runs both suites on every push and pull request, and the release job
  waits for them in addition to the three firmware targets.

## 1.6.0

A security release. It closes a credential-free path to the serial console and
changes behaviour for devices that are still on the factory password.
**Update recommendation: install this release.** See [SECURITY.md](SECURITY.md)
for the advisory.

### Security

- **Fixed unauthenticated WebSocket access to the serial console.** The `/ws`
  handshake's rejection path wrote a `401` body onto an already-upgraded
  socket, so it never actually rejected anything, and data frames from an
  unregistered socket descriptor were adopted as a client. Anyone able to reach
  the device had a serial console with no credentials. Present since the
  initial commit.
- **The default password change is now enforced server-side.** While
  `admin`/`admin` is still in place the device is reachable but inert: the
  terminal WebSocket, token mint, GPIO controls, OTA, TLS upload, ESP reboot,
  WiFi scan and all settings writes return `403`, and `POST /api/config`
  accepts nothing but the credential change. `GET /api/config` and
  `GET /api/sysinfo` stay open so the UI can render and show a banner
  explaining the lock.
- **Revoking a session now stops a live terminal.** Sockets are re-validated
  against their session on the data path, so a password change, a logout, or
  the one-hour timeout cuts the serial stream immediately instead of leaving an
  open socket reading it indefinitely.
- **An authenticated user can no longer reboot the device** with a malformed
  `POST /api/config`. WiFi paths that ran under `ESP_ERROR_CHECK` inside the
  request handler now log and return, and STA credentials are validated before
  they are written to NVS.
- **Login lockout is per address.** The counter used to be global, so anyone
  could lock every other client out of the device for five minutes without
  credentials.
- **New password must be at least 8 characters**, checked by the firmware
  rather than the page (the page had allowed 4). A username that is empty or
  over 32 characters is rejected instead of silently truncated.

### Breaking changes

- Devices still on the factory `admin`/`admin` password will see `403` from the
  terminal, GPIO controls, OTA and all settings writes until a password is set
  in Settings. This is the point of the change, but it will look like a
  malfunction if you aren't expecting it: the terminal pane stays empty and a
  banner explains why.
- WiFi configuration now sits behind that password change, since it is a
  settings write.

### Fixed

- **Relay debounce bypass.** `POST /api/power` with an explicit
  `{"power":bool}` bypassed the minimum interval between power changes that
  `toggle` enforced. Every power change now goes through one path.
- **Truncated request bodies.** `httpd_req_recv()` may return a short read, and
  every JSON handler parsed whatever arrived on the first call, turning a valid
  login or config change into a spurious `400 Invalid JSON` under load. Bodies
  are read to completion by a shared helper, which also distinguishes an absent
  body from one that failed to arrive — a truncated body used to fall through
  to *toggling the SBC power*.
- **Boot loop from a bad stored SSID.** An over-long SSID in NVS — reachable
  from a hand-edited `tools/factory_flash.py --config` seed, or from credentials
  written by older firmware — aborted at boot on a device that is usually
  mounted and unattended. It now logs and falls back to AP-only.

### Documentation

The README and `CLAUDE.md` were corrected against the code: the password hash
was described as salted SHA-256 where it is PBKDF2-HMAC-SHA256, "all API
endpoints require authentication" was untrue of `/api/login` itself, the
endpoint table listed 9 of 16 routes, `ap_ssid`/`ap_pass` and
`username`/`current_password` were undocumented, and the config table claimed
every field was independently optional when both WiFi pairs are read together.
`frontend/terminal.js` and `frontend/style.css` are now marked as unserved
reference copies that lag `index.html`.
