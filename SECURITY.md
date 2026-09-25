# Security Policy

## Supported versions

| Version | Supported |
|---------|-----------|
| 1.6.0   | Yes       |
| ≤ 1.5.2 | **No** — see the advisory below |

Because this device gives serial-console access to another computer, fixes are
only meaningful once installed. 1.5.2 and earlier contain a
credential-free path to that console and should be treated as untrusted
hosts on your network until updated.

## Reporting a vulnerability

Open a private vulnerability report through
[the repository's Security tab](../../security/advisories/new), or contact the
maintainer directly. Please don't open a public issue for anything exploitable.

There is no bug bounty. Reports are acknowledged by hand; expect a reply in a
few days, not hours.

## Advisory: unauthenticated WebSocket access to the serial console

**Affected:** every release from the initial commit through 1.5.2
**Fixed in:** 1.6.0
**Severity:** critical — remote, no credentials, root-equivalent access to the
attached SBC

### Summary

The `/ws` WebSocket handler could be reached without logging in. Anyone able to
open a TCP connection to the device — any host on the LAN, or any host that
could reach it over the device's own access point — could read the serial
console and send keystrokes to it, with no username, no password, and no
session. For the common deployment, where that console is a root shell on a
Raspberry Pi, this is full control of that machine.

### Detail

`esp_http_server` completes the WebSocket upgrade and sends the `101 Switching
Protocols` response *before* it calls the URI handler. The handler's
authentication-failure path answered with a `401 Unauthorized` body, which
after an upgrade is not a response — it is stray bytes on a live socket, and
`httpd_resp_send()` reported success doing it. So the rejection never
rejected: the connection stayed open.

The handler then keyed clients on socket descriptor rather than on session. A
data frame from a descriptor that had not been registered at an authenticated
handshake fell into a "late-add" path that adopted it as a client on port 0.
The result was a working serial console for a client that had never
authenticated.

Two further conditions made this worse than a single missing check:

- The default credentials are `admin`/`admin`, published in the README, and the
  access-point passphrase is fixed. Nothing on the server enforced changing
  them, so a large fraction of deployed devices were permanently in a
  default-credentials state — and even that state used to be enough for the
  terminal.
- Nothing revoked an open terminal socket when a session was invalidated. A
  password change killed the session but left any already-upgraded socket
  streaming serial data indefinitely.

### Fix

1.6.0 does three things:

- An unauthenticated `/ws` handshake is now refused by returning `ESP_FAIL`,
  which is what actually makes `httpd` tear the session down. Data frames are
  routed only from a descriptor that an authenticated handshake registered; an
  unknown descriptor is closed rather than adopted.
- While the factory password is still in place the device is reachable but
  inert: the terminal WebSocket, token mint, GPIO controls, OTA, TLS upload,
  ESP reboot, WiFi scan and all settings writes return `403`. `GET /api/config`
  and `GET /api/sysinfo` stay open so the UI can render and explain the lock,
  and `POST /api/config` accepts nothing but the credential change.
- Live sockets are checked against their session on the data path, so a
  password change, a logout, or the one-hour timeout stops a terminal's data
  immediately instead of whenever TCP happens to notice.

### What this means if you cannot update immediately

The device listens on TCP 443. Blocking LAN clients from reaching it, or
keeping the device on an isolated VLAN, removes the reachable-attacker
requirement. Changing the admin password helps against the *other* default
credential problems but **does not** close the WebSocket hole — in 1.5.2 and
earlier that path needed no credentials at all. Treat a password change on an
unpatched device as hardening, not as a fix.

## Fixed in 1.6.0, lower severity

- **Default credentials not enforced server-side.** The client prompted for a
  password change but nothing refused privileged requests without one, so the
  prompt was advisory. Enforced as described above; this is a breaking change
  for devices still on `admin`/`admin`.
- **Authenticating user could reboot the device.** Several WiFi paths called
  `esp_wifi_*` under `ESP_ERROR_CHECK` from the HTTP request handler, and the
  STA path did not validate the SSID before persisting it, so a malformed
  `POST /api/config` was a remote reboot. These now return errors.
- **Login lockout was device-wide.** Five failed guesses from any client locked
  every other client out for five minutes, with no credentials needed — the
  rate limiter was an availability bug pointed at the legitimate owner.
  Counters are now per address.
- **Relay debounce bypassed.** `POST /api/power` with an explicit boolean
  bypassed the minimum interval that protects the power relay from rapid
  cycling.
