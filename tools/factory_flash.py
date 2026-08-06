#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Dennis Gilmore
#
# Standalone factory flasher for ESP32 Web Terminal.
#
# Downloads a published factory image (bootloader + partition table + app) from
# GitHub releases and writes it to a blank board. The device boots unconfigured
# and goes through the normal first-time setup (AP mode, forced password
# change). Optionally, --config pre-seeds device settings into NVS.
#
# Only dependency: pip install esptool
# (plus esp-idf-nvs-partition-gen when using --config)
#
# Examples:
#   factory_flash.py -p /dev/ttyUSB0                      # latest release, chip auto-detected
#   factory_flash.py -p /dev/ttyUSB0 --release v1.4.2     # specific version
#   factory_flash.py -p /dev/ttyUSB0 --local              # use ./build from a local idf.py build
#   factory_flash.py -p /dev/ttyUSB0 --file factory.bin   # flash a downloaded image
#   factory_flash.py -p /dev/ttyUSB0 --config dev.json    # also pre-seed NVS config
#
# --config JSON format (every key optional; omitted keys keep firmware defaults):
#   {
#     "device_name": "rack-pi-01",             // <=32 chars, also sets hostname/mDNS
#     "sta_ssid": "mynetwork",                 // WiFi network to join
#     "sta_pass": "secret",
#     "ap_ssid": "ESP-Terminal",               // <=27 chars (-XXXX MAC suffix appended)
#     "ap_pass": "esp32term",                  // 8-64 chars, or "" for open AP
#     "username": "admin",
#     "password": "random",                    // literal password, or "random" to generate
#     "force_password_change": false,          // require password change on first login
#     "baud_rate": 115200,
#     "power_on_default": true,
#     "ntp_server": "",                        // "" = DHCP discovery
#     "timezone": "UTC0"                       // POSIX TZ string
#   }

import argparse
import csv
import hashlib
import json
import os
import re
import secrets
import subprocess
import sys
import tempfile
import urllib.error
import urllib.request

GITHUB_REPO = "ausil/esp32-web-terminal"
TARGETS = ("esp32c3", "esp32c6", "esp32s3")
BAUD = "460800"

# NVS partition layout from partitions.csv (same for all targets); parsed from
# the repo copy when the script runs from a checkout, these as fallback
NVS_OFFSET = 0x9000
NVS_SIZE = 0x6000

# Must match main/config.c
PBKDF2_ITERATIONS = 10000
AUTH_SALT_LEN = 16
AUTH_HASH_LEN = 32

VALID_BAUDS = {9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600, 1500000}

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))


def die(msg):
    print(f"error: {msg}", file=sys.stderr)
    sys.exit(1)


def run(cmd, **kwargs):
    print("+ " + " ".join(cmd))
    return subprocess.run(cmd, check=True, **kwargs)


def esptool_base(port, chip=None):
    cmd = [sys.executable, "-m", "esptool", "--port", port, "--baud", BAUD]
    if chip:
        cmd += ["--chip", chip]
    return cmd


def check_esptool():
    try:
        subprocess.run([sys.executable, "-m", "esptool", "version"],
                       capture_output=True, check=True)
    except (subprocess.CalledProcessError, FileNotFoundError):
        die("esptool not found — install with: pip install esptool")


def detect_chip(port):
    print("Detecting chip...")
    result = subprocess.run([sys.executable, "-m", "esptool", "--port", port, "chip-id"],
                            capture_output=True, text=True)
    m = re.search(r"(ESP32-[A-Z][A-Z0-9]*|ESP32)\b", result.stdout + result.stderr)
    if not m:
        die("could not detect chip type — check the port, or pass --target explicitly")
    chip = m.group(1).lower().replace("-", "")
    if chip not in TARGETS:
        die(f"detected {m.group(1)}, which is not a supported target ({', '.join(TARGETS)})")
    print(f"Detected {m.group(1)}")
    return chip


def github_get(url):
    req = urllib.request.Request(url, headers={
        "User-Agent": "esp32-web-terminal-factory-flash",
        "Accept": "application/vnd.github.v3+json",
    })
    with urllib.request.urlopen(req, timeout=30) as resp:
        return json.load(resp)


def download_factory_image(target, release_tag, dest_dir):
    api = f"https://api.github.com/repos/{GITHUB_REPO}/releases"
    if not release_tag:
        print(f"Fetching release info from {api}/latest")
        release_tag = github_get(f"{api}/latest").get("tag_name")
        if not release_tag:
            die("could not determine the latest release tag")
    # Factory images live in a companion "<tag>-factory" prerelease so the
    # main release stays small enough for the firmware's OTA update check
    if not release_tag.endswith("-factory"):
        release_tag += "-factory"
    url = f"{api}/tags/{release_tag}"
    print(f"Fetching release info from {url}")
    try:
        release = github_get(url)
    except urllib.error.HTTPError as e:
        if e.code == 404:
            die(f"no factory release {release_tag} found — "
                f"factory images are published for releases v1.5.0 and later")
        raise
    tag = release.get("tag_name", "?")

    asset_name = f"esp32-web-terminal-{target}-factory.bin"
    asset = next((a for a in release.get("assets", []) if a["name"] == asset_name), None)
    if not asset:
        available = ", ".join(a["name"] for a in release.get("assets", [])) or "none"
        die(f"release {tag} has no {asset_name} (assets: {available}) — "
            f"factory images are published for releases after v1.4.2")

    dest = os.path.join(dest_dir, asset_name)
    print(f"Downloading {asset_name} ({asset['size']} bytes) from release {tag}...")
    req = urllib.request.Request(asset["browser_download_url"],
                                 headers={"User-Agent": "esp32-web-terminal-factory-flash"})
    with urllib.request.urlopen(req, timeout=300) as resp, open(dest, "wb") as f:
        f.write(resp.read())
    return dest, tag


# --- Optional NVS pre-seeding (--config) ---

def parse_nvs_partition():
    """Read nvs offset/size from the repo's partitions.csv when available."""
    path = os.path.join(os.path.dirname(SCRIPT_DIR), "partitions.csv")
    if not os.path.isfile(path):
        return NVS_OFFSET, NVS_SIZE
    with open(path) as f:
        for line in f:
            fields = [x.strip() for x in line.split(",")]
            if len(fields) >= 5 and fields[0] == "nvs":
                return int(fields[3], 0), int(fields[4], 0)
    return NVS_OFFSET, NVS_SIZE


def find_nvs_gen():
    """Return the command prefix for nvs_partition_gen: the pip package if
    installed, otherwise the copy inside an exported ESP-IDF."""
    try:
        __import__("esp_idf_nvs_partition_gen")
        return [sys.executable, "-m", "esp_idf_nvs_partition_gen.nvs_partition_gen"]
    except ImportError:
        pass
    idf_path = os.environ.get("IDF_PATH", "")
    script = os.path.join(idf_path, "components", "nvs_flash",
                          "nvs_partition_generator", "nvs_partition_gen.py")
    if idf_path and os.path.isfile(script):
        return [sys.executable, script]
    die("nvs_partition_gen not found — install with: pip install esp-idf-nvs-partition-gen")


def build_nvs_image(config_path, workdir):
    """Generate an NVS image pre-seeding the 'webterm' namespace (keys must
    match main/config.c). Returns (image_path, generated_password_or_None)."""
    with open(config_path) as f:
        cfg = json.load(f)

    def get(key, default=""):
        return cfg.get(key, default)

    # Validate what's present; anything omitted keeps firmware defaults
    if len(get("sta_ssid")) > 32:
        die("sta_ssid exceeds 32 characters")
    if len(get("sta_pass")) > 64:
        die("sta_pass exceeds 64 characters")
    # 27-char limit: firmware appends a "-XXXX" MAC suffix (32-byte SSID limit)
    if len(get("ap_ssid")) > 27:
        die("ap_ssid exceeds 27 characters")
    if get("ap_pass") and not 8 <= len(get("ap_pass")) <= 64:
        die("ap_pass must be 8-64 characters, or empty for an open AP")
    if len(get("device_name")) > 32:
        die("device_name exceeds 32 characters")
    if len(get("ntp_server")) > 64:
        die("ntp_server exceeds 64 characters")
    if len(get("timezone")) > 40:
        die("timezone exceeds 40 characters")
    if get("baud_rate") and get("baud_rate") not in VALID_BAUDS:
        die(f"baud_rate must be one of {sorted(VALID_BAUDS)}")

    rows = [("key", "type", "encoding", "value"), ("webterm", "namespace", "", "")]
    for cfg_key, nvs_key in (("sta_ssid", "sta_ssid"), ("sta_pass", "sta_pass"),
                             ("ap_ssid", "ap_ssid"), ("ap_pass", "ap_pass"),
                             ("device_name", "dev_name"), ("ntp_server", "ntp_srv"),
                             ("timezone", "timezone")):
        if get(cfg_key):
            rows.append((nvs_key, "data", "string", cfg[cfg_key]))
    if get("baud_rate"):
        rows.append(("baud_rate", "data", "u32", str(cfg["baud_rate"])))
    if "power_on_default" in cfg:
        rows.append(("power_on", "data", "u8", "1" if cfg["power_on_default"] else "0"))

    generated_password = None
    password = get("password")
    if password:
        if password == "random":
            password = generated_password = secrets.token_urlsafe(12)
        salt = secrets.token_bytes(AUTH_SALT_LEN)
        pw_hash = hashlib.pbkdf2_hmac("sha256", password.encode(), salt,
                                      PBKDF2_ITERATIONS, dklen=AUTH_HASH_LEN)
        rows.append(("auth_user", "data", "string", get("username", "admin")))
        rows.append(("auth_hash", "data", "hex2bin", pw_hash.hex()))
        rows.append(("auth_salt", "data", "hex2bin", salt.hex()))
        rows.append(("hash_ver", "data", "u8", "1"))
        # Password was chosen deliberately — skip the forced first-login change
        # unless the config asks for it
        rows.append(("auth_init", "data", "u8",
                     "0" if cfg.get("force_password_change") else "1"))

    csv_path = os.path.join(workdir, "nvs.csv")
    with open(csv_path, "w", newline="") as f:
        csv.writer(f).writerows(rows)

    _, nvs_size = parse_nvs_partition()
    image = os.path.join(workdir, "nvs.bin")
    run(find_nvs_gen() + ["generate", csv_path, image, f"0x{nvs_size:x}"])
    return image, generated_password


# --- Main flash flow ---

def main():
    ap = argparse.ArgumentParser(
        description="Flash a factory image (bootloader + partition table + firmware) "
                    "from GitHub releases onto an ESP32 Web Terminal board")
    ap.add_argument("-p", "--port", required=True, help="serial port (e.g. /dev/ttyUSB0)")
    ap.add_argument("-t", "--target", choices=TARGETS,
                    help="chip target (auto-detected if omitted)")
    src = ap.add_mutually_exclusive_group()
    src.add_argument("--release", metavar="TAG",
                     help="release tag to flash (default: latest)")
    src.add_argument("--local", nargs="?", const="build", metavar="BUILD_DIR",
                     help="flash a local idf.py build instead of a release (default dir: build)")
    src.add_argument("--file", metavar="BIN",
                     help="flash a local factory image file at 0x0")
    ap.add_argument("--config", metavar="JSON",
                    help="optionally pre-seed device config (hostname, WiFi, auth, ...) into NVS")
    ap.add_argument("--no-erase", action="store_true",
                    help="skip the full flash erase (keeps any existing config)")
    args = ap.parse_args()

    check_esptool()
    chip = args.target or detect_chip(args.port)

    with tempfile.TemporaryDirectory() as workdir:
        nvs_image = None
        generated_password = None
        if args.config:
            nvs_image, generated_password = build_nvs_image(args.config, workdir)

        erase_args = [] if args.no_erase else ["--erase-all"]

        if args.local:
            build_dir = os.path.abspath(args.local)
            if not os.path.isfile(os.path.join(build_dir, "flash_args")):
                die(f"{build_dir}/flash_args not found — run idf.py build first")
            print(f"Flashing local build from {build_dir}...")
            run(esptool_base(args.port, chip) + ["write-flash"] + erase_args + ["@flash_args"],
                cwd=build_dir)
        else:
            if args.file:
                image = args.file
                if not os.path.isfile(image):
                    die(f"{image} not found")
            else:
                image, tag = download_factory_image(chip, args.release, workdir)
            print("Flashing factory image at 0x0...")
            run(esptool_base(args.port, chip) + ["write-flash"] + erase_args + ["0x0", image])

        if nvs_image:
            nvs_offset, _ = parse_nvs_partition()
            print(f"Writing pre-seeded NVS config at 0x{nvs_offset:x}...")
            run(esptool_base(args.port, chip) + ["write-flash", f"0x{nvs_offset:x}", nvs_image])

    print("\nDone. The board will reboot into the firmware.")
    if args.config:
        if generated_password:
            print(f"Generated admin password: {generated_password}  (store this securely)")
    else:
        print("First-time setup: join the ESP-Terminal-XXXX AP (password: esp32term),\n"
              "browse to https://192.168.4.1/ and log in as admin/admin.")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\naborted")
        sys.exit(130)
    except subprocess.CalledProcessError as e:
        die(f"command failed with exit code {e.returncode}")
    except urllib.error.URLError as e:
        die(f"download failed: {e}")
