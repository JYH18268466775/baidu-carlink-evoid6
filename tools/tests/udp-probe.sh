#!/usr/bin/env bash
# Host check of the read-only UDP probe: it logs datagrams (including a broadcast-style sender) and sends nothing.
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
src=$root/build/live-input-candidate/src/carlife_qnx_udp_probe.c
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
cat > "$work/main.c" <<'M'
int carlink_udp_probe(int argc, char **argv);
int main(int argc, char **argv) { return carlink_udp_probe(argc, argv); }
M
gcc -O1 -std=gnu11 -w -DCARLINK_PROBE_HOST -o "$work/probe" "$src" "$work/main.c" -ldl
fail=0
"$work/probe" --listen 7999 1 > "$work/none.out" 2>&1 || rc=$?
[ "${rc:-0}" = 1 ] && grep -q "0 datagram" "$work/none.out" && echo "PASS: quiet network -> exit 1, nothing logged" || { echo "FAIL: quiet run"; cat "$work/none.out"; fail=1; }
( sleep 0.6; python3 - <<'P'
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.sendto(bytes([0xde, 0xad, 0xbe, 0xef, 1, 2, 3]), ("127.0.0.1", 7999))
s.sendto(b"hello" * 20, ("127.0.0.1", 7999))
P
) &
"$work/probe" --listen 7999 3 > "$work/got.out" 2>&1 || true
wait
grep -q "from 127.0.0.1:.* 7 bytes: de ad be ef 01 02 03" "$work/got.out" && grep -q "100 bytes: 68 65 6c 6c 6f" "$work/got.out" && grep -q "2 datagram" "$work/got.out" && echo "PASS: datagrams logged with sender, length and first bytes" || { echo "FAIL: logging"; cat "$work/got.out"; fail=1; }
"$work/probe" --listen 80 5 > "$work/bad1.out" 2>&1 && bad=0 || bad=$?
"$work/probe" --listen 7999 0 > "$work/bad2.out" 2>&1 && bad2=0 || bad2=$?
"$work/probe" > "$work/bad3.out" 2>&1 && bad3=0 || bad3=$?
[ "$bad" = 2 ] && [ "$bad2" = 2 ] && [ "$bad3" = 2 ] && echo "PASS: bad arguments are refused" || { echo "FAIL: arguments $bad $bad2 $bad3"; fail=1; }
if grep -q -E "sendto|send\(|connect\(" "$src"; then echo "FAIL: the probe source contains a send/connect call"; fail=1; else echo "PASS: no send or connect call in the probe source"; fi
if command -v clang >/dev/null; then
    clang --target=armv7-none-eabi -marm -std=c11 -ffreestanding -fno-unwind-tables -fno-asynchronous-unwind-tables \
        -fno-stack-protector -Wall -Wextra -Werror -Os -c "$src" -o "$work/arm.o" && echo "PASS: probe compiles for armv7 without warnings" || fail=1
fi
exit $fail
