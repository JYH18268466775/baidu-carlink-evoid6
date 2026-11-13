#!/bin/sh
# Removes what clmenu_install.sh installed. Usage: sh clmenu_uninstall.sh --yes
plug=${CLM_PLUGIN_DIR:-/opt/hmi/ID5/data/ro/Plugins/NbtAad}
aadir=${CLM_AA_DIR:-/opt/aa}
state=${CLM_STATE_FILE:-/dev/shmem/nbt-aad-state}
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
if [ -z "$CLM_TEST" ]; then
    mount -o remount rw "$mnt" || { echo "Cannot remount $mnt read-write" >&2; exit 5; }
fi
for f in $pfiles; do rm -f "$plug/$f"; done
rmdir "$plug" 2>/dev/null
if [ -e "$plug" ] && has_entries "$plug"; then echo "$plug still contains other files; left in place" >&2; fi
for f in $cfiles; do rm -f "$aadir/carlink/$f"; done
rmdir "$aadir/carlink" 2>/dev/null
if [ -e "$aadir/carlink" ] && has_entries "$aadir/carlink"; then echo "$aadir/carlink still contains other files; left in place" >&2; fi
rm -f "$aadir/nbt-aad"
echo "idle 0 0" > "$state" 2>/dev/null
echo "Removed. To unload it from iDrive: OnOffDSICommander appreset."
exit 0
