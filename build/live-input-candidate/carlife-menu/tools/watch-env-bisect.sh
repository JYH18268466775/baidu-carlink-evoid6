#!/bin/sh
# Read-only experiment: does the MOST knob watcher crash only in the environment the iDrive app gives it?
# Runs the watcher for 12 s each time (no --emit: nothing is written anywhere but the log, nothing is sent to the phone)
# in four environments, one after another, and records how each one ended.
#   A  this shell, as is (what worked over SSH)
#   B  empty environment + only LD_LIBRARY_PATH
#   C  the environment the supervisor saw (needs /dev/shmem/carlink-env-supervisor.txt from supervisor v6)
#   D  as A, but in the background with no terminal input (like a program started by the app)
# Use with the car parked, phone unplugged. Output: /dev/shmem/carlink-watch-bisect.log
SHM=${CL_SHM:-/dev/shmem}
ELF=${CL_WATCH_ELF:-$SHM/carlife_qnx_most_watch.elf}
ENVF=${CL_ENV_FILE:-$SHM/carlink-env-supervisor.txt}
OUT=${CL_BISECT_OUT:-$SHM/carlink-watch-bisect.log}
SECS=${CL_BISECT_SECONDS:-12}
PAUSE=${CL_BISECT_PAUSE:-3}
[ -x "$ELF" ] || { echo "no executable $ELF (copy carlife_qnx_most_watch.elf there and chmod +x)"; exit 1; }
: > "$OUT"
note() { echo "$*" >> "$OUT"; echo "$*"; }
finish() { note "=== $1: ended with status $2"; sleep "$PAUSE"; }

note "=== env bisect $(date +%H:%M:%S), $SECS s per run"
cd "$SHM" || exit 1

note "=== A: this shell"
"$ELF" --watch "$SECS" >> "$OUT" 2>&1 < /dev/null
finish A $?

note "=== B: empty environment + LD_LIBRARY_PATH"
if env -i true 2>/dev/null; then
    env -i LD_LIBRARY_PATH="${LD_LIBRARY_PATH-}" "$ELF" --watch "$SECS" >> "$OUT" 2>&1 < /dev/null
    finish B $?
else
    note "=== B: skipped, env -i is not available"
fi

note "=== C: environment recorded by the supervisor"
if [ -r "$ENVF" ] && env -i true 2>/dev/null; then
    set --
    inside=0
    while IFS= read -r line; do
        case $line in
            "--- env") inside=1; continue ;;
            "--- "*) inside=0; continue ;;
        esac
        [ "$inside" = 1 ] || continue
        case $line in "export "*) line=${line#export } ;; esac
        case $line in
            *=*)
                n=${line%%=*}; v=${line#*=}
                case $v in \'*\') v=${v#\'}; v=${v%\'} ;; esac
                set -- "$@" "$n=$v" ;;
        esac
    done < "$ENVF"
    note "=== C: $# variables replayed"
    env -i "$@" "$ELF" --watch "$SECS" >> "$OUT" 2>&1 < /dev/null
    finish C $?
else
    note "=== C: skipped (no $ENVF, or env -i missing)"
fi

note "=== D: in the background, no terminal"
( "$ELF" --watch "$SECS" >> "$OUT" 2>&1 < /dev/null ) &
wait $!
finish D $?

note "=== done $(date +%H:%M:%S): send back $OUT"
