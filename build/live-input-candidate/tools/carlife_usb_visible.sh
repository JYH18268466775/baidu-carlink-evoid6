#!/bin/sh
# Use on the parked bench after ending any AA/CarPlay session.
set -u

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ] || [ ! -x "$1" ]; then
    echo "Usage: sh $0 USB_CAPTURE_ELF [MMR_REPLAY_ELF | --diagnose-usb]" >&2
    exit 2
fi
probe=$1
replay=${2-}
# Optional bench input for a live display run: CARLINK_INPUT_FILE=/dev/shmem/NAME.
# Append one action per line (echo OK >> FILE). Nothing is read before the
# first video frame, and the file is emptied at the start of each run.
input=${CARLINK_INPUT_FILE-}
if [ -n "$input" ]; then
    case "$input" in
        /dev/shmem/*) name=${input#/dev/shmem/} ;;
        *) name= ;;
    esac
    case "$name" in
        ''|.|..|*[!A-Za-z0-9._-]*)
            echo "CARLINK_INPUT_FILE must be /dev/shmem/NAME using A-Z a-z 0-9 . _ -" >&2
            exit 2 ;;
    esac
    if [ "${#name}" -gt 48 ]; then
        echo "CARLINK_INPUT_FILE name is longer than 48 characters" >&2
        exit 2
    fi
fi
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

# CARLINK_WIFI_PHONE=172.16.224.N: wireless CarLife. The receiver connects to the phone's CarLife ports on the head
# unit's hotspot instead of USB (live display only); press "Start connection" on the phone, it waits up to 30 s.
wifi=${CARLINK_WIFI_PHONE-}
if [ -n "$wifi" ]; then
    case "$wifi" in
        172.16.224.[0-9]|172.16.224.[0-9][0-9]|172.16.224.[0-9][0-9][0-9]) ;;
        *) echo "CARLINK_WIFI_PHONE must be 172.16.224.N" >&2; exit 2 ;;
    esac
    if [ -z "$replay" ] || [ "$replay" = "--diagnose-usb" ]; then
        echo "CARLINK_WIFI_PHONE needs the live display (MMR_REPLAY_ELF)" >&2
        exit 2
    fi
fi

# CARLINK_PLUGGED_OK=1 (the iDrive menu start): the phone may already be plugged in, and may already be in accessory
# mode from an earlier attempt, so a node found before the start is used instead of refused.
plugged=${CARLINK_PLUGGED_OK-}
[ -z "$wifi" ] || plugged=wifi
if [ "$plugged" != 1 ] && [ "$plugged" != wifi ]; then
    for node in /dev/android[0-9]* /dev/oap[0-9]*; do
        if [ -e "$node" ]; then
            echo "Found $node before plug-in. Disconnect USB phones first." >&2
            exit 3
        fi
    done
fi

have_oap=
if [ "$plugged" = 1 ]; then
    for node in /dev/oap[0-9]*; do [ -e "$node" ] && have_oap=1; done
fi
if [ -z "$have_oap" ] && [ -z "$wifi" ]; then
    echo "Plug in the unlocked test phone now and accept the CarLife accessory prompt if shown." >&2
    android=$(wait_node android 20) || exit 5
    [ -n "$android" ] || exit 5
    echo "Switching $android to Baidu CarLife accessory mode." >&2
    "$probe" --switch "$android" || exit 6
fi

if [ -n "$wifi" ]; then
    oap=$wifi
    source_flag=--stream-wifi
else
    oap=$(wait_node oap 10) || exit 7
    [ -n "$oap" ] || exit 7
    source_flag=--stream-visible
fi
if [ "$replay" = "--diagnose-usb" ]; then
    echo "Checking CarLife USB on $oap (900 frames, 60-second limit)." >&2
    [ -z "$input" ] || echo "CARLINK_INPUT_FILE is ignored by --diagnose-usb." >&2
    "$probe" --stream-visible "$oap" > /dev/null
    exit $?
fi
if [ -n "$replay" ]; then
    CARLINK_USB_NODE=$oap
    [ -z "$wifi" ] || CARLINK_USB_NODE=   # no device node to watch for unplugging
    CARLINK_USB_STOPPED=/dev/shmem/carlife-usb-$$.failed
    CARLINK_DISPLAY_DONE=${CARLINK_USB_STOPPED}.done
    if [ -e "$CARLINK_USB_STOPPED" ] || [ -e "$CARLINK_DISPLAY_DONE" ]; then
        echo "USB status file already exists; test deferred" >&2
        exit 4
    fi
    # Continuous session (CARLINK_SESSION_SECONDS=0): touch the stop file to end it; touch the hide file to give
    # the panel back (reverse gear, PDC) and remove it to show CarLife again.
    : "${CARLINK_SESSION_STOP:=/dev/shmem/carlink-stop}"
    : "${CARLINK_DISPLAY_HIDE:=/dev/shmem/carlink-hide}"
    # The hide file belongs to whoever wants the panel given back (the supervisor on reverse gear / park assist); a stale
    # stop file from an earlier run must not end this one.
    rm -f "$CARLINK_SESSION_STOP"
    export CARLINK_USB_NODE CARLINK_USB_STOPPED CARLINK_DISPLAY_DONE CARLINK_SESSION_STOP CARLINK_DISPLAY_HIDE
    trap 'rm -f "$CARLINK_USB_STOPPED" "$CARLINK_DISPLAY_DONE" "$CARLINK_SESSION_STOP"' 0
    if [ -n "$input" ]; then
        if ! : > "$input"; then
            echo "Cannot create input file $input" >&2
            exit 4
        fi
        echo "Key input: append actions to $input (OK BACK LEFT RIGHT UP DOWN +N -N)." >&2
    fi
    if [ "${CARLINK_SESSION_SECONDS-}" = 0 ]; then
        echo "Starting continuous CarLife on $oap. End it with: touch $CARLINK_SESSION_STOP" >&2
    else
        echo "Starting live CarLife video on $oap (900 frames, 60-second USB limit)." >&2
    fi
    (
        trap - 0
        if [ -n "$input" ]; then
            "$probe" "$source_flag" "$oap" --input-file "$input" &
        else
            "$probe" "$source_flag" "$oap" &
        fi
        capture_pid=$!
        (
            timer=
            trap '[ -z "$timer" ] || kill "$timer" 2>/dev/null; exit 0' 1 2 15
            while [ ! -e "$CARLINK_DISPLAY_DONE" ]; do
                sleep 1 &
                timer=$!
                wait "$timer"
            done
            kill -TERM "$capture_pid" 2>/dev/null
        ) </dev/null >/dev/null 2>&1 &
        monitor_pid=$!
        wait "$capture_pid"
        capture_status=$?
        kill "$monitor_pid" 2>/dev/null
        wait "$monitor_pid" 2>/dev/null
        if [ -e "$CARLINK_DISPLAY_DONE" ] && { [ "$capture_status" -eq 143 ] || [ "$capture_status" -eq 141 ]; }; then
            echo "USB capture stopped after completed display." >&2
            exit 0
        fi
        if [ "$capture_status" -ne 0 ]; then
            : > "$CARLINK_USB_STOPPED"
            echo "USB capture stopped with status $capture_status" >&2
            exit "$capture_status"
        fi
    ) | (
        trap - 0
        # Preserve the pipeline input for the asynchronous child.
        exec 3<&0
        "$replay" --visible-stream <&3 3<&- &
        replay_pid=$!
        exec 3<&-
        wait "$replay_pid"
        replay_status=$?
        sh /dev/shmem/carlife_visible_first_frame.sh --cleanup "$replay_pid"
        cleanup_status=$?
        if [ "$replay_status" -ne 0 ]; then
            exit "$replay_status"
        fi
        exit "$cleanup_status"
    )
    session_status=$?
    if [ "$session_status" -eq 0 ] && [ -e "$CARLINK_USB_STOPPED" ]; then
        session_status=65
    fi
    exit "$session_status"
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
