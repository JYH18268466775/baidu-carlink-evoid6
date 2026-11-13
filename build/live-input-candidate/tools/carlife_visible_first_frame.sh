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
# CARLINK_DISPLAY_SHOWN: a marker file that exists while the CarLife picture is on the panel.
mark_shown() { [ -z "${CARLINK_DISPLAY_SHOWN-}" ] || : > "$CARLINK_DISPLAY_SHOWN"; }
mark_hidden() { [ -z "${CARLINK_DISPLAY_SHOWN-}" ] || rm -f "$CARLINK_DISPLAY_SHOWN"; }
restore() {
    # The picture stays on the panel after the layer is given back to iDrive (LayerManager reports surface 1510 on no layer,
    # yet it is still drawn): hiding the surface itself is what removes it.
    $L set surface 1510 visibility 0
    $L set layer 0x80000000 render order 0x100
    $L set surface 256 visibility 1
    mark_hidden
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
    # CARLINK_SESSION_SECONDS: 30 (default), N seconds, or 0 = until the USB session ends,
    # CARLINK_SESSION_STOP appears, or this script is killed.
    session=${CARLINK_SESSION_SECONDS-30}
    case "$session" in ''|*[!0-9]*) exit 76 ;; esac
    [ "$session" -le 86400 ] || exit 76
    [ "$1" = "--show-live" ] || session=30
    parent=$$
    (
        timer=
        trap '[ -z "$timer" ] || kill "$timer" 2>/dev/null; exit 0' 1 2 15
        if [ "$session" -eq 0 ]; then
            # Dead-man: restore iDrive as soon as the display script is gone.
            while kill -0 "$parent" 2>/dev/null; do
                sleep 2 &
                timer=$!
                wait "$timer"
            done
            restore
        else
            if [ "$1" = "--show-live" ]; then sleep $((session + 15)) & else sleep 20 & fi
            timer=$!
            wait "$timer" && restore
        fi
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
    # Use the negotiated stream dimensions, never the requested dimensions.
    vw=${CARLINK_VIDEO_WIDTH-1280}
    vh=${CARLINK_VIDEO_HEIGHT-720}
    case "$vw:$vh" in *[!0-9:]*|:*|*:) exit 76 ;; esac
    [ "$vw" -gt 0 ] && [ "$vw" -le 65535 ] && [ "$vh" -gt 0 ] && [ "$vh" -le 65535 ] || exit 76
    if [ $((vw * 480)) -ge $((vh * 1280)) ]; then
        dw=1280
        dh=$(((1280 * vh + vw / 2) / vw))
    else
        dh=480
        dw=$(((480 * vw + vh / 2) / vh))
    fi
    [ "$dw" -gt 0 ] && [ "$dh" -gt 0 ] || exit 76
    dx=$(((1280 - dw) / 2))
    dy=$(((480 - dh) / 2))
    # CARLINK_DISPLAY_FIT=stretch fills the whole 1280x480 panel (the picture is squeezed vertically); default fit keeps
    # the aspect ratio. The surface is already scaled to 1280x480 by the renderer, so stretch only widens the destination.
    # crop shows the middle of the picture at the panel's full width with no distortion (the top and bottom rows that do
    # not fit are cut); crop:N cuts N picture rows at the top and at the bottom and squeezes what is left to the panel
    # height (N=60 on 720p: 600 rows into 480, 20% vertical squeeze).
    fitmode=${CARLINK_DISPLAY_FIT-fit}
    sy=0
    case "$fitmode" in
        fit) ;;
        stretch) dx=0; dy=0; dw=1280; dh=480 ;;
        crop|crop:*)
            if [ "$fitmode" = crop ]; then
                shown=$((480 * vw / 1280))
                if [ "$shown" -lt "$vh" ]; then cut=$(((vh - shown) / 2)); else cut=0; fi
            else
                cut=${fitmode#crop:}
                case "$cut" in ''|*[!0-9]*) exit 76 ;; esac
                [ "$cut" -le $((vh / 4)) ] || exit 76
            fi
            dx=0; dy=0; dw=1280; dh=480
            sy=$((cut * 480 / vh))
            ;;
        *) exit 76 ;;
    esac
    [ "$sy" -lt 240 ] || exit 76
    srch=$((480 - 2 * sy))
    echo "Display fit ($fitmode): phone ${vw}x${vh}, source rows $sy+$srch, destination $dx,$dy ${dw}x${dh}"
    $L set surface 1510 source region 0 "$sy" 1280 "$srch" || exit 76
    $L set surface 1510 destination region "$dx" "$dy" "$dw" "$dh" || exit 76
    $L set surface 1510 opacity 1.0 || exit 76
    $L set surface 1510 visibility 1 || exit 76
    if usb_ended "$1"; then
        echo "USB capture stopped before display switch."
        exit 76
    fi
    shown=1
    if [ "$1" = "--show-live" ] && [ -n "${CARLINK_DISPLAY_HIDE-}" ] && [ -e "$CARLINK_DISPLAY_HIDE" ]; then
        shown=0  # the car has the panel right now (reverse gear, park assist): start hidden
        $L set surface 1510 visibility 0
    else
        $L set layer 0x80000000 render order 1510 || exit 76
        mark_shown
    fi
    echo "VISIBLE TEST: observe the CarLife display."
    echo "A successful command does not prove physical display."
    if [ "$1" = "--show-live" ]; then
        seconds=0
        while [ "$session" -eq 0 ] || [ "$seconds" -lt "$session" ]; do
            if usb_ended "$1"; then
                echo "USB disconnected or capture stopped; restoring iDrive now."
                break
            fi
            if [ -n "${CARLINK_SESSION_STOP-}" ] && [ -e "$CARLINK_SESSION_STOP" ]; then
                echo "Stop requested; restoring iDrive now."
                break
            fi
            # CARLINK_DISPLAY_HIDE exists: give the panel back (reverse gear, PDC, popups) until it is removed.
            if [ -n "${CARLINK_DISPLAY_HIDE-}" ] && [ -e "$CARLINK_DISPLAY_HIDE" ]; then
                if [ "$shown" -eq 1 ]; then
                    # the supervisor (nbt-aad) gives the panel back itself and removes the shown file first: nothing to repeat
                    if [ -n "${CARLINK_DISPLAY_SHOWN-}" ] && [ ! -e "$CARLINK_DISPLAY_SHOWN" ]; then :; else restore; fi
                    shown=0
                fi
            elif [ "$shown" -eq 0 ]; then
                $L set surface 1510 visibility 1
                $L set layer 0x80000000 render order 1510 || break
                mark_shown
                shown=1
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
    if [ "$1" = "--show-live" ] && ! usb_ended "$1"; then
        if [ -n "${CARLINK_DISPLAY_DONE-}" ]; then
            : > "$CARLINK_DISPLAY_DONE" || exit 79
            echo "Display session completed; stopping this USB session."
        fi
    fi
    exit 0
fi
if [ "$#" -ne 2 ] || [ ! -x "$1" ] || [ ! -f "$2" ]; then
    echo "Usage: sh $0 REPLAY_ELF SAVED_SINGLE_FRAME_MDEV"
    exit 2
fi
exec "$1" --visible-file "$2"
