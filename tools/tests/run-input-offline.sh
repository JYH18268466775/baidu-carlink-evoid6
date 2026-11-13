#!/usr/bin/env bash
# Every offline check for build/live-input-candidate. Linux/WSL with gcc, clang, node and dash/bash.
# Needs no head unit and no phone. Exit status 0 means all passed.
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
echo "### USB write serialization, input thread, mutation checks, ARM compile"
bash "$here/usb-write-serial.sh"
echo "### Launcher CARLINK_INPUT_FILE handling"
bash "$here/usb-visible-input.sh"
echo "### CarPlay-context purge tool"
bash "$here/cp-purge.sh"
echo "### Read-only MOST watch (set UPSTREAM_SRC and UPSTREAM_FIXTURES to add the upstream decoder oracle)"
bash "$here/most-watch.sh"
echo "### Build self-check script"
node "$here/check-build.cjs"
echo "### Display geometry (fit / stretch)"
bash "$here/display-fit.sh" || exit 1
echo "### USB receive resync (record whose AOA header was lost)"
bash "$here/usb-resync.sh"
echo "### Live spool ring and continuous session"
bash "$here/usb-ring.sh"
echo "### Display session loop (mock LayerManagerControl)"
bash "$here/display-session.sh"
echo "### UDP control port helper"
bash "$here/ctl.sh"
echo "### CarLife iDrive menu: installer and session supervisor (about 2 minutes)"
bash "$here/../../build/live-input-candidate/carlife-menu/tests/clmenu-install-test.sh"
bash "$here/../../build/live-input-candidate/carlife-menu/tests/carlife-menu-test.sh"
echo "### Existing encoders"
node "$here/idrive-keys.cjs"
node "$here/idrive-adapter.cjs"
node "$here/media-pcm.cjs"
echo "ALL OFFLINE CHECKS PASSED"
