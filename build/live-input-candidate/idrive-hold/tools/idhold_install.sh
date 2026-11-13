#!/bin/sh
# Installs the NBT-AAD iDrive app (personal use, PolyForm Noncommercial) plus a stand-in daemon.
# Usage: sh idhold_install.sh --yes
# Source files are expected as /dev/shmem/idhold-<name> (see upload-idrive-hold.cmd).
# Nothing restarts iDrive here: run `OnOffDSICommander appreset` yourself afterwards.
pfx=${IDH_PFX:-/dev/shmem/idhold-}
plug=${IDH_PLUGIN_DIR:-/opt/hmi/ID5/data/ro/Plugins/NbtAad}
aadir=${IDH_AA_DIR:-/opt/aa}
mnt=${IDH_MOUNT:-/net/hu-omap/fs/sda0}
files="libNbtAad.so NbtAad.manifest NbtAad.xml NbtAad_ALL_Images.zip NbtAad_ALL_Texts.zip"
if [ "$#" -ne 1 ] || [ "$1" != "--yes" ]; then
    echo "Usage: sh $0 --yes" >&2
    exit 2
fi
for f in $files nbt-aad; do
    if [ ! -f "$pfx$f" ]; then
        echo "Missing $pfx$f; run upload-idrive-hold.cmd first" >&2
        exit 3
    fi
done
if [ -e "$plug" ] && [ -n "$(ls "$plug" 2>/dev/null)" ]; then
    echo "$plug is not empty; run idhold_uninstall.sh first" >&2
    exit 4
fi
if [ -e "$aadir/nbt-aad" ]; then
    echo "$aadir/nbt-aad already exists; run idhold_uninstall.sh first" >&2
    exit 4
fi
if [ -z "$IDH_TEST" ]; then
    mount -o remount rw "$mnt" || { echo "Cannot remount $mnt read-write" >&2; exit 5; }
fi
mkdir -p "$plug" "$aadir" || { echo "Cannot create install directories" >&2; exit 5; }
for f in $files; do
    cp "$pfx$f" "$plug/$f" || { echo "Copy failed: $f" >&2; exit 6; }
done
cp "${pfx}nbt-aad" "$aadir/nbt-aad" || { echo "Copy failed: nbt-aad" >&2; exit 6; }
chmod +x "$aadir/nbt-aad"
for f in $files; do
    a=$(wc -c < "$pfx$f")
    b=$(wc -c < "$plug/$f")
    if [ "$a" != "$b" ]; then
        echo "Size mismatch after copy: $f ($a vs $b); run idhold_uninstall.sh" >&2
        exit 7
    fi
done
a=$(wc -c < "${pfx}nbt-aad")
b=$(wc -c < "$aadir/nbt-aad")
if [ "$a" != "$b" ]; then
    echo "Size mismatch after copy: nbt-aad; run idhold_uninstall.sh" >&2
    exit 7
fi
echo "Installed into $plug and $aadir."
ls -l "$plug" "$aadir"
echo "Not active yet. To load it: OnOffDSICommander appreset (iDrive restarts for about a minute)."
echo "To remove it again: sh idhold_uninstall.sh --yes, then appreset."
exit 0
