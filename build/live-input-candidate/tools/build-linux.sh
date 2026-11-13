#!/usr/bin/env bash
# Linux equivalent of tools/build-qnx-probes.ps1 (same flags, same link steps) using clang-15 / ld.lld-15.
set -euo pipefail
project=${1:?project dir (build/live-input-candidate)}
out=${2:-$project/build-linux}
CLANG=${CLANG:-clang-15}; LLD=${LLD:-ld.lld-15}
work=$out/obj; mkdir -p "$out" "$work"
cd "$project"
ps1=tools/build-qnx-probes.ps1
# stub libc: header from the ps1 here-string, symbols from its list
python3 - "$ps1" "$work/libc-link-only.S" <<'P'
import re, sys
s = open(sys.argv[1]).read()
hdr = re.search(r"\$stubHeader = @'\r?\n(.*?)\r?\n'@", s, re.S).group(1).replace('\r\n', '\n')
syms = re.search(r"\$symbols = @\((.*?)\)\s*\n\s*\$stub =", s, re.S).group(1)
names = re.findall(r"'([^']+)'", syms)
open(sys.argv[2], 'w', newline='').write(hdr + '\n' + '\n'.join('symbol ' + n for n in names) + '\n')
P
target="--target=armv7-none-eabi -mfpu=neon -mfloat-abi=softfp"  # the Windows toolchain defaults to these (byte-identical status ELF)
$CLANG $target -c "$work/libc-link-only.S" -o "$work/libc-link-only.o"
$LLD -m armelf -shared --hash-style=sysv -soname libc.so.3 -o "$work/libc-link-only.so" "$work/libc-link-only.o"
stub=$work/libc-link-only.so
python3 - src/legacy/carlife_qnx_dyn_probe.ld "$work/probe.ld" <<'P'
import sys
t = open(sys.argv[1], newline='').read()
t = t.replace('*(.text) }', '*(.text .text.*) *(.rodata .rodata.*) }')
t = t.replace('/DISCARD/ : { *(.note.GNU-stack) }', '/DISCARD/ : { *(.note.GNU-stack) *(.ARM.exidx*) *(.ARM.extab*) *(.comment) }')
open(sys.argv[2], 'w', newline='').write(t)
P
cflags=($target -marm -std=c11 -ffreestanding -fno-unwind-tables -fno-asynchronous-unwind-tables -fno-stack-protector -Wall -Wextra -Werror -Os)
link=(-m armelf -z max-page-size=4096 --hash-style=sysv --dynamic-linker /usr/lib/ldqnx.so.2 -T "$work/probe.ld")
one() { # name entry source
    $CLANG $target -DCARLINK_PROBE_ENTRY=$2 -c src/carlife_qnx_mmr_start.S -o "$work/$1-start.o"
    $CLANG "${cflags[@]}" -c "src/$3" -o "$work/$1.o"
    $LLD "${link[@]}" -o "$out/carlife_qnx_$4.elf" "$work/$1-start.o" "$work/$1.o" "$stub"
}
one usb-status carlink_usb_status carlife_qnx_usb_status.c usb_status
one usb-capture carlink_usb_capture carlife_qnx_usb_capture.c usb_capture
one cp-purge carlink_cp_purge carlife_qnx_cp_purge.c cp_purge
one most-watch carlink_most_watch carlife_qnx_most_watch.c most_watch
one ctl carlink_ctl carlife_qnx_ctl.c ctl
one udp-probe carlink_udp_probe carlife_qnx_udp_probe.c udp_probe
one tcp-probe carlink_tcp_probe carlife_qnx_tcp_probe.c tcp_probe
one sniff carlink_sniff carlife_qnx_sniff.c sniff
one bssrole carlink_bssrole carlife_qnx_bssrole.c bssrole
# replay (with embedded first GOP)
printf '.section .rodata\n.global carlink_gop_start, carlink_gop_end\ncarlink_gop_start:\n.incbin "samples/carlife-mdev-first-gop.bin"\ncarlink_gop_end:\n' > "$work/gop.S"
$CLANG $target -c src/carlife_qnx_mmr_start.S -o "$work/mmr-start.o"
$CLANG "${cflags[@]}" -DCARLINK_ENABLE_SHORT_REPLAY -c src/carlife_qnx_mmr_connect.c -o "$work/mmr-replay-output.o"
$CLANG "${cflags[@]}" -c src/carlife_qnx_short_replay.c -o "$work/short-replay.o"
$CLANG $target -c src/legacy/carlife_qnx_mdev_arm.S -o "$work/mdev.o"
$CLANG $target -c "$work/gop.S" -o "$work/gop.o"
$LLD "${link[@]}" --allow-shlib-undefined -o "$out/carlife_qnx_mmr_replay.elf" "$work/mmr-start.o" "$work/mmr-replay-output.o" "$work/short-replay.o" "$work/mdev.o" "$work/gop.o" "$stub" references/qnx/libscreen.so.1
sha256sum "$out"/*.elf | sed 's#  .*/#  #'
