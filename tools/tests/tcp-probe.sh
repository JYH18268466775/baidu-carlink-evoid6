#!/usr/bin/env bash
# Host check of the outbound TCP connect probe: open/refused/silent/banner reporting, local source ports, address limits,
# and that it never writes to a connection.
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
src=$root/build/live-input-candidate/src/carlife_qnx_tcp_probe.c
work=$(mktemp -d); trap 'kill $(jobs -p) 2>/dev/null || true; rm -rf "$work"' EXIT
cat > "$work/main.c" <<'M'
int carlink_tcp_probe(int argc, char **argv);
int main(int argc, char **argv) { return carlink_tcp_probe(argc, argv); }
M
gcc -O1 -std=gnu11 -w -DCARLINK_PROBE_HOST -o "$work/probe" "$src" "$work/main.c" -ldl
fail=0
cat > "$work/servers.py" <<'P'
import socket, sys, threading, time
log = open(sys.argv[1], "w", buffering=1)
def serve(port, banner):
    s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", port)); s.listen(5)
    def loop():
        while True:
            c, a = s.accept()
            log.write("accept %d from_port=%d\n" % (port, a[1]))
            if banner: c.sendall(banner)
            c.settimeout(3)
            try:
                data = c.recv(100)
                log.write("received %d bytes on %d\n" % (len(data), port))
            except Exception: pass
            c.close()
    threading.Thread(target=loop, daemon=True).start()
serve(7240, b"HELLO")
serve(8240, None)
serve(9340, b"\x00\x01\x02")
log.write("ready\n"); time.sleep(40)
P
python3 "$work/servers.py" "$work/server.log" & 
for _ in 1 2 3 4 5 6 7 8 9 10; do grep -q ready "$work/server.log" 2>/dev/null && break; sleep 0.3; done
rc=0; "$work/probe" --scan 127.0.0.1 > "$work/out.txt" 2>&1 || rc=$?
[ "$rc" = 0 ] && echo "PASS: exit 0 when a port is open" || { echo "FAIL: exit $rc"; cat "$work/out.txt"; fail=1; }
grep -q "port 7240 OPEN, sent 5 bytes first: 48 45 4c 4c 4f" "$work/out.txt" && echo "PASS: banner of an open port is dumped" || { echo "FAIL: banner"; cat "$work/out.txt"; fail=1; }
grep -q "port 8240 OPEN, silent" "$work/out.txt" && echo "PASS: a silent open port is reported silent" || { echo "FAIL: silent"; cat "$work/out.txt"; fail=1; }
grep -q "port 9340 OPEN, sent 3 bytes first: 00 01 02" "$work/out.txt" && echo "PASS: binary banner" || { echo "FAIL: binary banner"; cat "$work/out.txt"; fail=1; }
grep -q "port 9240 refused" "$work/out.txt" && grep -q "port 9241 refused" "$work/out.txt" && echo "PASS: closed ports are reported refused" || { echo "FAIL: refused"; cat "$work/out.txt"; fail=1; }
grep -q "done, 3 port(s) open" "$work/out.txt" && echo "PASS: summary counts open ports" || { echo "FAIL: summary"; cat "$work/out.txt"; fail=1; }
ports=$(grep -o "from_port=[0-9]*" "$work/server.log" | cut -d= -f2)
ok=1; for p in $ports; do [ "$p" -ge 49512 ] && [ "$p" -lt 50400 ] || ok=0; done
[ -n "$ports" ] && [ "$ok" = 1 ] && echo "PASS: local source ports are 49512 or higher" || { echo "FAIL: source ports: $ports"; fail=1; }
if grep -q -E "^received [1-9]" "$work/server.log"; then echo "FAIL: the probe sent data"; cat "$work/server.log"; fail=1; else echo "PASS: nothing was sent to the open ports"; fi
for bad in "" "8.8.8.8" "172.16.224.1" "172.16.224.255" "172.16.225.5" "172.16.224" "172.16.224.7.9" "abc" "172.16.224.300"; do
    if [ -z "$bad" ]; then "$work/probe" > /dev/null 2>&1 && r=0 || r=$?; else "$work/probe" --scan "$bad" > /dev/null 2>&1 && r=0 || r=$?; fi
    [ "$r" = 2 ] || { echo "FAIL: address '$bad' gave exit $r"; fail=1; }
done
echo "PASS: addresses outside the hotspot network and malformed ones are refused (checked above, no FAIL lines)"
"$work/probe" --scan 172.16.224.50 > /dev/null 2>&1 &
sleep 0.3; kill %2 2>/dev/null || true
if grep -n -E "(^|[^a-z_])(send|sendto|write|sendmsg)\(" "$src" | grep -v "^[0-9]*: *\*" | grep -q .; then echo "FAIL: the probe source contains a send/write call"; fail=1; else echo "PASS: no send or write call in the probe source"; fi
if command -v clang >/dev/null; then
    clang --target=armv7-none-eabi -marm -std=c11 -ffreestanding -fno-unwind-tables -fno-asynchronous-unwind-tables \
        -fno-stack-protector -Wall -Wextra -Werror -Os -c "$src" -o "$work/arm.o" && echo "PASS: probe compiles for armv7 without warnings" || fail=1
fi
exit $fail
