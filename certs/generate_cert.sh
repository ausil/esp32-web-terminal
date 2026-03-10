#!/bin/bash
# Generate a self-signed ECC P-256 certificate for the ESP32 HTTPS server.
# Run this once before building. The generated cert/key are embedded in firmware.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo "Generating ECC P-256 private key..."
openssl ecparam -genkey -name prime256v1 -noout -out server.key

echo "Generating self-signed certificate (valid 10 years)..."
openssl req -new -x509 -key server.key -out server.crt -days 3650 \
    -subj "/CN=ESP32 Web Terminal/O=ESP32/C=US" \
    -addext "subjectAltName=IP:192.168.4.1,DNS:esp-terminal.local"

echo "Done. Files created:"
echo "  $SCRIPT_DIR/server.crt"
echo "  $SCRIPT_DIR/server.key"
echo ""
echo "Certificate fingerprint:"
openssl x509 -in server.crt -noout -fingerprint -sha256
