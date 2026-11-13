#!/bin/sh
# Use on the parked bench after ending any AA/CarPlay session.
set -u

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ] || [ ! -x "$1" ]; then
    echo "Usage: sh $0 USB_CAPTURE_ELF [MMR_REPLAY_ELF | --diagnose-usb]" >&2
    exit 2
fi
probe=$1
replay=${2-}
if [ -n "$replay" ] && [ "$replay" != "--diagnose-usb" ]; then
    [ -x "$replay" ] || exit 2
    sh /dev/shmem/carlife_visible_first_frame.sh --check || exit $?
fi

# Wait for one matching device node.
wait_node() {
    prefix=$1
    seconds=$2
    while [ "$seconds" -gt 0 ]; do
        found=
        for candidate in /dev/"$prefix"[0-9]*; do
            [ -e "$candidate" ] || continue
            suffix=${candidate#/dev/"$prefix"}
            case "$suffix" in *[!0-9]*) continue ;; esac
            if [ -n "$found" ]; then
                echo "Multiple $prefix USB nodes; test deferred" >&2
                return 1
            fi
            found=$candidate
        done
        if [ -n "$found" ]; then
            echo "$found"
            return $?
        fi
        sleep 1
        seconds=$((seconds - 1))
    done
    echo "Timed out waiting for /dev/$prefix<digits>" >&2
    return 1
}

for node in /dev/android[0-9]* /dev/oap[0-9]*; do
    if [ -e "$node" ]; then
        echo "Found $node before plug-in. Disconnect USB phones first." >&2
        exit 3
    fi
done

echo "Plug in the unlocked test phone now and accept the CarLife accessory prompt if shown." >&2
android=$(wait_node android 20) || exit 5
[ -n "$android" ] || exit 5
echo "Switching $android to Baidu CarLife accessory mode." >&2
"$probe" --switch "$android" || exit 6

oap=$(wait_node oap 10) || exit 7
[ -n "$oap" ] || exit 7
if [ "$replay" = "--diagnose-usb" ]; then
    echo "Checking CarLife USB on $oap (300 frames, 45-second limit)." >&2
    "$probe" --stream-visible "$oap" > /dev/null
    exit $?
fi
if [ -n "$replay" ]; then
    echo "Starting live CarLife video on $oap (300 frames, 45-second USB limit)." >&2
    "$probe" --stream-visible "$oap" | "$replay" --visible-stream
    exit $?
fi
umask 077
output=/dev/shmem/carlife-first-idr.$$.h264
# /dev/shmem on the head unit accepts files but cannot create subdirectories.
if ! (set -C; : > "$output"); then
    echo "Cannot create capture file $output" >&2
    exit 4
fi
echo "Starting CarLife handshake on $oap (45-second limit)." >&2
if "$probe" --capture "$oap" > "$output"; then
    echo "Capture saved to $output"
else
    status=$?
    rm -f "$output"
    echo "CarLife capture ended with status $status. Save the stage messages above." >&2
    exit "$status"
fi
