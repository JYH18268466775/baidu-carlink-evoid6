#!/bin/bash
# Display geometry of build/live-input-candidate/tools/carlife_visible_first_frame.sh (fit and stretch modes).
here=$(cd "$(dirname "$0")/../.." && pwd)
script=$here/build/live-input-candidate/tools/carlife_visible_first_frame.sh
body=$(awk '/^    vw=\$\{/{f=1} /set surface 1510 opacity/{f=0} f' "$script")
[ -n "$body" ] || { echo "FAIL: layout block not found" >&2; exit 1; }
run() { # width height fit
    printf 'L=echo\n%s\n' "$body" | env CARLINK_VIDEO_WIDTH="$1" CARLINK_VIDEO_HEIGHT="$2" ${3+CARLINK_DISPLAY_FIT="$3"} ${TEST_SHELL:-bash} -s 2>&1
    echo "status=$?"
}
check() { out=$(run "$@" ); case "$out" in *"$EXPECT"*) ;; *) echo "FAIL: $* expected '$EXPECT' got: $out" >&2; exit 1 ;; esac; }
EXPECT='destination region 213 0 853 480'; check 1280 720
EXPECT='destination region 213 0 853 480'; check 1280 720 fit
EXPECT='destination region 0 0 1280 480'; check 1280 480
EXPECT='destination region 0 0 1280 480'; check 1280 720 stretch
EXPECT='destination region 0 0 1280 480'; check 1920 1080 stretch
EXPECT='destination region 0 0 1280 480'; check 720 1280 stretch
EXPECT='Display fit (stretch)'; check 1280 720 stretch
EXPECT='status=76'; check 1280 720 zoom
EXPECT='status=76'; check 1280 720 ""
EXPECT='status=76'; check 0 720 stretch
# crop (no distortion) and crop:N (cut N picture rows top and bottom)
EXPECT='source region 0 80 1280 320'; check 1280 720 crop
EXPECT='destination region 0 0 1280 480'; check 1280 720 crop
EXPECT='source region 0 80 1280 320'; check 1920 1080 crop
EXPECT='source region 0 0 1280 480'; check 1280 480 crop
EXPECT='source region 0 40 1280 400'; check 1280 720 crop:60
EXPECT='source region 0 0 1280 480'; check 1280 720 crop:0
EXPECT='source region 0 120 1280 240'; check 1280 720 crop:180
EXPECT='Display fit (crop:60)'; check 1280 720 crop:60
EXPECT='status=76'; check 1280 720 crop:181
EXPECT='status=76'; check 1280 720 crop:
EXPECT='status=76'; check 1280 720 crop:6x
EXPECT='status=76'; check 1280 720 crop:-5
echo "PASS: display fit keeps the aspect ratio by default; stretch, crop and crop:N fill 1280x480; anything else is refused"
