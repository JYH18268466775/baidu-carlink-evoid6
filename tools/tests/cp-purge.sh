#!/usr/bin/env bash
# Builds the real carlife_qnx_cp_purge.c against mocks, then tests its launcher script.
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
src=$here/../../build/live-input-candidate/src/carlife_qnx_cp_purge.c
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
gcc -w -Dcarlink_cp_purge=carlink_cp_purge -Ddlopen=mock_dlopen -Ddlsym=mock_dlsym -Ddlerror=mock_dlerror \
    -Ddlclose=mock_dlclose -Dusleep=mock_usleep -D__get_errno_ptr=mock_errno \
    -o "$tmp/host" "$here/cp-purge-host.c" "$src"
"$tmp/host" | tail -1
if command -v clang >/dev/null; then
    clang --target=armv7-none-eabi -marm -std=c11 -ffreestanding -fno-unwind-tables -fno-asynchronous-unwind-tables \
        -fno-stack-protector -Wall -Wextra -Werror -Os -c "$src" -o "$tmp/arm.o"
    echo "PASS: purge tool compiles for armv7 without warnings"
fi
bash "$here/cp-purge-script.sh"
sh "$here/cp-purge-script.sh"
