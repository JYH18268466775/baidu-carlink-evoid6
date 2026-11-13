#!/bin/bash
# Offline test of clmenu_install.sh / clmenu_uninstall.sh with temp directories.
here=$(cd "$(dirname "$0")/.." && pwd)
t=$(mktemp -d); trap 'rm -rf "$t"' EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }
export CLM_TEST=1 CLM_PFX="$t/src/clmenu-" CLM_PLUGIN_DIR="$t/hmi/NbtAad" CLM_AA_DIR="$t/aa" CLM_STATE_FILE="$t/state"
mkdir -p "$t/src"
names="libNbtAad.so NbtAad.manifest NbtAad.xml NbtAad_ALL_Images.zip NbtAad_ALL_Texts.zip nbt-aad carlife_qnx_usb_capture.elf carlife_qnx_mmr_replay.elf carlife_qnx_most_watch.elf carlife_qnx_cp_purge.elf carlife_qnx_ctl.elf carlife_usb_visible.sh carlife_visible_first_frame.sh carlife_cp_purge.sh carlife_qnx_replay_guard.sh"
for n in $names; do echo "data $n" > "$t/src/clmenu-$n"; done
# an empty plugin directory (the head unit shows "." and ".." for it) must not count as non-empty
mkdir -p "$t/hmi/NbtAad"; mkdir -p "$t/fakebin"
printf '#!/bin/sh\necho .; echo ..; /bin/ls "$@"\n' > "$t/fakebin/ls"; chmod +x "$t/fakebin/ls"
PATH="$t/fakebin:$PATH" sh "$here/tools/clmenu_install.sh" --yes >/dev/null 2>&1 || fail "empty plugin dir refused (ls prints dot entries)"
PATH="$t/fakebin:$PATH" sh "$here/tools/clmenu_uninstall.sh" --yes >/dev/null 2>&1
for sh in sh bash; do
    rm -rf "$t/hmi" "$t/aa"
    $sh "$here/tools/clmenu_install.sh" >/dev/null 2>&1 && fail "no --yes ($sh)"
    mv "$t/src/clmenu-carlife_qnx_ctl.elf" "$t/ctl.bak"
    $sh "$here/tools/clmenu_install.sh" --yes >/dev/null 2>&1; [ $? -eq 3 ] || fail "missing source ($sh)"
    [ ! -e "$t/aa" ] || fail "partial install ($sh)"
    mv "$t/ctl.bak" "$t/src/clmenu-carlife_qnx_ctl.elf"
    $sh "$here/tools/clmenu_install.sh" --yes >/dev/null 2>&1 || fail "install ($sh)"
    [ -x "$t/aa/nbt-aad" ] && [ -x "$t/aa/carlink/carlife_qnx_ctl.elf" ] && [ -f "$t/hmi/NbtAad/NbtAad.xml" ] || fail "files missing ($sh)"
    cmp "$t/src/clmenu-nbt-aad" "$t/aa/nbt-aad" || fail "supervisor differs ($sh)"
    $sh "$here/tools/clmenu_install.sh" --yes >/dev/null 2>&1; [ $? -eq 4 ] || fail "second install not refused ($sh)"
    echo x > "$t/hmi/NbtAad/other.txt"
    $sh "$here/tools/clmenu_uninstall.sh" --yes >/dev/null 2>&1 || fail "uninstall ($sh)"
    [ -f "$t/hmi/NbtAad/other.txt" ] && [ ! -e "$t/aa/nbt-aad" ] && [ ! -e "$t/aa/carlink" ] && [ "$(cat "$t/state")" = "idle 0 0" ] || fail "uninstall result ($sh)"
    $sh "$here/tools/clmenu_uninstall.sh" >/dev/null 2>&1 && fail "uninstall without --yes ($sh)"
done
echo "clmenu install tests ok"
