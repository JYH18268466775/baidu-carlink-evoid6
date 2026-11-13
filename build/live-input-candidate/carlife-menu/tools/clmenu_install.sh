#!/bin/sh
# Installs the CarLife iDrive menu: the NBT-AAD iDrive app (personal use, PolyForm Noncommercial; menu texts changed to
# CarLife), the session supervisor as /opt/aa/nbt-aad, and the CarLife programs under /opt/aa/carlink.
# Usage: sh clmenu_install.sh --yes     Sources: /dev/shmem/clmenu-<name> (see upload-carlife-menu.cmd).
# Nothing restarts iDrive here: run `OnOffDSICommander appreset` yourself afterwards.
pfx=${CLM_PFX:-/dev/shmem/clmenu-}
plug=${CLM_PLUGIN_DIR:-/opt/hmi/ID5/data/ro/Plugins/NbtAad}
aadir=${CLM_AA_DIR:-/opt/aa}
mnt=${CLM_MOUNT:-/net/hu-omap/fs/sda0}
pfiles="libNbtAad.so NbtAad.manifest NbtAad.xml NbtAad_ALL_Images.zip NbtAad_ALL_Texts.zip"
cfiles="carlife_qnx_usb_capture.elf carlife_qnx_mmr_replay.elf carlife_qnx_most_watch.elf carlife_qnx_cp_purge.elf carlife_qnx_ctl.elf carlife_usb_visible.sh carlife_visible_first_frame.sh carlife_cp_purge.sh carlife_qnx_replay_guard.sh"
# True when the directory has real entries. Not `ls`: the head unit's ls also prints "." and "..".
has_entries() {
    for e in "$1"/* "$1"/.[!.]*; do [ -e "$e" ] && return 0; done
    return 1
}
if [ "$#" -ne 1 ] || [ "$1" != "--yes" ]; then
    echo "Usage: sh $0 --yes" >&2
    exit 2
fi
for f in $pfiles $cfiles nbt-aad; do
    [ -f "$pfx$f" ] || { echo "Missing $pfx$f; run upload-carlife-menu.cmd first" >&2; exit 3; }
done
if [ -e "$plug" ] && has_entries "$plug"; then
    echo "$plug is not empty; run clmenu_uninstall.sh (or idhold_uninstall.sh) first" >&2
    exit 4
fi
if [ -e "$aadir/nbt-aad" ] || [ -e "$aadir/carlink" ]; then
    echo "$aadir/nbt-aad or $aadir/carlink already exists; run clmenu_uninstall.sh first" >&2
    exit 4
fi
if [ -z "$CLM_TEST" ]; then
    mount -o remount rw "$mnt" || { echo "Cannot remount $mnt read-write" >&2; exit 5; }
fi
mkdir -p "$plug" "$aadir/carlink" || { echo "Cannot create install directories" >&2; exit 5; }
for f in $pfiles; do cp "$pfx$f" "$plug/$f" || { echo "Copy failed: $f" >&2; exit 6; }; done
for f in $cfiles; do cp "$pfx$f" "$aadir/carlink/$f" || { echo "Copy failed: $f" >&2; exit 6; }; done
cp "${pfx}nbt-aad" "$aadir/nbt-aad" || { echo "Copy failed: nbt-aad" >&2; exit 6; }
chmod +x "$aadir/nbt-aad" "$aadir"/carlink/*
check() { # SRC DST
    [ "$(wc -c < "$1")" = "$(wc -c < "$2")" ] || { echo "Size mismatch after copy: $2; run clmenu_uninstall.sh" >&2; exit 7; }
}
for f in $pfiles; do check "$pfx$f" "$plug/$f"; done
for f in $cfiles; do check "$pfx$f" "$aadir/carlink/$f"; done
check "${pfx}nbt-aad" "$aadir/nbt-aad"
echo "Installed into $plug, $aadir/nbt-aad and $aadir/carlink."
echo "Not active yet. To load it: OnOffDSICommander appreset (iDrive restarts for about a minute)."
echo "To remove it again: sh clmenu_uninstall.sh --yes, then appreset."
exit 0
