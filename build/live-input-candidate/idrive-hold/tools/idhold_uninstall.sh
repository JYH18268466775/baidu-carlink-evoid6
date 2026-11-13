#!/bin/sh
# Removes what idhold_install.sh installed. Usage: sh idhold_uninstall.sh --yes
plug=${IDH_PLUGIN_DIR:-/opt/hmi/ID5/data/ro/Plugins/NbtAad}
aadir=${IDH_AA_DIR:-/opt/aa}
state=${IDH_STATE_FILE:-/dev/shmem/nbt-aad-state}
mnt=${IDH_MOUNT:-/net/hu-omap/fs/sda0}
files="libNbtAad.so NbtAad.manifest NbtAad.xml NbtAad_ALL_Images.zip NbtAad_ALL_Texts.zip"
if [ "$#" -ne 1 ] || [ "$1" != "--yes" ]; then
    echo "Usage: sh $0 --yes" >&2
    exit 2
fi
if [ -z "$IDH_TEST" ]; then
    mount -o remount rw "$mnt" || { echo "Cannot remount $mnt read-write" >&2; exit 5; }
fi
for f in $files; do
    rm -f "$plug/$f"
done
rmdir "$plug" 2>/dev/null
if [ -e "$plug" ]; then
    echo "$plug still contains other files; left in place: $(ls "$plug")" >&2
fi
rm -f "$aadir/nbt-aad"
echo "idle 0 0" > "$state" 2>/dev/null
echo "Removed. To unload it from iDrive: OnOffDSICommander appreset."
exit 0
