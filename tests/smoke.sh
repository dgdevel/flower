#!/usr/bin/env bash
# smoke.sh — end-to-end smoke tests for flower.
# Usage: tests/smoke.sh [path-to-binary]   (run from the repo root)
set -u

BIN=${1:-./flower}
PORT=${SMOKE_PORT:-8199}
CFG=$(mktemp -d)
LOG=$(mktemp)
PID=""
cleanup() { [ -n "$PID" ] && kill "$PID" 2>/dev/null; rm -rf "$CFG" "$LOG"; }
trap cleanup EXIT

fail() { echo "FAIL: $*" >&2; sed 's/^/  server: /' "$LOG" >&2; exit 1; }

[ -x "$BIN" ] || fail "binary not found: $BIN"
case "$BIN" in */*) ;; *) [ -x "./$BIN" ] && BIN="./$BIN" ;; esac

"$BIN" -p "$PORT" -c "$CFG" >"$LOG" 2>&1 &
PID=$!

B=http://127.0.0.1:$PORT
for _ in $(seq 1 50); do
    curl -s -o /dev/null "$B/" 2>/dev/null && break
    sleep 0.1
done
curl -s -o /dev/null "$B/" || fail "server did not come up on $B"

echo "== 1. embedded assets are byte-identical, no stray bytes =="
for f in index.html config.html components.html style.css theme.js config.js app.js; do
    curl -s "$B/$f" | cmp -s - "web/$f" || fail "$f: served bytes differ from web/$f"
done
for p in / /config.html /style.css /app.js /api/theme; do
    n=$(curl -s "$B$p" | tr -dc '\0' | wc -c)
    [ "$n" -eq 0 ] || fail "$p: $n NUL bytes in response body"
done

echo "== 2. default theme API =="
curl -s "$B/api/theme" | python3 -c '
import json, sys
d = json.load(sys.stdin)
keys = {"background_primary","background_secondary","background_tertiary",
        "text_primary_color","text_primary_font","text_primary_size",
        "text_secondary_color","text_secondary_font","text_secondary_size",
        "accent","success","warning"}
assert set(d) == keys, f"key mismatch: {set(d) ^ keys}"
for k in keys:
    if "color" in k or k in ("accent","success","warning","background_primary",
                             "background_secondary","background_tertiary"):
        assert d[k].startswith("#") and len(d[k]) == 7, f"bad color {k}={d[k]}"
' || fail "GET /api/theme returned an unexpected document"

echo "== 3. PUT a valid theme, verify persistence =="
curl -s -X PUT -H "Content-Type: application/json" --data-binary @- "$B/api/theme" <<'EOF' | grep -q '"accent":"#ff00ff"' || fail "PUT /api/theme did not echo the saved theme"
{"background_primary":"#101010","background_secondary":"#1a1a1a","background_tertiary":"#242424",
 "text_primary_color":"#eeeeee","text_primary_font":"Verdana, sans-serif","text_primary_size":"15px",
 "text_secondary_color":"#999999","text_secondary_font":"Georgia, serif","text_secondary_size":"12px",
 "accent":"#ff00ff","success":"#00cc66","warning":"#ff9900"}
EOF
[ -f "$CFG/theme.json" ] || fail "theme.json not written to config dir"
curl -s "$B/api/theme" | grep -q '"accent":"#ff00ff"' || fail "GET /api/theme after PUT does not reflect the change"

echo "== 4. validation rejects bad input =="
code=$(curl -s -o /dev/null -w '%{http_code}' -X PUT --data '{"accent":"nothex"}' "$B/api/theme")
[ "$code" = 422 ] || fail "invalid color: expected 422, got $code"
code=$(curl -s -o /dev/null -w '%{http_code}' -X PUT --data '{"nonsense":"x"}' "$B/api/theme")
[ "$code" = 422 ] || fail "unknown key: expected 422, got $code"
code=$(curl -s -o /dev/null -w '%{http_code}' -X PUT --data 'not json' "$B/api/theme")
[ "$code" = 400 ] || fail "malformed JSON: expected 400, got $code"
code=$(curl -s -o /dev/null -w '%{http_code}' -X PUT --data '{"text_primary_size":"9em; background:red"}' "$B/api/theme")
[ "$code" = 422 ] || fail "CSS injection in size: expected 422, got $code"
curl -s "$B/api/theme" | grep -q '"accent":"#ff00ff"' || fail "rejected PUT must not change the stored theme"

echo "== 5. reset restores defaults =="
curl -s -X POST "$B/api/theme/reset" | grep -q '"accent":"#58a6ff"' || fail "reset did not return defaults"
curl -s "$B/api/theme" | grep -q '"accent":"#58a6ff"' || fail "defaults not persisted after reset"

echo "== 6. SSE still streams =="
curl -sN --max-time 3 "$B/api/time" | grep -m1 -q '^data: {"unix"' || fail "no SSE event within 3s"

echo "== 7. connection semantics =="
python3 - "$PORT" <<'EOF' || exit 1
import socket, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=2)
s.sendall(b"GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
data, eof = b"", False
try:
    while True:
        c = s.recv(65536)
        if not c: eof = True; break
        data += c
except socket.timeout:
    pass
assert eof and b"200 OK" in data and b"\x00" not in data, "Connection: close not honored"
EOF
[ $? -eq 0 ] || fail "Connection: close handling regressed"

echo "ALL SMOKE TESTS PASSED"
