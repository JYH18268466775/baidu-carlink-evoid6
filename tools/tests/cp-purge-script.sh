#!/bin/sh
# Tests build/live-input-candidate/tools/carlife_cp_purge.sh with fake context and device directories.
set -u
here=$(cd "$(dirname "$0")/../.." && pwd)
script=$here/build/live-input-candidate/tools/carlife_cp_purge.sh
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
fail() { echo "FAIL: $1"; exit 1; }
elf=$tmp/fake.elf
printf '#!/bin/sh\necho "$@" >> %s/args\nexit ${FAKE_RC:-0}\n' "$tmp" > "$elf"; chmod +x "$elf"
run() { # context names..., env via ENV prefix by caller
    rm -rf "$tmp/ctx" "$tmp/dev" "$tmp/args"; mkdir -p "$tmp/ctx" "$tmp/dev"
    for n in $CTX; do mkdir "$tmp/ctx/$n"; done
    for n in $DEVS; do : > "$tmp/dev/$n"; done
    CARLINK_CONTEXT_ROOT=$tmp/ctx CARLINK_DEV_ROOT=$tmp/dev sh "$script" "$elf" ${YES---yes} >"$tmp/out" 2>&1
    echo $?
}
CTX=cpvideoctx16838 DEVS= ; rc=$(CTX=cpvideoctx16838 DEVS= run); [ "$rc" = 0 ] || fail "happy path rc=$rc"
grep -qx -- "--destroy cpvideoctx16838" "$tmp/args" || fail "elf not called with the context"
grep -q "unavailable until the head unit restarts" "$tmp/out" || fail "no reboot warning"
rc=$(CTX= DEVS= run); [ "$rc" = 0 ] && [ ! -e "$tmp/args" ] || fail "empty root must not call the elf"
rc=$(CTX="cpvideoctx1 aadvideoctx2" DEVS= run); [ "$rc" = 3 ] && [ ! -e "$tmp/args" ] || fail "two contexts"
rc=$(CTX=aadvideoctx9 DEVS= run); [ "$rc" = 3 ] && [ ! -e "$tmp/args" ] || fail "non-factory context"
rc=$(CTX=cpvideoctx DEVS= run); [ "$rc" = 3 ] && [ ! -e "$tmp/args" ] || fail "name without digits"
rc=$(CTX=cpvideoctx12x DEVS= run); [ "$rc" = 3 ] && [ ! -e "$tmp/args" ] || fail "non-digit name"
rc=$(CTX=cpvideoctx1 DEVS=oap01 run); [ "$rc" = 4 ] && [ ! -e "$tmp/args" ] || fail "phone node present"
rc=$(CTX=cpvideoctx1 DEVS=android01 run); [ "$rc" = 4 ] && [ ! -e "$tmp/args" ] || fail "android node present"
rc=$(CTX=cpvideoctx1 DEVS= YES= run); [ "$rc" = 2 ] && [ ! -e "$tmp/args" ] || fail "missing --yes"
rc=$(CTX=cpvideoctx1 DEVS= FAKE_RC=45 run); [ "$rc" = 45 ] || fail "elf failure must propagate (rc=$rc)"
echo "PASS: purge launcher refuses anything but one idle factory context and propagates failures"
