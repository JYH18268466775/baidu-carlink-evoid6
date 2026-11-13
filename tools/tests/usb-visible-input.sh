#!/usr/bin/env bash
# Offline check of CARLINK_INPUT_FILE handling in build/live-input-candidate/tools/carlife_usb_visible.sh.
# Every /dev/ path in a copy of the launcher is redirected into a temp directory and the capture
# and replay programs are fakes that record their command lines. Nothing touches a head unit.
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
script=${LAUNCHER:-$root/build/live-input-candidate/tools/carlife_usb_visible.sh}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
fake=$work/dev
mkdir -p "$fake/shmem"

sed "s#/dev/#$fake/#g" "$script" > "$work/launcher.sh"
cat > "$work/probe" <<'EOF'
#!/bin/sh
echo "$*" >> "$FAKE_LOG"
if [ "$1" = "--switch" ]; then : > "$FAKE_ROOT/oap1"; fi
exit 0
EOF
cat > "$work/replay" <<'EOF'
#!/bin/sh
echo "replay $*" >> "$FAKE_LOG"
cat > /dev/null
exit 0
EOF
printf '#!/bin/sh\nexit 0\n' > "$fake/shmem/carlife_visible_first_frame.sh"
chmod +x "$work/probe" "$work/replay"

run() {  # run SHELL INPUT_VALUE|- [--diagnose-usb]; sets status, stdout/stderr in $work/err
    local shell=$1 input=$2 mode=${3:-$work/replay}
    rm -f "$fake"/android* "$fake"/oap* "$work/log" "$work/err"
    : > "$work/log"
    ( sleep 1; : > "$fake/android5" ) &
    local env=(FAKE_LOG="$work/log" FAKE_ROOT="$fake")
    [ "$input" = "-" ] || env+=(CARLINK_INPUT_FILE="$input")
    status=0
    env "${env[@]}" timeout 40 "$shell" "$work/launcher.sh" "$work/probe" "$mode" > /dev/null 2> "$work/err" || status=$?
    wait
}

expect() {  # expect DESCRIPTION CONDITION...
    local what=$1; shift
    if ! "$@"; then
        echo "FAIL: $what"; echo "--- launcher log:"; cat "$work/log"; echo "--- stderr:"; cat "$work/err"
        exit 1
    fi
}
log_has() { grep -qxF -- "$1" "$work/log"; }
log_lacks() { ! grep -qF -- "$1" "$work/log"; }

long48=$(printf 'a%.0s' $(seq 48))
long49=$(printf 'a%.0s' $(seq 49))

for shell in sh bash; do
    echo "== launcher under $shell"
    run "$shell" -
    expect "no input: exit 0" [ "$status" -eq 0 ]
    expect "no input: capture runs without --input-file" log_has "--stream-visible $fake/oap1"
    expect "no input: --input-file never appears" log_lacks "--input-file"

    printf 'OK\nBACK\n' > "$fake/shmem/keys"
    run "$shell" "$fake/shmem/keys"
    expect "input: exit 0" [ "$status" -eq 0 ]
    expect "input: capture gets --input-file" log_has "--stream-visible $fake/oap1 --input-file $fake/shmem/keys"
    expect "input: stale content is emptied" [ ! -s "$fake/shmem/keys" ]
    expect "input: file exists after the run" [ -e "$fake/shmem/keys" ]

    run "$shell" "$fake/shmem/$long48"
    expect "48-character name is accepted" [ "$status" -eq 0 ]

    for bad in "$work/keys" "$fake/shmem/" "$fake/shmem/.." "$fake/shmem/." "$fake/shmem/a/b" \
               "$fake/shmem/a b" "$fake/shmem/$long49" "$fake/shmemx/keys"; do
        run "$shell" "$bad"
        expect "refused: [$bad]" [ "$status" -eq 2 ]
        expect "refused: nothing was started for [$bad]" [ ! -s "$work/log" ]
    done

    run "$shell" "$fake/shmem/keys" --diagnose-usb
    expect "diagnose: exit 0" [ "$status" -eq 0 ]
    expect "diagnose: capture runs without --input-file" log_has "--stream-visible $fake/oap1"
    expect "diagnose: --input-file never appears" log_lacks "--input-file"
    expect "diagnose: user is told the input file is ignored" grep -q "ignored by --diagnose-usb" "$work/err"
done
echo "PASS: CARLINK_INPUT_FILE is validated early, emptied at start, passed only to the live display run"
