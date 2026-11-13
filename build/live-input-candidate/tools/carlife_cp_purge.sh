#!/bin/sh
# Destroys the factory CarPlay video context so CarLife can take the decoder.
# Usage: sh carlife_cp_purge.sh ./carlife_qnx_cp_purge.elf --yes
# Afterwards the factory CarPlay stays unavailable until the head unit restarts.
root=${CARLINK_CONTEXT_ROOT:-/pps/services/multimedia/renderer/context}
dev=${CARLINK_DEV_ROOT:-/dev}
if [ "$#" -ne 2 ] || [ ! -x "$1" ] || [ "$2" != "--yes" ]; then
    echo "Usage: sh $0 PURGE_ELF --yes" >&2
    exit 2
fi
if ! ls "$root" >/dev/null 2>&1; then
    echo "Cannot inspect renderer contexts" >&2
    exit 74
fi
set -- "$1" $(ls "$root")
elf=$1
shift
if [ "$#" -eq 0 ]; then
    echo "No renderer context exists; nothing to destroy."
    exit 0
fi
if [ "$#" -ne 1 ]; then
    echo "Expected exactly one context, found: $*" >&2
    exit 3
fi
name=$1
case "$name" in
    cpvideoctx*[!0-9]*|cpvideoctx) echo "Not a factory CarPlay context: $name" >&2; exit 3 ;;
    cpvideoctx*) ;;
    *) echo "Not a factory CarPlay context: $name" >&2; exit 3 ;;
esac
for node in "$dev"/oap* "$dev"/android*; do
    if [ -e "$node" ]; then
        echo "A phone USB node exists ($node); unplug the phone first" >&2
        exit 4
    fi
done
echo "Destroying $name. Factory CarPlay stays unavailable until the head unit restarts."
"$elf" --destroy "$name"
rc=$?
echo "purge exit=$rc"
echo "Remaining contexts:"
ls "$root"
exit "$rc"
