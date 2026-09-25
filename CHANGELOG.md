# Changelog

Notable changes per release. This log starts at 1.6.0 — earlier releases are
described by their [GitHub release notes](https://github.com/ausil/esp32-web-terminal/releases)
and git history.

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
