#!/usr/bin/env bash
# Offline check of the USB receive resync (record whose AOA header was lost). Real receiver source, scripted reads.
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
src=${1:-$root/build/live-input-candidate/src}
tests=$root/tools/tests
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
renames=(-Dopen=test_open -Dclose=test_close -Ddevctl=test_devctl -Dread=test_read
         -Dwrite=test_write -Dalarm=test_alarm -Dusleep=test_usleep
         -Dpthread_create=test_pthread_create -Dpthread_join=test_pthread_join)
gcc -O1 -g -std=gnu11 -w "${renames[@]}" -I"$src" -c "$tests/usb-resync-impl.c" -o "$work/impl.o"
gcc -O1 -g -std=gnu11 -Wall -Wextra -c "$tests/usb-resync-host.c" -o "$work/host.o"
gcc "$work/impl.o" "$work/host.o" -pthread -ldl -o "$work/resync"
gcc -O1 -std=gnu11 -Wall -Wextra -shared -fPIC "$tests/fake-asound.c" -o "$work/fake-asound.so"
rm -f /tmp/fake_asound_closed
FAKE_OUT="$work/played.pcm" CARLINK_ASOUND_SHIM="$work/fake-asound.so" "$work/resync"
