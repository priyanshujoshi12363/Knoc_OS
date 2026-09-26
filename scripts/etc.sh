#!/usr/bin/env bash
set -eu

DISK=$1
KNOCFS="python3 tools/knocfs.py"

$KNOCFS mkdir "$DISK" /etc /etc/ssl 2>/dev/null || true

for bundle in /etc/ssl/certs/ca-certificates.crt /etc/pki/tls/certs/ca-bundle.crt /etc/ssl/cert.pem; do
    if [ -f "$bundle" ]; then
        $KNOCFS put "$DISK" "$bundle" /etc/ssl/certs.pem
        break
    fi
done

if ! $KNOCFS cat "$DISK" /etc/hosts > /dev/null 2>&1; then
    $KNOCFS put-text "$DISK" /etc/hosts "10.0.2.2 host pc
10.0.2.15 knocos
"
fi
