#!/bin/bash
# Offline checks for the iDrive hold package. Usage: bash tests/idhold-test.sh
here=$(cd "$(dirname "$0")/.." && pwd)
t=$(mktemp -d)
trap 'rm -rf "$t"' EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }
export IDH_TEST=1 IDH_PFX="$t/src/idhold-" IDH_PLUGIN_DIR="$t/hmi/Plugins/NbtAad" IDH_AA_DIR="$t/aa" IDH_STATE_FILE="$t/state" IDH_SHIM_LOG="$t/shim.log"
mkdir -p "$t/src"
for f in "$here"/package/*; do cp "$f" "$t/src/idhold-$(basename "$f")"; done
for sh in sh bash; do
    rm -rf "$t/hmi" "$t/aa"
    $sh "$here/tools/idhold_install.sh" >/dev/null 2>&1 && fail "install without --yes ($sh)"
    $sh "$here/tools/idhold_install.sh" --bogus >/dev/null 2>&1 && fail "install bad arg ($sh)"
    mv "$t/src/idhold-NbtAad.xml" "$t/xml.bak"
    $sh "$here/tools/idhold_install.sh" --yes >/dev/null 2>&1; [ $? -eq 3 ] || fail "missing source not rejected ($sh)"
    [ ! -e "$t/aa/nbt-aad" ] || fail "partial install after missing source ($sh)"
    mv "$t/xml.bak" "$t/src/idhold-NbtAad.xml"
    $sh "$here/tools/idhold_install.sh" --yes >/dev/null 2>&1 || fail "install ($sh)"
    for f in libNbtAad.so NbtAad.manifest NbtAad.xml NbtAad_ALL_Images.zip NbtAad_ALL_Texts.zip; do
        cmp "$here/package/$f" "$IDH_PLUGIN_DIR/$f" || fail "copy differs $f ($sh)"
    done
    [ -x "$IDH_AA_DIR/nbt-aad" ] || fail "shim not executable ($sh)"
    $sh "$here/tools/idhold_install.sh" --yes >/dev/null 2>&1; [ $? -eq 4 ] || fail "second install not refused ($sh)"
    echo "extra" > "$IDH_PLUGIN_DIR/other.txt"
    $sh "$here/tools/idhold_uninstall.sh" --yes >/dev/null 2>&1 || fail "uninstall ($sh)"
    [ -f "$IDH_PLUGIN_DIR/other.txt" ] || fail "uninstall deleted a foreign file ($sh)"
    [ ! -e "$IDH_AA_DIR/nbt-aad" ] || fail "shim left behind ($sh)"
    [ ! -e "$IDH_PLUGIN_DIR/libNbtAad.so" ] || fail "plugin left behind ($sh)"
    [ "$(cat "$t/state")" = "idle 0 0" ] || fail "state not idle after uninstall ($sh)"
    rm -f "$IDH_PLUGIN_DIR/other.txt"
    $sh "$here/tools/idhold_uninstall.sh" --yes >/dev/null 2>&1 || fail "second uninstall ($sh)"
    $sh "$here/tools/idhold_uninstall.sh" >/dev/null 2>&1 && fail "uninstall without --yes ($sh)"
done
# the stand-in daemon
for sh in sh bash; do
    rm -f "$t/state" "$t/shim.log"
    $sh "$here/package/nbt-aad" &
    pid=$!
    for i in 1 2 3 4 5 6 7 8 9 10; do [ -s "$t/state" ] && break; sleep 0.1; done
    [ "$(cat "$t/state")" = "connected $pid 1" ] || fail "state while running: '$(cat "$t/state")' vs $pid ($sh)"
    kill -0 "$pid" || fail "shim died ($sh)"
    kill -TERM "$pid"
    wait "$pid"; rc=$?
    [ "$rc" -eq 0 ] || fail "shim exit code $rc ($sh)"
    [ "$(cat "$t/state")" = "idle 0 0" ] || fail "state after TERM: '$(cat "$t/state")' ($sh)"
    grep -q "stopped" "$t/shim.log" || fail "no stop log ($sh)"
done
# the upstream app reads "%23s %d %d": check the format we write parses that way
read -r a b c < <(echo "connected 123 1")
[ "$a" = connected ] && [ "$b" = 123 ] && [ "$c" = 1 ] || fail "state line format"
echo "IDHOLD OFFLINE CHECKS PASSED"
