#!/usr/bin/env bash
# Host check of the receiver's Wi-Fi mode (--stream-wifi) against a fake phone on 127.0.0.1: handshake over the command
# socket, video reassembled from its own socket, audio parsed from its socket, a key written to the touch socket, local
# ports 49512 and up, refused addresses, and a clean end when the phone closes. Linux with gcc and python3; needs root to
# create /dev/shmem for the key file.
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
src=$root/build/live-input-candidate/src
work=$(mktemp -d); trap 'kill $(jobs -p) 2>/dev/null || true; rm -rf "$work"; rm -f /dev/shmem/carlink-wifi-test-keys' EXIT
cat > "$work/main.c" <<'M'
#include <errno.h>
int carlink_usb_capture(int argc, char **argv);
int *__get_errno_ptr(void) { return &errno; }
int devctl(int a, int b, void *c, unsigned long d, int *e) { (void)a; (void)b; (void)c; (void)d; (void)e; return -1; }
/* The receiver keeps thread ids in 32-bit unsigned (QNX pthread_t); map them to real 64-bit pthread_t here. */
#include <pthread.h>
static pthread_t threads[16]; static unsigned thread_count;
int test_pthread_create(unsigned *id, const void *attr, void *(*fn)(void *), void *arg)
{ (void)attr; if (thread_count >= 16) return 1; const int rc = pthread_create(&threads[thread_count], 0, fn, arg); if (!rc) *id = thread_count++; return rc; }
int test_pthread_join(unsigned id, void **out) { return id < thread_count ? pthread_join(threads[id], out) : 1; }
int main(int argc, char **argv) { return carlink_usb_capture(argc, argv); }
M
gcc -O1 -std=gnu11 -w -DCARLINK_HOST_TEST -Dpthread_create=test_pthread_create -Dpthread_join=test_pthread_join -I"$src" \
    -c "$src/carlife_qnx_usb_capture.c" -o "$work/capture.o"
gcc -O1 -std=gnu11 -w -o "$work/capture" "$work/capture.o" "$work/main.c" -ldl -pthread
fail=0
pass() { echo "PASS: $1"; }
bad() { echo "FAIL: $1"; fail=1; }
mkdir -p /dev/shmem && : > /dev/shmem/carlink-wifi-test-keys
python3 "$root/tools/tests/wifi-fake-phone.py" "$work/phone.log" "$root/build/carlife-first-idr.1716310.h264" &
for _ in $(seq 1 30); do grep -q ready "$work/phone.log" 2>/dev/null && break; sleep 0.2; done
( sleep 2.5; echo RIGHT >> /dev/shmem/carlink-wifi-test-keys ) &
rc=0
CARLINK_SESSION_SECONDS=0 timeout 30 "$work/capture" --stream-wifi 127.0.0.1 --input-file /dev/shmem/carlink-wifi-test-keys \
    > "$work/spool.bin" 2> "$work/err.log" || rc=$?
wait %1 2>/dev/null || true
grep -q "Wi-Fi: connected to 127.0.0.1:7240" "$work/err.log" && grep -q "Wi-Fi: connected to 127.0.0.1:9340" "$work/err.log" \
    && pass "all six ports connected" || { bad "connect"; cat "$work/err.log"; }
ports=$(grep -o "local port [0-9]*" "$work/phone.log" | awk '{print $3}')
ok=1; for p in $ports; do [ "$p" -ge 49512 ] || ok=0; done
[ -n "$ports" ] && [ "$ok" = 1 ] && pass "local source ports are 49512 or higher ($(echo $ports | tr '\n' ' '))" || bad "source ports: $ports"
grep -q "hu cmd 0x00018001 08011000" "$work/phone.log" && pass "HU_PROTOCOL_VERSION 1.0 sent on the command socket without an AOA header" || { bad "version"; cat "$work/phone.log"; }
grep -q "hu cmd 0x00018007" "$work/phone.log" && grep -q "hu cmd 0x00018009" "$work/phone.log" && grep -q "hu cmd 0x0001801d" "$work/phone.log" \
    && pass "handshake continued: video init, encoder start" || bad "handshake"
grep -q "Phone encoder: 1280x720 @ 30 fps" "$work/err.log" && pass "VIDEO_ENCODER_INIT_DONE parsed" || bad "encoder"
grep -q "Key input enabled: first video frame queued" "$work/err.log" && [ -s "$work/spool.bin" ] \
    && pass "video frames reassembled from the video socket and queued ($(stat -c %s "$work/spool.bin") bytes written)" || { bad "video"; cat "$work/err.log"; }
grep -q "Audio format: 44100 Hz, 2 channel(s), 16 bits" "$work/err.log" && grep -q "Audio: 1 PCM records" "$work/err.log" \
    && pass "audio format and PCM record parsed from the audio socket (split across reads)" || { bad "audio"; grep -i audio "$work/err.log"; }
grep -q "touch 0x00068008 0816" "$work/phone.log" && pass "RIGHT went to the touch socket as 0x00068008 08 16" || { bad "key"; grep touch "$work/phone.log"; }
grep -q "Latency: key written to the phone" "$work/err.log" && pass "the key opened a latency window" || bad "latency"
grep -q "Wi-Fi: channel 1 (port 7240) closed by the phone" "$work/err.log" && pass "the phone closing the command socket ends the session" || bad "close"
[ "$rc" = 12 ] && pass "receiver ended by itself with the session-ended status (exit 12, as over USB)" || bad "receiver exit $rc"
for a in "" "8.8.8.8" "172.16.224.1" "172.16.225.9" "172.16.224.256" "1.2.3"; do
    "$work/capture" --stream-wifi "$a" > /dev/null 2>&1 && r=0 || r=$?
    [ "$r" = 2 ] || bad "address '$a' gave exit $r"
done
pass "addresses outside the hotspot network are refused before anything is opened"
"$work/capture" --stream-wifi 127.0.0.1 > /dev/null 2> "$work/none.log" && r=0 || r=$?
[ "$r" = 10 ] && grep -q "port 7240 not reachable" "$work/none.log" && pass "no phone listening: exit 10 with the reason logged" || { bad "no phone: $r"; cat "$work/none.log"; }
# --wifi-wait: find the phone whose port 7240 is open (the supervisor uses it before starting a session)
python3 -c "
import socket,time
s=socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); s.bind(('127.0.0.1',7240)); s.listen(4)
open('$work/w.ready','w').close(); time.sleep(6)" &
for _ in $(seq 1 30); do [ -e "$work/w.ready" ] && break; sleep 0.2; done
out=$(CARLINK_WIFI_WAIT=3 "$work/capture" --wifi-wait 127.0.0.2,127.0.0.1 2>/dev/null) && r=0 || r=$?
[ "$r" = 0 ] && [ "$out" = 127.0.0.1 ] && pass "--wifi-wait prints the address whose port 7240 is open" || bad "wifi-wait found: '$out' exit $r"
wait
start=$(date +%s)
CARLINK_WIFI_WAIT=2 "$work/capture" --wifi-wait 127.0.0.1 > /dev/null 2>&1 && r=0 || r=$?
took=$(( $(date +%s) - start ))
[ "$r" = 1 ] && [ "$took" -ge 2 ] && [ "$took" -le 5 ] && pass "--wifi-wait gives up after CARLINK_WIFI_WAIT seconds (exit 1, ${took} s)" || bad "wifi-wait none: exit $r after $took s"
for a in "8.8.8.8" "172.16.224.5,,172.16.224.6" "172.16.224.1" "a,b"; do
    "$work/capture" --wifi-wait "$a" > /dev/null 2>&1 && r=0 || r=$?
    [ "$r" = 2 ] || bad "wifi-wait list '$a' gave exit $r"
done
pass "--wifi-wait refuses lists with an address outside the hotspot"
exit $fail
