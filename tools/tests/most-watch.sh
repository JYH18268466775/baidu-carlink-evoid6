#!/usr/bin/env bash
# Tests build/live-input-candidate/src/carlife_qnx_most_watch.c (real source, fake DSI library).
# UPSTREAM_FIXTURES / UPSTREAM_SRC (optional): upstream NBT-AAD tests/fixtures and src dirs for the decoder oracle.
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
src=$here/../../build/live-input-candidate/src/carlife_qnx_most_watch.c
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/shm"
gcc -w -DCARLINK_SHMEM_DIR="\"$tmp/shm/\"" -o "$tmp/host" "$here/most-watch-host.c"
"$tmp/host" "$here/fixtures/most-watch-own.txt" > "$tmp/own.txt"
echo "PASS: handshake requests, notification order, forbidden requests refused, noise ignored, argument rules"
# --emit: lines the capture program's input file would receive
: > "$tmp/shm/keys"
"$tmp/host" "$here/fixtures/most-watch-own.txt" "$tmp/shm/keys" > /dev/null 2> "$tmp/emit.err"
grep -q '^RC=0$' "$tmp/emit.err" || { echo "FAIL: emit run did not succeed"; cat "$tmp/emit.err"; exit 1; }
python3 - "$tmp/shm/keys" <<'PY'
import sys
lines = open(sys.argv[1]).read().split("\n")
assert lines[-1] == "", "last line must end with a newline"
lines = lines[:-1]
expect = ["+3", "RIGHT", "OK", "LEFT", "+1", "BACK", "BACK", "+2", "-3", "OK", "RIGHT", "BACK"]
assert lines == expect, (lines, expect)
PY
echo "PASS: --emit writes exactly the CarLife action lines, one per line"
# --watch 0 --emit: runs until CARLINK_WATCH_STOP appears, no watchdog alarm
: > "$tmp/shm/keys2"
WATCH_MODE=forever MOCK_STOP_AFTER=7 CARLINK_WATCH_STOP="$tmp/shm/stop" "$tmp/host" "$here/fixtures/most-watch-own.txt" "$tmp/shm/keys2" > /dev/null 2> "$tmp/fe.err"
grep -q '^RC=0$' "$tmp/fe.err" && grep -q '^SLEEPS=7$' "$tmp/fe.err" && grep -q '^ALARM=0$' "$tmp/fe.err" && cmp -s "$tmp/shm/keys" "$tmp/shm/keys2" \
    || { echo "FAIL: --watch 0 did not stop on the stop file or changed the emitted lines"; cat "$tmp/fe.err"; exit 1; }
for env in "X=1" "CARLINK_WATCH_STOP=/tmp/elsewhere" "CARLINK_WATCH_STOP=$tmp/shm/stop CARLINK_EMIT_HIDE=/tmp/elsewhere"; do
    : > "$tmp/shm/keys3"
    # shellcheck disable=SC2086
    env WATCH_MODE=forever $env "$tmp/host" "$here/fixtures/most-watch-own.txt" "$tmp/shm/keys3" > /dev/null 2> "$tmp/fe2.err"
    grep -q '^RC=2$' "$tmp/fe2.err" || { echo "FAIL: forever run accepted '$env'"; cat "$tmp/fe2.err"; exit 1; }
done
echo "PASS: --watch 0 needs --emit and a stop file under /dev/shmem, stops when it appears, sets no alarm"
# hide file: nothing is emitted while it exists, and the held-back lines are counted
: > "$tmp/shm/keys4"; : > "$tmp/shm/hide"
CARLINK_EMIT_HIDE="$tmp/shm/hide" "$tmp/host" "$here/fixtures/most-watch-own.txt" "$tmp/shm/keys4" > /dev/null 2> "$tmp/hide.err"
grep -q '^RC=0$' "$tmp/hide.err" && [ ! -s "$tmp/shm/keys4" ] && grep -q '12 held back' "$tmp/hide.err" \
    || { echo "FAIL: actions were emitted while the hide file existed"; cat "$tmp/hide.err"; exit 1; }
: > "$tmp/shm/keys4"; rm -f "$tmp/shm/hide"
CARLINK_EMIT_HIDE="$tmp/shm/hide" "$tmp/host" "$here/fixtures/most-watch-own.txt" "$tmp/shm/keys4" > /dev/null 2> "$tmp/hide.err"
cmp -s "$tmp/shm/keys" "$tmp/shm/keys4" || { echo "FAIL: actions were lost with no hide file"; exit 1; }
echo "PASS: no action reaches the phone while the hide file exists; all of them do once it is gone"
for bad in "$tmp/shm/missing" "$tmp/shm/bad name" "/tmp/elsewhere" "$tmp/shm/.hidden" "$tmp/shm/"; do
    "$tmp/host" "$here/fixtures/most-watch-own.txt" "$bad" > /dev/null 2> "$tmp/bad.err"
    grep -qE '^RC=(2|6)$' "$tmp/bad.err" || { echo "FAIL: emit path '$bad' was accepted"; cat "$tmp/bad.err"; exit 1; }
done
echo "x" > "$tmp/shm/full"
"$tmp/host" "$here/fixtures/most-watch-own.txt" "$tmp/shm/full" > /dev/null 2> "$tmp/bad.err"
grep -q '^RC=7$' "$tmp/bad.err" && [ "$(cat "$tmp/shm/full")" = "x" ] || { echo "FAIL: non-empty emit file was not refused untouched"; exit 1; }
echo "PASS: emit path rules and a non-empty file is refused without being modified"
if command -v clang >/dev/null; then
    clang --target=armv7-none-eabi -marm -std=c11 -ffreestanding -fno-unwind-tables -fno-asynchronous-unwind-tables \
        -fno-stack-protector -Wall -Wextra -Werror -Os -c "$src" -o "$tmp/arm.o"
    echo "PASS: MOST watch compiles for armv7 without warnings"
fi
if [ -n "${UPSTREAM_SRC:-}" ] && [ -d "$UPSTREAM_SRC" ]; then
    g++ -std=gnu++98 -w -I"$UPSTREAM_SRC" -o "$tmp/oracle" "$here/most-watch-oracle.cpp"
    fixtures=("$here/fixtures/most-watch-own.txt")
    [ -n "${UPSTREAM_FIXTURES:-}" ] && fixtures+=("$UPSTREAM_FIXTURES"/controls_2026-10-22_e5.txt "$UPSTREAM_FIXTURES"/controls_2026-10-23_push.txt)
    for f in "${fixtures[@]}"; do
        "$tmp/host" "$f" > "$tmp/mine.txt"; "$tmp/oracle" "$f" > "$tmp/ref.txt"
        cmp -s "$tmp/mine.txt" "$tmp/ref.txt" || { echo "FAIL: decoding differs from the upstream decoder on $(basename "$f")"; diff "$tmp/mine.txt" "$tmp/ref.txt" | head; exit 1; }
        echo "PASS: $(wc -l < "$tmp/ref.txt") events on $(basename "$f") identical to the upstream decoder"
    done
else
    echo "SKIP: upstream decoder comparison (set UPSTREAM_SRC and UPSTREAM_FIXTURES)"
fi
# Mutation checks: the safeguards must be what the tests catch.
mut() { # name sed-expr
    sed "$2" "$src" > "$tmp/mut.c"
    sed "s#../../build/live-input-candidate/src/carlife_qnx_most_watch.c#$tmp/mut.c#" "$here/most-watch-host.c" > "$tmp/mut-host.c"
    gcc -w -o "$tmp/mut" "$tmp/mut-host.c"
    if "$tmp/mut" "$here/fixtures/most-watch-own.txt" >"$tmp/mut.out" 2>&1 && ( [ -z "${UPSTREAM_SRC:-}" ] || cmp -s "$tmp/mut.out" <("$tmp/oracle" "$here/fixtures/most-watch-own.txt") ); then
        echo "FAIL: mutation $1 was not caught"; exit 1
    fi
    echo "PASS: mutation $1 caught"
}
mut allow-any-request 's/if (id != REQ_REGISTER_CLIENT \&\& id != REQ_REGISTER_DEVCTRLS) {/if (0) {/'
mut allow-any-notification 's/    if (!allowed) {/    if (0) {/'
mut tilt-direction 's/return dir <= 5 ? 2 : dir <= 8 ? 3 : 4;/return dir <= 5 ? 4 : dir <= 8 ? 3 : 2;/'
# Mutation checks for the hide file and the stop file (they need the emit scenarios, not just decoding).
mut2() { # name sed-expr
    sed "$2" "$src" > "$tmp/mut.c"
    cmp -s "$src" "$tmp/mut.c" && { echo "FAIL: mutation $1 changed nothing"; exit 1; }
    sed "s#../../build/live-input-candidate/src/carlife_qnx_most_watch.c#$tmp/mut.c#" "$here/most-watch-host.c" > "$tmp/mut-host.c"
    gcc -w -DCARLINK_SHMEM_DIR="\"$tmp/shm/\"" -o "$tmp/mut" "$tmp/mut-host.c"
    : > "$tmp/shm/m1"; : > "$tmp/shm/m2"; : > "$tmp/shm/hide"; rm -f "$tmp/shm/stop"
    CARLINK_EMIT_HIDE="$tmp/shm/hide" "$tmp/mut" "$here/fixtures/most-watch-own.txt" "$tmp/shm/m1" > /dev/null 2>&1
    WATCH_MODE=forever MOCK_STOP_AFTER=7 CARLINK_WATCH_STOP="$tmp/shm/stop" timeout 10 "$tmp/mut" "$here/fixtures/most-watch-own.txt" "$tmp/shm/m2" > /dev/null 2> "$tmp/m2.err"
    if [ ! -s "$tmp/shm/m1" ] && grep -q '^SLEEPS=7$' "$tmp/m2.err"; then echo "FAIL: mutation $1 was not caught"; exit 1; fi
    echo "PASS: mutation $1 caught"; rm -f "$tmp/shm/hide"
}
mut2 hide-ignored 's/if (path_present(hide_path)) {/if (0) {/'
mut2 stop-ignored 's/while (!path_present(stop_path))/while (0)/'
