#!/usr/bin/env bash
# Offline check for the live spool ring and continuous-session switches. Nothing here touches the head unit.
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
    gcc -O1 -g -std=gnu11 -w "${renames[@]}" -I"$2" -c "$tests/usb-ring-impl.c" -o "$work/$1-impl.o"
    gcc -O1 -g -std=gnu11 -Wall -Wextra -pthread -c "$tests/usb-ring-host.c" -o "$work/$1-host.o"
    gcc -pthread "$work/$1-impl.o" "$work/$1-host.o" -o "$work/$1"
}
echo "== 1. Real receiver source: ring, wrap, backpressure, timestamps, continuous switches"
build real "$src"
"$work/real"
echo "== 2. Mutation checks"
expect_caught() {  # NAME SED TEXT
    mkdir "$work/$1-src"; cp "$src"/*.c "$src"/*.h "$work/$1-src/"
    sed -i "$2" "$work/$1-src/carlife_qnx_usb_capture.c"
    if cmp -s "$src/carlife_qnx_usb_capture.c" "$work/$1-src/carlife_qnx_usb_capture.c"; then echo "FAIL: mutation $1 changed nothing"; exit 1; fi
    build "$1" "$work/$1-src"
    if "$work/$1" > "$work/$1.out"; then cat "$work/$1.out"; echo "FAIL: mutation $1 was not caught"; exit 1; fi
    grep -q "$3" "$work/$1.out" || { cat "$work/$1.out"; echo "FAIL: mutation $1 failed for another reason"; exit 1; }
    echo "PASS: $1 caught -> $(grep -m1 "$3" "$work/$1.out")"
}
expect_caught no-wrap-copy 's/if (size > first)/if (0)/' 'FAIL: 40 MiB of records cross'
expect_caught no-backpressure 's/if (size > LIVE_RING - pending)/if (0)/' 'FAIL: a stalled pipe'
expect_caught no-remainder 's/capture->frames \/ 3u;/0;/' 'FAIL: timestamp'
expect_caught no-high-word 's/(unsigned)(micros >> 32)/0/' 'FAIL: timestamp'
expect_caught no-continuous-cap-skip 's/if (!capture->continuous \&\&/if (1 \&\&/' 'FAIL: continuous session keeps'
expect_caught no-alarm-cancel 's/alarm(0); \/\* the startup/0; \/* the startup/' 'FAIL: continuous session cancels'
echo "== 3. ARM32 build flags with -Werror (compile only)"
for f in carlife_qnx_usb_capture.c carlife_qnx_short_replay.c; do
  clang --target=armv7-none-eabi -marm -std=c11 -ffreestanding -fno-unwind-tables -fno-asynchronous-unwind-tables \
    -fno-stack-protector -Wall -Wextra -Werror -Os -I"$src" -c "$src/$f" -o "$work/arm-$f.o"
done
clang --target=armv7-none-eabi -marm -std=c11 -ffreestanding -fno-unwind-tables -fno-asynchronous-unwind-tables \
    -fno-stack-protector -Wall -Wextra -Werror -Os -DCARLINK_ENABLE_SHORT_REPLAY -I"$src" -c "$src/carlife_qnx_mmr_connect.c" -o "$work/arm-mmr.o"
echo "PASS: all three compile for armv7 without warnings"
