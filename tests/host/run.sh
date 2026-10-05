#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Host-side unit tests for the platform-independent parts of esp32-zapret.
# Usage: bash tests/host/run.sh   (override compiler with CC=...)
set -euo pipefail

cd "$(dirname "$0")"

CC="${CC:-gcc}"
CFLAGS="-std=c17 -Wall -Wextra -Werror -O1"
ROOT="../.."

OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

echo "== TLS parser / fake ClientHello tests =="
"$CC" $CFLAGS \
    -Istub \
    -I"$ROOT/components/esp_desync" \
    -I"$ROOT/components/esp_desync/include" \
    test_tls.c stub/esp_random.c "$ROOT/components/esp_desync/desync_tls.c" \
    -o "$OUT/tls_tests"
"$OUT/tls_tests"

echo "== net utils tests =="
"$CC" $CFLAGS -I"$ROOT/main" test_net.c "$ROOT/main/net_utils.c" -o "$OUT/net_tests"
"$OUT/net_tests"

echo "ALL HOST TESTS PASSED"
