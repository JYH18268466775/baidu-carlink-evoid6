#!/bin/sh
# Run only on a supervised bench with projection clients disconnected.
if [ "$#" -ne 1 ] || [ "$1" != "--check" ]; then
    if [ "$#" -lt 1 ] || [ "$#" -gt 2 ] || [ ! -x "$1" ]; then
        echo "Usage: sh $0 PROBE [FIRST_FRAME_MDEV | --stream]" >&2
        exit 2
    fi
fi

root=/pps/services/multimedia/renderer/context
if ! ls "$root" >/dev/null; then
    echo "Cannot inspect renderer contexts; replay deferred" >&2
    exit 74
fi
for context in "$root"/*; do
    test -d "$context" || continue
    if ! test -f "$context/input"; then
        echo "Cannot verify renderer input for $context; replay deferred" >&2
        exit 74
    fi
    input=$(cat "$context/input") || exit 74
    case "$input" in
        */dev/name/local/mdev*)
            echo "mdev already attached by $context; replay deferred" >&2
            exit 75
            ;;
    esac
done
if [ "$1" = "--check" ]; then
    exit 0
fi
if [ "$#" -eq 2 ]; then
    if [ "$2" = "--stream" ]; then
        exec "$1" --replay-stream
    fi
    exec "$1" --replay-file "$2"
fi
exec "$1" --replay-once
