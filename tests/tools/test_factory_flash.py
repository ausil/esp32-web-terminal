"""Host-side tests for tools/factory_flash.py — no serial port, no network.

Covers the parts that can silently corrupt a board: the NVS pre-seed CSV
generation (key names must match main/config.c exactly), the auth hash
parity with the firmware, and the release-asset selection logic.

Run:  python3 -m pytest tests/tools
"""
import csv
import hashlib
import importlib.util
import os
import re
import sys

import pytest

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
TOOLS = os.path.join(REPO, "tools")

# Import tools/factory_flash.py as a module without packaging tricks
_spec = importlib.util.spec_from_file_location(
    "factory_flash", os.path.join(TOOLS, "factory_flash.py"))
ff = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(ff)


# --- Crypto parity with the firmware -------------------------------------

def read_c_define(path, name):
    """Scrape `#define NAME value` from a C source/header."""
    with open(os.path.join(REPO, path)) as f:
        m = re.search(r"^#define\s+%s\s+(\S+)" % name, f.read(), re.M)
    assert m, f"#define {name} not found in {path}"
    return int(m.group(1), 0)


def test_auth_constants_match_firmware():
    """A pre-seeded password only works if the tool hashes exactly like the
    firmware does. These constants are the contract; drift means every
    factory-flashed board locks its owner out."""
    assert ff.PBKDF2_ITERATIONS == read_c_define("main/config.c", "PBKDF2_ITERATIONS")
    assert ff.AUTH_SALT_LEN == read_c_define("main/config.h", "CONFIG_AUTH_SALT_LEN")
    assert ff.AUTH_HASH_LEN == read_c_define("main/config.h", "CONFIG_AUTH_HASH_LEN")


# Same vector as the C suite (tests/host/test_config.c) — if either side
# changes its KDF, one of the two suites goes red.
PARITY_SALT = "6b2f8e4a1d3c5b7a9e0f1c2d3e4f5061"
PARITY_PW = "factory-parity-check"
PARITY_HASH = "1d814c1ceca29cd19b89fe147ba988c03cd383e0d1dfeeb7c13cbbf73b4c933b"


def test_pbkdf2_parity_vector():
    out = hashlib.pbkdf2_hmac("sha256", PARITY_PW.encode(), bytes.fromhex(PARITY_SALT),
                              ff.PBKDF2_ITERATIONS, dklen=ff.AUTH_HASH_LEN)
    assert out.hex() == PARITY_HASH


# --- NVS partition parsing --------------------------------------------------

def test_parse_nvs_partition_reads_repo_csv():
    """The real partitions.csv: nvs at 0x9000, size 0x6000."""
    assert ff.parse_nvs_partition() == (0x9000, 0x6000)


def test_parse_nvs_partition_custom_and_fallback(tmp_path, monkeypatch):
    # parse_nvs_partition looks in dirname(SCRIPT_DIR), so emulate the
    # real layout: SCRIPT_DIR=<here>/tools, CSV one level up.
    (tmp_path / "tools").mkdir()
    p = tmp_path / "partitions.csv"
    p.write_text("# Name, Type, SubType, Offset, Size\n"
                 "nvs, data, nvs, 0x12000, 0x8000\n")
    monkeypatch.setattr(ff, "SCRIPT_DIR", str(tmp_path / "tools"))
    assert ff.parse_nvs_partition() == (0x12000, 0x8000)
    # No CSV next to the script: fall back to the baked-in constants
    p.unlink()
    assert ff.parse_nvs_partition() == (ff.NVS_OFFSET, ff.NVS_SIZE)


# --- build_nvs_image: CSV generation ---------------------------------------

@pytest.fixture
def nvs_image(tmp_path, monkeypatch):
    """Runs build_nvs_image with the nvs_partition_gen subprocess stubbed out;
    returns (rows_by_key, image_result_tuple, run_cmd)."""
    calls = {}

    def fake_find():
        return ["echo"]

    def fake_run(cmd, **kw):
        calls["cmd"] = cmd

    monkeypatch.setattr(ff, "find_nvs_gen", fake_find)
    monkeypatch.setattr(ff, "run", fake_run)

    def build(cfg):
        import json
        cfg_path = tmp_path / "cfg.json"
        cfg_path.write_text(json.dumps(cfg))
        workdir = tmp_path / "wd"
        workdir.mkdir(exist_ok=True)
        image, gen_pw = ff.build_nvs_image(str(cfg_path), str(workdir))
        with open(os.path.join(str(workdir), "nvs.csv")) as f:
            rows = list(csv.reader(f))
        return {r[0]: tuple(r) for r in rows[1:]}, (image, gen_pw), calls.get("cmd")
    return build


def test_nvs_basic_rows_and_key_mapping(nvs_image):
    rows, (image, gen_pw), cmd = nvs_image({
        "sta_ssid": "lab", "sta_pass": "secret123",
        "device_name": "rack-pi-01", "ntp_server": "pool.ntp.org",
        "timezone": "EST5EDT", "baud_rate": 230400, "power_on_default": True,
    })
    assert gen_pw is None
    assert image.endswith("nvs.bin")
    # Namespace declaration must be the first data row
    assert rows["webterm"][1] == "namespace"
    # NVS key names must match what config.c reads — the short ones bite here
    assert rows["dev_name"] == ("dev_name", "data", "string", "rack-pi-01")
    assert rows["ntp_srv"] == ("ntp_srv", "data", "string", "pool.ntp.org")
    assert rows["sta_ssid"][3] == "lab"
    assert rows["baud_rate"] == ("baud_rate", "data", "u32", "230400")
    assert rows["power_on"] == ("power_on", "data", "u8", "1")
    # ...and the generator got invoked with the parsed partition size
    assert cmd[0] == "echo" and cmd[1] == "generate"
    assert cmd[2].endswith("nvs.csv") and cmd[3].endswith("nvs.bin")
    assert cmd[4] == "0x6000"   # size parsed from the repo partitions.csv


def test_nvs_password_rows(nvs_image):
    rows, (_, gen_pw), _ = nvs_image({"password": "hunter2hunter2", "username": "root"})
    assert gen_pw is None
    assert rows["auth_user"] == ("auth_user", "data", "string", "root")
    assert rows["hash_ver"] == ("hash_ver", "data", "u8", "1")
    # Deliberate password => no forced change
    assert rows["auth_init"] == ("auth_init", "data", "u8", "1")
    salt = bytes.fromhex(rows["auth_salt"][3])
    assert len(salt) == ff.AUTH_SALT_LEN
    expect = hashlib.pbkdf2_hmac("sha256", b"hunter2hunter2", salt,
                                 ff.PBKDF2_ITERATIONS, dklen=ff.AUTH_HASH_LEN)
    assert rows["auth_hash"][3] == expect.hex()


def test_nvs_random_password_and_force_change(nvs_image):
    rows, (_, gen_pw), _ = nvs_image({"password": "random", "force_password_change": True})
    assert gen_pw and len(gen_pw) >= 12
    salt = bytes.fromhex(rows["auth_salt"][3])
    expect = hashlib.pbkdf2_hmac("sha256", gen_pw.encode(), salt,
                                 ff.PBKDF2_ITERATIONS, dklen=ff.AUTH_HASH_LEN)
    assert rows["auth_hash"][3] == expect.hex()
    # force_password_change => auth_init 0 => firmware shows the lock banner
    assert rows["auth_init"] == ("auth_init", "data", "u8", "0")


def test_nvs_no_password_no_auth_rows(nvs_image):
    rows, _, _ = nvs_image({"sta_ssid": "lab"})
    for k in ("auth_user", "auth_hash", "auth_salt", "hash_ver", "auth_init"):
        assert k not in rows


@pytest.mark.parametrize("cfg,frag", [
    ({"sta_ssid": "x" * 33}, "sta_ssid"),
    ({"sta_pass": "x" * 65}, "sta_pass"),
    ({"ap_ssid": "x" * 28}, "ap_ssid"),
    ({"ap_pass": "short"}, "ap_pass"),
    ({"device_name": "x" * 33}, "device_name"),
    ({"timezone": "x" * 41}, "timezone"),
    ({"baud_rate": 12345}, "baud_rate"),
])
def test_nvs_validation_exits(nvs_image, cfg, frag):
    with pytest.raises(SystemExit):
        nvs_image(cfg)


# --- release asset selection --------------------------------------------------

class _FakeResp:
    def __init__(self, body):
        self._body = body
    def read(self):
        return self._body
    def __enter__(self):
        return self
    def __exit__(self, *a):
        return False


def _fake_release(tag="v1.5.0-factory", assets=None):
    return {"tag_name": tag, "assets": assets if assets is not None else [
        {"name": "esp32-web-terminal-esp32c6-factory.bin", "size": 4,
         "browser_download_url": "https://download.invalid/factory.bin"},
    ]}


def test_release_tag_gets_factory_suffix(monkeypatch, tmp_path):
    seen = []
    monkeypatch.setattr(ff, "github_get", lambda url: seen.append(url) or _fake_release())
    monkeypatch.setattr("urllib.request.urlopen", lambda req, **kw: _FakeResp(b"FACT"))
    dest, tag = ff.download_factory_image("esp32c6", "v1.5.0", str(tmp_path))
    assert seen == ["https://api.github.com/repos/%s/releases/tags/v1.5.0-factory"
                    % ff.GITHUB_REPO]
    assert tag == "v1.5.0-factory"
    assert dest.endswith("esp32-web-terminal-esp32c6-factory.bin")
    assert open(dest, "rb").read() == b"FACT"


def test_latest_release_resolves_then_factory(monkeypatch, tmp_path):
    seen = []
    def fake_get(url):
        seen.append(url)
        return {"tag_name": "v1.6.0"} if url.endswith("/latest") else _fake_release("v1.6.0-factory")
    monkeypatch.setattr(ff, "github_get", fake_get)
    monkeypatch.setattr("urllib.request.urlopen", lambda req, **kw: _FakeResp(b"x"))
    _, tag = ff.download_factory_image("esp32c6", None, str(tmp_path))
    assert seen[0].endswith("/releases/latest")
    assert seen[1].endswith("/tags/v1.6.0-factory")
    assert tag == "v1.6.0-factory"


def test_release_missing_asset_dies(monkeypatch):
    monkeypatch.setattr(ff, "github_get", lambda url: _fake_release(assets=[
        {"name": "esp32-web-terminal-esp32c3-factory.bin", "size": 1},
    ]))
    with pytest.raises(SystemExit):
        ff.download_factory_image("esp32s3", "v1.5.0", "/tmp")


def test_missing_factory_release_dies(monkeypatch):
    import urllib.error
    def raise404(url):
        raise urllib.error.HTTPError(url, 404, "Not Found", {}, None)
    monkeypatch.setattr(ff, "github_get", raise404)
    with pytest.raises(SystemExit):
        ff.download_factory_image("esp32c6", "v1.4.2", "/tmp")
