#!/bin/sh
# Supervised stationary-car test. Uses a saved single frame and private ID 1510.
L=/opt/sys/bin/LayerManagerControl
baseline() {
    # BMW's query can print a valid layer followed by an unrelated
    # "on screen" lookup error. Validate the required fields explicitly.
    layer=$($L get layer 0x80000000 2>&1)
    echo "$layer" | grep -E '^layer 2147483648 \(0x80000000\)' >/dev/null || return 1
    echo "$layer" | grep -E 'surface render order: *256\(0x100\), *$' >/dev/null || return 1
    surface=$($L get surface 256 2>&1)
    echo "$surface" | grep -E '^surface 256 \(0x100\)' >/dev/null || return 1
    echo "$surface" | grep -E 'visibility: *1 *$' >/dev/null || return 1
}
restore() {
    $L set layer 0x80000000 render order 0x100
    $L set surface 256 visibility 1
}
usb_ended() {
    [ "$1" = "--show-live" ] || return 1
    { [ -n "${CARLINK_USB_NODE-}" ] && [ ! -e "$CARLINK_USB_NODE" ]; } ||
        { [ -n "${CARLINK_USB_STOPPED-}" ] && [ -e "$CARLINK_USB_STOPPED" ]; }
}
if [ "$#" -eq 2 ] && [ "$1" = "--cleanup" ]; then
    case "$2" in ''|*[!0-9]*|0) exit 2 ;; esac
    owner=$2
    for attempt in 1 2 3; do
        surface=$($L get surface 1510 2>&1)
        if echo "$surface" | grep -E '^No surface with ID 1510 found[[:space:]]*$' >/dev/null; then
            exit 0
        fi
        if ! echo "$surface" | grep -E "created by pid:[[:space:]]+$owner[[:space:]]*$" >/dev/null; then
            echo "Surface 1510 owner could not be verified; cleanup deferred" >&2
            exit 79
        fi
        if echo "$surface" | grep -E 'native surface:[[:space:]]+4294967295[[:space:]]*$' >/dev/null &&
           echo "$surface" | grep -E 'on layer:[[:space:]]*$' >/dev/null; then
            if ! baseline; then
                echo "iDrive baseline changed; surface cleanup deferred" >&2
                exit 79
            fi
            $L destroy surface 1510
            surface=$($L get surface 1510 2>&1)
            if echo "$surface" | grep -E '^No surface with ID 1510 found[[:space:]]*$' >/dev/null; then
                echo "Removed detached CarLife surface 1510."
                exit 0
            fi
        fi
        if [ "$attempt" -lt 3 ]; then sleep 1; fi
    done
    echo "CarLife surface 1510 is still present; cleanup deferred" >&2
    exit 79
fi
if [ "$#" -eq 1 ] && [ "$1" = "--check" ]; then
    sh /dev/shmem/carlife_qnx_replay_guard.sh --check || exit 75
    baseline || { echo "iDrive baseline changed; visible test deferred"; exit 75; }
    existing=$($L get surface 1510 2>&1)
    if echo "$existing" | grep 'native surface:' >/dev/null; then
        echo "Surface 1510 already exists; visible test deferred"
        exit 75
    fi
    exit 0
fi
if [ "$#" -eq 1 ] && { [ "$1" = "--show" ] || [ "$1" = "--show-live" ]; }; then
    echo "Checking iDrive baseline before visible switch"
    if ! baseline; then
        echo "iDrive baseline changed before visible switch; deferred"
        echo "$layer"
        echo "$surface"
        exit 75
    fi
    ready=0
    for attempt in 1 2 3 4 5; do
        if usb_ended "$1"; then
            echo "USB capture stopped before display switch."
            exit 76
        fi
        surface=$($L get surface 1510 2>&1)
        if echo "$surface" | grep 'native surface:' >/dev/null; then
            ready=1
            break
        fi
        sleep 1
    done
    [ "$ready" -eq 1 ] || { echo "Private surface did not appear"; echo "$surface"; exit 76; }
    # Wait on an asynchronous timer so a signal interrupts the watchdog's
    # wait instead of being deferred until a foreground sleep finishes.
    (
        timer=
        trap '[ -z "$timer" ] || kill "$timer" 2>/dev/null; exit 0' 1 2 15
        if [ "$1" = "--show-live" ]; then sleep 45 & else sleep 20 & fi
        timer=$!
        wait "$timer" && restore
    ) </dev/null >/dev/null 2>&1 &
    watchdog=$!
    restored=0
    finish_display() {
        if [ "$restored" -eq 0 ]; then restore; fi
        kill "$watchdog" 2>/dev/null
        wait "$watchdog" 2>/dev/null
    }
    trap 'finish_display' 0
    trap 'exit 77' 1 2 15
    $L set surface 1510 source region 0 0 1280 480 || exit 76
    $L set surface 1510 destination region 213 0 854 480 || exit 76
    $L set surface 1510 opacity 1.0 || exit 76
    $L set surface 1510 visibility 1 || exit 76
    if usb_ended "$1"; then
        echo "USB capture stopped before display switch."
        exit 76
    fi
    $L set layer 0x80000000 render order 1510 || exit 76
    echo "VISIBLE TEST: observe the CarLife display."
    echo "A successful command does not prove physical display."
    if [ "$1" = "--show-live" ]; then
        seconds=0
        while [ "$seconds" -lt 30 ]; do
            if usb_ended "$1"; then
                echo "USB disconnected or capture stopped; restoring iDrive now."
                break
            fi
            sleep 1
            seconds=$((seconds + 1))
        done
    else
        sleep 5
    fi
    restore
    baseline || { echo "iDrive restoration verification failed"; exit 79; }
    restored=1
    echo "iDrive layer order and visibility restored."
    if [ "$1" = "--show-live" ] && [ "$seconds" -eq 30 ] && ! usb_ended "$1"; then
        if [ -n "${CARLINK_DISPLAY_DONE-}" ]; then
            : > "$CARLINK_DISPLAY_DONE" || exit 79
            echo "30-second display completed; stopping this USB session."
        fi
    fi
    exit 0
fi
if [ "$#" -ne 2 ] || [ ! -x "$1" ] || [ ! -f "$2" ]; then
    echo "Usage: sh $0 REPLAY_ELF SAVED_SINGLE_FRAME_MDEV"
    exit 2
fi
exec "$1" --visible-file "$2"
