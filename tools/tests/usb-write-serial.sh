#!/usr/bin/env bash
# Offline check for the serialized USB write path in build/live-input-candidate.
# Needs gcc, clang (ARM target) and node. Nothing here touches the head unit.
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
src=$root/build/live-input-candidate/src
tests=$root/tools/tests
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

renames=(-Dopen=test_open -Dclose=test_close -Ddevctl=test_devctl -Dread=test_read
         -Dwrite=test_write -Dalarm=test_alarm -Dusleep=test_usleep
         -Dpthread_create=test_pthread_create -Dpthread_join=test_pthread_join)

build() {  # build NAME SRC_DIR
    gcc -O1 -g -std=gnu11 -w "${renames[@]}" -I"$2" -c "$tests/usb-write-serial-impl.c" -o "$work/$1-impl.o"
    gcc -O1 -g -std=gnu11 -Wall -Wextra -pthread -c "$tests/usb-write-serial-host.c" -o "$work/$1-host.o"
    gcc -pthread "$work/$1-impl.o" "$work/$1-host.o" -o "$work/$1"
}

echo "== 1. Real receiver source, concurrent writers, failure injection"
build locked "$src"
"$work/locked"

echo "== 2. Mutation checks: each safeguard removed in turn must be caught"
expect_caught() {  # NAME SED_EXPRESSION TEXT_THE_TEST_MUST_PRINT
    mkdir "$work/$1-src"
    cp "$src"/*.c "$src"/*.h "$work/$1-src/"
    sed -i "$2" "$work/$1-src/carlife_qnx_usb_capture.c"
    if cmp -s "$src/carlife_qnx_usb_capture.c" "$work/$1-src/carlife_qnx_usb_capture.c"; then
        echo "FAIL: mutation $1 changed nothing"; exit 1
    fi
    build "$1" "$work/$1-src"
    if "$work/$1" > "$work/$1.out"; then
        cat "$work/$1.out"
        echo "FAIL: mutation $1 was not caught"; exit 1
    fi
    grep -q "$3" "$work/$1.out" || { cat "$work/$1.out"; echo "FAIL: mutation $1 failed for another reason"; exit 1; }
    echo "PASS: $1 caught -> $(grep -m1 "$3" "$work/$1.out")"
}
expect_caught no-lock 's/__atomic_exchange_n(&usb_write_lock, 1, __ATOMIC_ACQUIRE)/0/' INTERLEAVED
expect_caught no-gate 's/__atomic_load_n(&input->capture->input_ready, __ATOMIC_ACQUIRE)/1/' 'FAIL: keys before the first frame'
expect_caught no-close-guard 's/if (usb_closed) {/if (0) {/' 'FAIL: a late writer'
expect_caught no-key-gap 's/INPUT_KEY_GAP_US = 5000/INPUT_KEY_GAP_US = 0/' 'FAIL: consecutive keys'

echo "== 3. Bytes match carlife_idrive_keys.cjs (OK, BACK)"
"$work/locked" identity | sed -n 's/^HEX://p' | tr -d '\n' > "$work/c.hex"
node "$root/src/carlife_idrive_keys.cjs" OK BACK | od -An -v -tx1 | tr -d ' \n' > "$work/js.hex"
[ -s "$work/c.hex" ] && [ -s "$work/js.hex" ]
cmp "$work/c.hex" "$work/js.hex"
echo "PASS: C and JS emit identical outer/inner bytes"

echo "== 4. Handshake send_cmd bytes unchanged vs the car-verified 30 s candidate source"
base=$root/build/live-30s-candidate/src
mkdir "$work/base-src"
cp "$base"/*.c "$base"/*.h "$work/base-src/"
cat > "$work/base-wrap.c" <<'EOF'
#include "carlife_qnx_usb_capture.c"
int hook_send_cmd(int fd, unsigned type, const unsigned char *body, unsigned size)
{
    return send_cmd(fd, type, body, size);
}
int hook_send_hardkey(int fd, unsigned code)
{
    (void)fd; (void)code;
    return -1;
}
EOF
gcc -O1 -g -std=gnu11 -w "${renames[@]}" -I"$work/base-src" -c "$work/base-wrap.c" -o "$work/base-impl.o"
gcc -O1 -g -std=gnu11 -Wall -Wextra -pthread -c "$tests/usb-write-serial-host.c" -o "$work/base-host.o"
gcc -pthread "$work/base-impl.o" "$work/base-host.o" -o "$work/base"
"$work/base" cmdbytes > "$work/base.cmd"
"$work/locked" cmdbytes > "$work/new.cmd"
[ "$(wc -l < "$work/new.cmd")" -eq 114 ]
cmp "$work/base.cmd" "$work/new.cmd"
echo "PASS: 57 body sizes (114 USB writes) identical to the verified source"

echo "== 5. ARM32 build flags with -Werror (compile only, no ELF produced)"
clang --target=armv7-none-eabi -marm -std=c11 -ffreestanding -fno-unwind-tables \
    -fno-asynchronous-unwind-tables -fno-stack-protector -Wall -Wextra -Werror -Os \
    -I"$src" -c "$src/carlife_qnx_usb_capture.c" -o "$work/arm.o"
echo "PASS: compiles for armv7 without warnings"
