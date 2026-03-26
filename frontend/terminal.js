// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Dennis Gilmore

(function() {
    "use strict";

    var ws = null;
    var term = null;
    var fitAddon = null;
    var sessionToken = null;
    var reconnectTimer = null;
    var RECONNECT_DELAY = 3000;

    // --- DOM refs ---
    var statusDot = document.getElementById("status-dot");
    var statusText = document.getElementById("status-text");
    var baudSelect = document.getElementById("baud-select");
    var btnReset = document.getElementById("btn-reset");
    var btnPower = document.getElementById("btn-power");
    var btnLogout = document.getElementById("btn-logout");
    var btnSettings = document.getElementById("btn-settings");
    var loginOverlay = document.getElementById("login-overlay");
    var loginError = document.getElementById("login-error");
    var settingsOverlay = document.getElementById("settings-overlay");
    var terminalContainer = document.getElementById("terminal-container");

    // --- Helpers ---
    function setStatus(connected, text) {
        statusDot.classList.toggle("connected", connected);
        statusText.textContent = text;
    }

    function authHeaders() {
        var h = {"Content-Type": "application/json"};
        if (sessionToken) h["Authorization"] = "Bearer " + sessionToken;
        return h;
    }

    function apiRequest(method, url, data, cb) {
        var xhr = new XMLHttpRequest();
        xhr.open(method, url, true);
        var hdrs = authHeaders();
        for (var k in hdrs) xhr.setRequestHeader(k, hdrs[k]);
        xhr.onload = function() {
            try { cb(null, JSON.parse(xhr.responseText), xhr.status); }
            catch(e) { cb(e); }
        };
        xhr.onerror = function() { cb(new Error("Network error")); };
        xhr.send(data ? JSON.stringify(data) : null);
    }

    // --- Login ---
    document.getElementById("login-form").addEventListener("submit", function(e) {
        e.preventDefault();
        loginError.textContent = "";
        var u = document.getElementById("login-user").value;
        var p = document.getElementById("login-pass").value;

        apiRequest("POST", "/api/login", {username: u, password: p}, function(err, data, status) {
            if (err || status !== 200) {
                loginError.textContent = (data && data.error) || "Login failed";
                return;
            }
            sessionToken = data.token;
            loginOverlay.classList.add("hidden");

            if (data.must_change_password && term) {
                term.writeln("\r\n*** Default credentials. Change password in Settings. ***\r\n");
            }

            connectWs();
            loadConfig();
        });
    });

    // --- WebSocket ---
    function connectWs() {
        if (reconnectTimer) { clearTimeout(reconnectTimer); reconnectTimer = null; }
        if (!sessionToken) return;

        var proto = location.protocol === "https:" ? "wss:" : "ws:";
        ws = new WebSocket(proto + "//" + location.host + "/ws");
        ws.binaryType = "arraybuffer";

        ws.onopen = function() { setStatus(true, "Connected"); };
        ws.onmessage = function(evt) {
            if (!term) return;
            if (evt.data instanceof ArrayBuffer) {
                term.write(new Uint8Array(evt.data));
            } else {
                term.write(evt.data);
            }
        };
        ws.onclose = function() {
            setStatus(false, "Disconnected");
            if (sessionToken) {
                reconnectTimer = setTimeout(connectWs, RECONNECT_DELAY);
            }
        };
        ws.onerror = function() { setStatus(false, "Connection error"); };
    }

    // --- Config ---
    function loadConfig() {
        apiRequest("GET", "/api/config", null, function(err, conf) {
            if (err || !conf) return;
            if (conf.baud_rate) baudSelect.value = String(conf.baud_rate);
            updatePowerButton(conf.power_on);
        });
    }

    baudSelect.addEventListener("change", function() {
        var baud = parseInt(baudSelect.value, 10);
        apiRequest("POST", "/api/config", {baud_rate: baud}, function(err, res) {
            if (!err && res && res.ok && term) {
                term.writeln("\r\n[Baud rate changed to " + baud + "]\r\n");
            }
        });
    });

    // --- Reset ---
    btnReset.addEventListener("click", function() {
        if (!confirm("Reset the SBC?")) return;
        apiRequest("POST", "/api/reset", null, function(err, res) {
            if (!err && res && res.ok && term) {
                term.writeln("\r\n[SBC reset triggered]\r\n");
            }
        });
    });

    // --- Power ---
    function updatePowerButton(powerOn) {
        btnPower.textContent = powerOn ? "Power: ON" : "Power: OFF";
        btnPower.classList.toggle("power-on", powerOn);
        btnPower.classList.toggle("power-off", !powerOn);
    }

    btnPower.addEventListener("click", function() {
        apiRequest("POST", "/api/power", {}, function(err, res) {
            if (!err && res && res.ok) {
                updatePowerButton(res.power_on);
                if (term) term.writeln("\r\n[SBC power " + (res.power_on ? "ON" : "OFF") + "]\r\n");
            }
        });
    });

    // --- Settings ---
    var settingsStatus = document.getElementById("settings-status");
    var wifiStatus = document.getElementById("wifi-status");

    function showSettingsMsg(msg, isError) {
        settingsStatus.textContent = msg;
        settingsStatus.style.color = isError ? "var(--danger)" : "var(--success)";
    }

    btnSettings.addEventListener("click", function() {
        settingsOverlay.classList.remove("hidden");
        settingsStatus.textContent = "";
        apiRequest("GET", "/api/config", null, function(err, conf) {
            if (err || !conf) return;
            document.getElementById("sta-ssid").value = conf.sta_ssid || "";
            document.getElementById("sta-pass").value = "";
            document.getElementById("power-default").checked = conf.power_on_default;
            var ws = "";
            if (conf.wifi_mode === "ap") {
                ws = "Mode: AP only";
            } else if (conf.sta_connected) {
                ws = "Connected: " + conf.sta_ssid + " (" + conf.sta_ip + ")";
            } else {
                ws = "Disconnected from " + conf.sta_ssid;
                wifiStatus.style.color = "var(--warning)";
            }
            wifiStatus.textContent = ws;
        });
    });

    document.getElementById("btn-settings-close").addEventListener("click", function() {
        settingsOverlay.classList.add("hidden");
    });

    document.getElementById("btn-wifi-save").addEventListener("click", function() {
        var ssid = document.getElementById("sta-ssid").value.trim();
        var pass = document.getElementById("sta-pass").value;
        if (!ssid) { showSettingsMsg("SSID required", true); return; }
        apiRequest("POST", "/api/config", {sta_ssid: ssid, sta_pass: pass}, function(err, res) {
            if (!err && res && res.ok) {
                showSettingsMsg("Connecting to " + ssid + "...");
            } else {
                showSettingsMsg((res && res.error) || "Failed", true);
            }
        });
    });

    document.getElementById("btn-pass-save").addEventListener("click", function() {
        var p1 = document.getElementById("new-pass").value;
        var p2 = document.getElementById("new-pass2").value;
        if (!p1 || p1.length < 4) { showSettingsMsg("Min 4 characters", true); return; }
        if (p1 !== p2) { showSettingsMsg("Passwords don't match", true); return; }
        apiRequest("POST", "/api/config", {new_password: p1}, function(err, res) {
            if (!err && res && res.ok) {
                showSettingsMsg("Password changed");
                document.getElementById("new-pass").value = "";
                document.getElementById("new-pass2").value = "";
            } else {
                showSettingsMsg((res && res.error) || "Failed", true);
            }
        });
    });

    document.getElementById("power-default").addEventListener("change", function() {
        apiRequest("POST", "/api/config", {power_on_default: this.checked}, function(err, res) {
            if (!err && res && res.ok) showSettingsMsg("Saved");
        });
    });

    // --- Logout ---
    btnLogout.addEventListener("click", function() {
        apiRequest("POST", "/api/logout", null, function() {
            sessionToken = null;
            if (ws) ws.close();
            loginOverlay.classList.remove("hidden");
        });
    });

    // --- Terminal init (deferred until xterm.js loads) ---
    function initTerminal() {
        if (term) return;
        term = new Terminal({
            cursorBlink: true, fontSize: 14,
            fontFamily: "'Fira Code', 'Cascadia Code', Menlo, monospace",
            theme: { background: "#1a1a2e", foreground: "#e0e0e0", cursor: "#0ea5e9" },
            scrollback: 5000, convertEol: true,
        });
        fitAddon = new FitAddon.FitAddon();
        term.loadAddon(fitAddon);
        term.open(terminalContainer);
        fitAddon.fit();
        term.onData(function(data) {
            if (ws && ws.readyState === WebSocket.OPEN) {
                ws.send(new TextEncoder().encode(data));
            }
        });
        window.addEventListener("resize", function() { if (fitAddon) fitAddon.fit(); });
    }

    // Poll for xterm.js availability
    var retries = 0;
    var poll = setInterval(function() {
        if (typeof Terminal !== "undefined" && typeof FitAddon !== "undefined") {
            clearInterval(poll);
            initTerminal();
        } else if (++retries > 100) {
            clearInterval(poll);
        }
    }, 200);

    // --- Startup: check existing session or show login ---
    apiRequest("GET", "/api/config", null, function(err, conf, status) {
        if (!err && status === 200 && conf) {
            sessionToken = "cookie";
            loginOverlay.classList.add("hidden");
            connectWs();
            if (conf.baud_rate) baudSelect.value = String(conf.baud_rate);
            updatePowerButton(conf.power_on);
        }
        // else login overlay is already visible
    });

})();
