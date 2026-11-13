#!/bin/bash
# Session loop of carlife_visible_first_frame.sh --show-live against a mock LayerManagerControl.
here=$(cd "$(dirname "$0")/../.." && pwd)
script=$here/build/live-input-candidate/tools/carlife_visible_first_frame.sh
work=$(mktemp -d); trap 'kill $(jobs -p) 2>/dev/null; rm -rf "$work"' EXIT
cat > "$work/lmc" <<'MOCK'
#!/bin/sh
echo "$*" >> "$LMC_LOG"
case "$*" in
  "get layer 0x80000000") echo "layer 2147483648 (0x80000000)"; echo "  surface render order: 256(0x100), " ;;
  "get surface 256") echo "surface 256 (0x100)"; echo "  visibility: 1 " ;;
  "get surface 1510") echo "  native surface: 7" ;;
esac
exit 0
MOCK
chmod +x "$work/lmc"
sed "s|^L=.*|L=$work/lmc|" "$script" > "$work/show.sh"
SHELLS=(bash); command -v dash >/dev/null && SHELLS+=(dash)
bad=0
fail() { echo "FAIL: $1"; bad=1; }
orders() { grep -c "render order $1" "$work/log" ; }
for sh in "${SHELLS[@]}"; do
  base=(env CARLINK_DISPLAY_SHOWN="$work/shown" LMC_LOG="$work/log" CARLINK_DISPLAY_DONE="$work/done" CARLINK_SESSION_STOP="$work/stop" CARLINK_DISPLAY_HIDE="$work/hide" CARLINK_VIDEO_WIDTH=1280 CARLINK_VIDEO_HEIGHT=720)
  reset() { rm -f "$work/log" "$work/done" "$work/stop" "$work/hide" "$work/shown"; : > "$work/log"; }

  reset; "${base[@]}" CARLINK_SESSION_SECONDS=2 $sh "$work/show.sh" --show-live >"$work/out" 2>&1; rc=$?
  [ $rc -eq 0 ] && [ -e "$work/done" ] && [ "$(orders 1510)" = 1 ] && [ "$(orders 0x100)" -ge 1 ] || fail "$sh: 2-second session ends, restores iDrive and signals done (rc=$rc)"
  [ $rc -eq 0 ] && echo "PASS: $sh: bounded session"

  reset; ( sleep 3; : > "$work/stop" ) & 
  "${base[@]}" CARLINK_SESSION_SECONDS=0 $sh "$work/show.sh" --show-live >"$work/out" 2>&1; rc=$?
  [ $rc -eq 0 ] && [ -e "$work/done" ] && grep -q "Stop requested" "$work/out" && [ "$(grep -o "render order [0-9x]*" "$work/log" | tail -n 1)" = "render order 0x100" ] && echo "PASS: $sh: continuous session runs until the stop file appears" || fail "$sh: continuous session stops on the stop file (rc=$rc)"
  wait

  reset; ( sleep 2; : > "$work/hide"; sleep 3; rm -f "$work/hide"; sleep 3; : > "$work/stop" ) &
  ( sleep 1; [ -e "$work/shown" ] && echo yes > "$work/mark1"; sleep 2.5; [ -e "$work/shown" ] || echo yes > "$work/mark2" ) &
  "${base[@]}" CARLINK_SESSION_SECONDS=0 $sh "$work/show.sh" --show-live >"$work/out" 2>&1; rc=$?
  seq=$(grep -o "render order [0-9x]*" "$work/log" | tr '\n' ' ')
  case "$seq" in "render order 1510 render order 0x100 render order 1510 render order 0x100 ") echo "PASS: $sh: hide file hands the panel back, removal shows CarLife again" ;;
    *) fail "$sh: hide/show order was: $seq" ;; esac
  wait
  [ -e "$work/mark1" ] && [ -e "$work/mark2" ] && [ ! -e "$work/shown" ] && echo "PASS: $sh: shown marker follows the layer (present while shown, gone while hidden and after the end)" || fail "$sh: shown marker (mark1=$([ -e "$work/mark1" ] && echo y), mark2=$([ -e "$work/mark2" ] && echo y))"
  rm -f "$work/mark1" "$work/mark2"

  # started while the car owns the panel: the picture stays down until the hide file goes
  reset; : > "$work/hide"; ( sleep 2.5; rm -f "$work/hide"; sleep 2; : > "$work/stop" ) &
  "${base[@]}" CARLINK_SESSION_SECONDS=0 $sh "$work/show.sh" --show-live >"$work/out" 2>&1; rc=$?
  seq=$(grep -o "render order [0-9x]*" "$work/log" | tr '\n' ' ')
  [ "$seq" = "render order 1510 render order 0x100 " ] && echo "PASS: $sh: a session started during reverse stays hidden until the hide file is removed" || fail "$sh: start-hidden order was: $seq"
  wait

  reset
  "${base[@]}" CARLINK_SESSION_SECONDS=0 $sh "$work/show.sh" --show-live >"$work/out" 2>&1 &
  pid=$!; sleep 3; kill -9 $pid 2>/dev/null; before=$(orders 0x100); sleep 5
  [ "$(orders 0x100)" -gt "$before" ] && echo "PASS: $sh: dead-man restores iDrive within seconds of the script being killed" || fail "$sh: no restore after kill -9"
  wait $pid 2>/dev/null

  reset; "${base[@]}" CARLINK_SESSION_SECONDS=abc $sh "$work/show.sh" --show-live >"$work/out" 2>&1; rc=$?
  [ $rc -eq 76 ] && echo "PASS: $sh: bad session value refused" || fail "$sh: bad session value rc=$rc"
  reset; "${base[@]}" CARLINK_SESSION_SECONDS=0 $sh "$work/show.sh" --show 2>&1 | head -0
done
[ $bad -eq 0 ] && echo "display session tests ok"
exit $bad
