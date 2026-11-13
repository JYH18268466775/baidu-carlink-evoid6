#!/bin/bash
# Offline test of package/nbt-aad (the CarLife session supervisor) with fake ctl, session, watcher and LayerManagerControl.
# Usage: bash tests/carlife-menu-test.sh   (needs bash and dash/sh, ps, sleep with fractions)
here=$(cd "$(dirname "$0")/.." && pwd)
t=$(mktemp -d); trap 'kill $(jobs -p) 2>/dev/null; pkill -f "$t/" 2>/dev/null; rm -rf "$t"' EXIT
bad=0
fail() { echo "FAIL: $*"; bad=1; }
ok() { echo "PASS: $*"; }
shm=$t/shm; dev=$t/dev; mkdir -p "$shm" "$dev" "$t/bin"

cat > "$t/bin/fakectl" <<'F'
#!/bin/sh
# prints "ctl: bound ..." then every line appended to $FAKE_CMDS; leaves when its parent changes
[ -n "$FAKE_CTL_FAIL" ] && { echo "ctl: bind failed"; exit 3; }
[ -n "$FAKE_CTL_DELAY" ] && sleep "$FAKE_CTL_DELAY"
[ -n "$FAKE_CTL_ORPHAN" ] && { echo $$ > "$FAKE_CTL_ORPHAN"; sleep 120; exit 0; }
if [ -n "$FAKE_CTL_LOCK" ] && [ -e "$FAKE_CTL_LOCK" ] && kill -0 "$(cat "$FAKE_CTL_LOCK")" 2>/dev/null; then echo "ctl: bind failed"; exit 3; fi
if [ -n "$FAKE_CTL_FAIL_ONCE" ] && [ ! -e "$FAKE_CTL_FAIL_ONCE" ]; then : > "$FAKE_CTL_FAIL_ONCE"; echo "ctl: bind failed"; exit 3; fi
echo "ctl: bound UDP 127.0.0.1:$2 (fake address layout)"
parent=$(ps -o ppid= -p $$ | tr -d ' ')
n=0
while :; do
    total=$(wc -l < "$FAKE_CMDS")
    while [ "$n" -lt "$total" ]; do n=$((n + 1)); sed -n "${n}p" "$FAKE_CMDS"; done
    [ "$(ps -o ppid= -p $$ | tr -d ' ')" = "$parent" ] || exit 0
    sleep 0.1
done
F
cat > "$t/bin/fakeaudioconn" <<'F'
#!/bin/sh
echo "audioconn $*" >> "$SHM/order.log"
echo "audioconn $*" >> "$SHM/audioconn.log"
[ -n "$FAKE_AUDIOCONN_HANG" ] && [ "$1" = "-b" ] && sleep 30
case "$1" in -b) exit ${FAKE_AUDIOCONN_RC:-0} ;; esac
exit 0
F
cat > "$t/bin/fakepidin" <<'F'
#!/bin/sh
ps -eo pid=,args=
F
cat > "$t/bin/fakesession" <<'F'
#!/bin/sh
# stands in for the launcher + display: creates the key file, shows after 1 s, follows the hide and stop files
echo "start" >> "$SHM/session.log"
echo "$LD_LIBRARY_PATH" > "$SHM/ld.log"
echo "$PATH" > "$SHM/path.log"
echo "rt=${CARLINK_READ_THREAD-} audio=${CARLINK_AUDIO_DUMP-} play=${CARLINK_AUDIO_PLAY-} ldmm=$(case "$LD_LIBRARY_PATH" in */opt/mm/lib) echo 1 ;; esac)" > "$SHM/opt.log"
echo "session $(date +%s)" >> "$SHM/order.log"
echo "wifi=${CARLINK_WIFI_PHONE-}" >> "$SHM/wifi.log"
: > "$CARLINK_INPUT_FILE"
n=$(grep -c start "$SHM/session.log")
if [ "$n" -le "${FAKE_FAIL:-0}" ]; then sleep 0.5; echo "fail" >> "$SHM/session.log"; exit 12; fi
sleep 1
if [ -n "$FAKE_DIE_AFTER" ] && [ "$n" -eq 1 ]; then : > "$CARLINK_DISPLAY_SHOWN"; sleep "$FAKE_DIE_AFTER"; rm -f "$CARLINK_DISPLAY_SHOWN"; echo "died" >> "$SHM/session.log"; exit 12; fi
while :; do
    if [ -e "$CARLINK_SESSION_STOP" ]; then rm -f "$CARLINK_DISPLAY_SHOWN"; echo "stopped" >> "$SHM/session.log"; exit 0; fi
    if [ -e "$CARLINK_DISPLAY_HIDE" ]; then rm -f "$CARLINK_DISPLAY_SHOWN"; else : > "$CARLINK_DISPLAY_SHOWN"; fi
    sleep 0.3
done
F
cat > "$t/bin/fakearp" <<'F'
#!/bin/sh
echo '? (169.254.213.50) at 5c:53:10:29:2b:cc on dwc0'
echo '? (172.16.224.101) at 1a:7d:f3:5b:89:af on uap0'
echo '? (172.16.224.103) at 6a:1d:79:b4:44:13 on uap0'
echo '? (172.16.224.1) at bc:30:7e:30:93:c4 on uap0'
F
cat > "$t/bin/fakewifiwait" <<'F'
#!/bin/sh
# stands in for "capture --wifi-wait LIST": the phone answers once the user has pressed Start ($SHM/pressed)
echo "$2 wait=$CARLINK_WIFI_WAIT" >> "$SHM/wifiwait.log"
if [ -e "$SHM/pressed" ]; then echo 172.16.224.103; exit 0; fi
sleep 0.5; exit 1
F
cat > "$t/bin/fakewatch" <<'F'
#!/bin/sh
echo "watch start emit=$(wc -c < "$CARLINK_INPUT_FILE" 2>/dev/null) hide=$CARLINK_EMIT_HIDE shown=$CARLINK_WATCH_SHOWN lmc=$CARLINK_FAST_HIDE_LMC" >> "$SHM/watch.log"
while [ ! -e "$CARLINK_WATCH_STOP" ]; do sleep 0.2; done
echo "watch stop" >> "$SHM/watch.log"
F
cat > "$t/bin/fakelmc" <<'F'
#!/bin/sh
echo "$*" >> "$SHM/lmc.log"
F
chmod +x "$t/bin"/*

state() { cat "$shm/nbt-aad-state" 2>/dev/null; }
until_state() { # PATTERN SECONDS
    for _ in $(seq $(( $2 * 10 ))); do case "$(state)" in $1) return 0 ;; esac; sleep 0.1; done; return 1
}
cmd() { echo "$*" >> "$shm/cmds"; }
start() { # SHELL [env...]
    local sh=$1; shift
    pkill -f "$t/bin/fake" 2>/dev/null; pkill -f "$here/package/nbt-aad" 2>/dev/null; sleep 0.5
    rm -rf "$shm"; mkdir -p "$shm/ctx"; rm -f "$dev"/*; : > "$shm/cmds"; : > "$shm/ctx/cpvideoctx7"
    env SHM="$shm" FAKE_CMDS="$shm/cmds" CL_SHM="$shm" CL_DEV="$dev" CL_CTL="$t/bin/fakectl" CL_NO_STAGE=1 \
        CL_CTX_ROOT="$shm/ctx" CL_PIDIN="$t/bin/fakepidin" CARLINK_WATCH=1 CL_AUDIOCONN="$t/bin/fakeaudioconn" \
        CL_SESSION_CMD="$t/bin/fakesession" CL_WATCH_CMD="$t/bin/fakewatch" \
        CL_PURGE_CMD="if [ -e $dev/android0 ] || [ -e $dev/oap0 ]; then echo REFUSED-PLUGGED >> $shm/purge.log; else echo purged >> $shm/purge.log; rm -f $shm/ctx/cpvideoctx*; fi" \
        CL_LMC="$t/bin/fakelmc" CL_LOG="$shm/log" "$@" $sh "$here/package/nbt-aad" > "$shm/out" 2>&1 &
    sup=$!
}
plug() { # NODE: plug the phone once the factory context has been cleared (the supervisor wants it unplugged until then)
    for _ in $(seq 100); do { [ -e "$shm/purge.log" ] || ! ls "$shm/ctx"/cpvideoctx* >/dev/null 2>&1; } && break; sleep 0.1; done
    : > "$dev/$1"
}
replugger() { # NODE: acts as the user: whenever the supervisor asks for a replug, unplug and plug the phone again
    local node=$1 seen=0 n
    while :; do
        n=$(grep -c "plug it in again" "$shm/log" 2>/dev/null); n=${n:-0}
        if [ "$n" -gt "$seen" ]; then seen=$n; rm -f "$dev/$node"; sleep 1.5; : > "$dev/$node"; fi
        sleep 0.2
    done
}
gone() { for _ in $(seq 250); do kill -0 "$1" 2>/dev/null || return 0; sleep 0.1; done; return 1; }
leftovers() { pgrep -f "$t/bin/fake" >/dev/null && return 0; return 1; }

for sh in sh bash; do
    echo "== supervisor under $sh"
    # 1. waits for the phone, then shows; hide/show; reverse; park assist
    start $sh
    until_state "connecting $sup 0" 5 && ok "$sh: waits for the phone (connecting, overlay 0)" || fail "$sh: first state '$(state)'"
    [ ! -e "$shm/session.log" ] && ok "$sh: no session before a phone appears" || fail "$sh: session started without a phone"
    plug android0
    until_state "connected $sup 1" 8 && ok "$sh: phone appears -> session -> connected, overlay 1" || fail "$sh: state after phone '$(state)'"
    grep -q purged "$shm/purge.log" && ok "$sh: factory CarPlay context purge ran first" || fail "$sh: no purge"
    grep -q "emit=0 hide=$shm/carlink-hide shown=$shm/carlink-shown lmc=$t/bin/fakelmc" "$shm/watch.log" && ok "$sh: watcher started on an empty key file with the hide, shown and LayerManagerControl paths wired in" || fail "$sh: watcher start line: $(cat "$shm/watch.log" 2>/dev/null)"
    cmd hide; until_state "connecting $sup 0" 3 && [ -e "$shm/carlink-hide" ] && grep -q "render order 0x100" "$shm/lmc.log" && ok "$sh: hide -> iDrive layer restored at once, overlay 0, hide file set" || fail "$sh: hide: '$(state)' lmc=$(cat "$shm/lmc.log" 2>/dev/null)"
    cmd show; until_state "connected $sup 1" 4 && [ ! -e "$shm/carlink-hide" ] && ok "$sh: show -> picture back" || fail "$sh: show: '$(state)'"
    cmd 'gear 2'; until_state "connecting $sup 0" 3 && [ -e "$shm/carlink-hide" ] && ok "$sh: reverse gear hands the panel to iDrive" || fail "$sh: reverse: '$(state)'"
    cmd show; sleep 1; [ -e "$shm/carlink-hide" ] && ok "$sh: show does not override reverse gear" || fail "$sh: show overrode reverse"
    cmd 'gear 1'; until_state "connected $sup 1" 4 && ok "$sh: leaving reverse brings CarLife back" || fail "$sh: leave reverse: '$(state)'"
    cmd 'pdc 1'; until_state "connecting $sup 0" 3 && ok "$sh: park assist hands the panel to iDrive" || fail "$sh: pdc: '$(state)'"
    cmd show; until_state "connected $sup 1" 4 && ok "$sh: the driver choosing CarLife overrides park assist" || fail "$sh: pdc show: '$(state)'"
    cmd 'pdc 0'; cmd 'pdc 1'; until_state "connecting $sup 0" 3 && ok "$sh: a new park-assist episode hands the panel over again" || fail "$sh: pdc again: '$(state)'"
    cmd 'pdc 0'; until_state "connected $sup 1" 4 && ok "$sh: park assist ending brings CarLife back" || fail "$sh: pdc end: '$(state)'"
    cmd 'rm -rf /'; cmd 'gear'; cmd 'foo bar'; sleep 0.6; until_state "connected $sup 1" 2 && ok "$sh: unknown or malformed messages change nothing" || fail "$sh: garbage changed state '$(state)'"
    # 2. quit
    cmd quit
    gone $sup && ok "$sh: quit ends the supervisor" || fail "$sh: supervisor still running after quit"
    [ "$(state)" = "idle 0 0" ] && ok "$sh: state is idle 0 0 afterwards" || fail "$sh: final state '$(state)'"
    [ ! -e "$shm/carlink-hide" ] && [ ! -e "$shm/carlink-shown" ] && [ ! -e "$shm/carlink-stop" ] && ok "$sh: markers cleaned up" || fail "$sh: markers left: $(ls "$shm")"
    grep -q stopped "$shm/session.log" && grep -q "watch stop" "$shm/watch.log" && ok "$sh: session and watcher were stopped" || fail "$sh: session/watcher not stopped"
    sleep 1; leftovers && fail "$sh: stray processes after quit" || ok "$sh: no stray processes"

    # 3. SIGTERM
    start $sh; plug oap0; until_state "connected $sup 1" 8 || fail "$sh: sigterm run did not connect"
    kill -TERM $sup; gone $sup && [ "$(state)" = "idle 0 0" ] && ok "$sh: SIGTERM shuts down like quit" || fail "$sh: SIGTERM: '$(state)'"
    sleep 1; leftovers && fail "$sh: stray processes after SIGTERM"

    # 4. kill -9: the ticker gives the panel back and stops the session
    start $sh; plug oap0; until_state "connected $sup 1" 8 || fail "$sh: kill-9 run did not connect"
    : > "$shm/lmc.log"; kill -9 $sup; sleep 3.5
    grep -q "render order 0x100" "$shm/lmc.log" && grep -q stopped "$shm/session.log" && ok "$sh: after kill -9 the panel is restored and the session stops" || fail "$sh: kill -9 cleanup (lmc: $(cat "$shm/lmc.log"))"
    sleep 1; leftovers && fail "$sh: stray processes after kill -9"

    # 5. initial gear from the file the app writes
    echo "gear 2" > "$t/gear"; start $sh CL_GEAR_FILE="$t/gear"; plug oap0
    sleep 4; [ -e "$shm/carlink-hide" ] && ! grep -q "connected" "$shm/nbt-aad-state" && ok "$sh: starts hidden when the car is already in reverse" || fail "$sh: initial reverse: '$(state)' hide=$([ -e "$shm/carlink-hide" ] && echo y)"
    echo "gear 1" >> "$shm/cmds"; echo "gear 1" > /dev/null; cmd 'gear 1'; until_state "connected $sup 1" 4 && ok "$sh: leaving reverse shows CarLife" || fail "$sh: initial reverse end '$(state)'"
    cmd quit; gone $sup

    # 6. retries: two failures then a picture; five failures -> gives up
    start $sh FAKE_FAIL=2; plug oap0
    replugger oap0 & rp=$!
    until_state "connected $sup 1" 25 && ok "$sh: a failed handshake is retried (2 failures, then connected)" || fail "$sh: retry: '$(state)' $(cat "$shm/session.log" 2>/dev/null | tr '\n' ' ')"
    kill $rp 2>/dev/null; cmd quit; gone $sup
    # without a replug a failed attempt is NOT retried (the phone must re-enumerate)
    start $sh FAKE_FAIL=9; plug oap0
    sleep 6
    [ "$(grep -c '^start' "$shm/session.log")" = 1 ] && grep -q "plug it in again" "$shm/log" && ok "$sh: after a failure it waits for the phone to be replugged" || fail "$sh: no-replug: starts=$(grep -c '^start' "$shm/session.log") $(cat "$shm/log" | tr '\n' '|')"
    [ -s "$shm/carlink-session.log.prev" ] || [ -e "$shm/carlink-session.log" ] && ok "$sh: session output goes to carlink-session.log" || fail "$sh: no session log file"
    replugger oap0 & rp=$!
    gone $sup; kill $rp 2>/dev/null
    [ "$(state)" = "idle 0 0" ] && [ "$(grep -c '^start' "$shm/session.log")" = 3 ] && ok "$sh: gives up after 3 attempts without a picture, state idle" || fail "$sh: give-up: starts=$(grep -c '^start' "$shm/session.log") state='$(state)'"

    # 7. control port taken: nothing is touched
    rm -rf "$shm"; mkdir -p "$shm"; echo "connected 99999 1" > "$shm/nbt-aad-state"; : > "$shm/carlink-hide"
    env SHM="$shm" CL_SHM="$shm" CL_DEV="$dev" CL_CTL="$t/bin/fakectl" FAKE_CTL_FAIL=1 CL_BIND_TRIES=1 FAKE_CMDS="$shm/cmds" CL_NO_STAGE=1 CL_LOG="$shm/log" $sh "$here/package/nbt-aad" > "$shm/out" 2>&1; rc=$?
    [ $rc -eq 3 ] && [ "$(state)" = "connected 99999 1" ] && [ -e "$shm/carlink-hide" ] && ok "$sh: port taken -> exit 3 and another daemon's files are left alone" || fail "$sh: port taken rc=$rc state='$(state)'"

    # 8. "quit" while the control port is still being taken (the app sends it for a stale instance): exits cleanly, no
    #    ctl helper left holding the port, and the next start works
    start $sh FAKE_CTL_DELAY=3
    sleep 1; cmd quit
    gone $sup && ok "$sh: quit during start ends the supervisor" || fail "$sh: quit during start: still running"
    sleep 1
    leftovers && fail "$sh: helper processes left behind after quit during start" || ok "$sh: no helper left behind after quit during start"
    start $sh
    until_state "connecting $sup 0" 5 && ok "$sh: a fresh start after that works" || fail "$sh: fresh start '$(state)'"
    cmd quit; gone $sup

    # 10. purge order: the factory CarPlay context is destroyed BEFORE the phone is plugged in
    start $sh
    until_state "connecting $sup 0" 5
    for _ in $(seq 40); do grep -q '^purged' "$shm/purge.log" 2>/dev/null && break; sleep 0.1; done
    [ "$(cat "$shm/purge.log" 2>/dev/null)" = purged ] && [ ! -e "$shm/session.log" ] && ok "$sh: CarPlay context purged while waiting, before any phone" || fail "$sh: purge order: '$(cat "$shm/purge.log" 2>/dev/null)'"
    : > "$dev/android0"
    until_state "connected $sup 1" 8 && ok "$sh: then the phone starts the session" || fail "$sh: session after purge: '$(state)'"
    [ "$(cat "$shm/purge.log")" = purged ] && ok "$sh: purge ran exactly once" || fail "$sh: purge log $(cat "$shm/purge.log" | tr '\n' ' ')"
    cmd quit; gone $sup

    # 11. phone already plugged in while CarPlay still holds the decoder: asks to unplug, purges, waits for the replug
    start $sh; : > "$dev/android0"
    sleep 3
    grep -q "unplug the phone now" "$shm/log" && [ ! -e "$shm/session.log" ] && [ ! -e "$shm/purge.log" ] && ok "$sh: plugged phone -> asked to unplug, nothing started" || fail "$sh: plugged start: log=$(cat "$shm/log" | tr '\n' '|')"
    rm -f "$dev/android0"
    for _ in $(seq 80); do grep -q '^purged' "$shm/purge.log" 2>/dev/null && break; sleep 0.1; done
    [ "$(cat "$shm/purge.log" 2>/dev/null)" = purged ] && ok "$sh: unplugged -> purge ran with the phone absent" || fail "$sh: purge after unplug: '$(cat "$shm/purge.log" 2>/dev/null)'"
    : > "$dev/android0"
    until_state "connected $sup 1" 10 && ok "$sh: replug -> session" || fail "$sh: replug: '$(state)'"
    cmd quit; gone $sup

    # 12. no factory context (already purged): a plugged phone starts at once, no purge
    start $sh; rm -f "$shm/ctx"/cpvideoctx*; : > "$dev/android0"
    until_state "connected $sup 1" 10 && [ ! -e "$shm/purge.log" ] && ok "$sh: no context -> no purge, straight to the session" || fail "$sh: no-context start '$(state)' purge=$(cat "$shm/purge.log" 2>/dev/null)"
    cmd quit; gone $sup

    # 15. children get the ssh-login library path
    start $sh CL_LD_PATH=/boot/lib:/lib:/opt/sys/lib CL_PATH="$PATH:/opt/car/bin"; rm -f "$shm/ctx"/cpvideoctx*; plug android0
    until_state "connected $sup 1" 10
    [ "$(cat "$shm/ld.log" 2>/dev/null)" = "/boot/lib:/lib:/opt/sys/lib" ] && ok "$sh: session inherits LD_LIBRARY_PATH" || fail "$sh: LD_LIBRARY_PATH='$(cat "$shm/ld.log" 2>/dev/null)'"
    [ "$(cat "$shm/path.log" 2>/dev/null)" = "$PATH:/opt/car/bin" ] && ok "$sh: session inherits the ssh-login PATH" || fail "$sh: PATH='$(cat "$shm/path.log" 2>/dev/null)'"
    cmd quit; gone $sup

    # 16. the layer state is logged once the picture is marked shown
    start $sh; rm -f "$shm/ctx"/cpvideoctx*; plug android0
    until_state "connected $sup 1" 10; sleep 3
    grep -q "get surface 1510" "$shm/lmc.log" && grep -q "layer state 2 s after" "$shm/log" && [ "$(grep -c "get surface 1510" "$shm/lmc.log")" = 1 ] && ok "$sh: layer state logged once after the picture was shown" || fail "$sh: layer log: $(cat "$shm/lmc.log" 2>/dev/null | tr '\n' '|')"
    cmd quit; gone $sup
    # 17. the session log keeps every attempt
    start $sh FAKE_FAIL=2; rm -f "$shm/ctx"/cpvideoctx*; plug oap0
    replugger oap0 & rp=$!
    until_state "connected $sup 1" 25; kill $rp 2>/dev/null
    [ "$(grep -c "attempt starting" "$shm/carlink-session.log")" = 3 ] && ok "$sh: session log holds all 3 attempts" || fail "$sh: attempts in session log: $(grep -c "attempt starting" "$shm/carlink-session.log")"
    cmd quit; gone $sup

    # 18. a picture that ended after it was shown: not a "failed attempt", clear message, replug restarts it
    start $sh FAKE_DIE_AFTER=2.5; rm -f "$shm/ctx"/cpvideoctx*; plug oap0
    replugger oap0 & rp=$!
    for _ in $(seq 150); do grep -q "the CarLife picture ended" "$shm/log" 2>/dev/null && break; sleep 0.1; done
    grep -q "the CarLife picture ended" "$shm/log" && grep -q "picture shown: yes, failed attempts in a row: 0, ran [0-9]* s" "$shm/log" && ok "$sh: an ended picture is reported as such, with the time it ran" || fail "$sh: ended picture log: $(tail -4 "$shm/log")"
    ! grep -q "attempt 0 failed" "$shm/log" && ok "$sh: an ended picture is not reported as attempt 0 failed" || fail "$sh: old wording"
    until_state "connected $sup 1" 25; kill $rp 2>/dev/null
    [ "$(grep -c "attempt starting" "$shm/carlink-session.log")" = 2 ] && ok "$sh: replugging the phone starts CarLife again" || fail "$sh: attempts after replug: $(grep -c "attempt starting" "$shm/carlink-session.log")"
    cmd quit; gone $sup

    # 19. read thread and audio dump are opt-in
    start $sh; rm -f "$shm/ctx"/cpvideoctx*; : > "$dev/android0"
    until_state "connected $sup 1" 10
    [ "$(cat "$shm/opt.log")" = "rt= audio= play= ldmm=" ] && [ ! -e "$shm/carlink-audio.raw" ] && ok "$sh: read thread and audio dump are off by default" || fail "$sh: defaults: $(cat "$shm/opt.log")"
    cmd quit; gone $sup
    start $sh; rm -f "$shm/ctx"/cpvideoctx*; : > "$shm/carlink-readthread-enable"; : > "$shm/carlink-audio-enable"; : > "$dev/android0"
    until_state "connected $sup 1" 10
    [ "$(cat "$shm/opt.log")" = "rt=1 audio=$shm/carlink-audio.raw play= ldmm=" ] && [ -e "$shm/carlink-audio.raw" ] && ok "$sh: enable files turn on the read thread and the audio dump" || fail "$sh: enabled: $(cat "$shm/opt.log")"
    cmd quit; gone $sup

    # 20. a session stopped on purpose (stop file) is reported as stopped, not as a broken link
    start $sh; rm -f "$shm/ctx"/cpvideoctx*; : > "$dev/android0"
    until_state "connected $sup 1" 10; sleep 1.5; : > "$shm/carlink-stop"
    for _ in $(seq 100); do grep -q "session was stopped" "$shm/log" 2>/dev/null && break; sleep 0.1; done
    grep -q "the CarLife session was stopped" "$shm/log" && ! grep -q "USB link broke" "$shm/log" && ok "$sh: a stop file ends the session and is reported as stopped" || fail "$sh: stop wording: $(tail -3 "$shm/log")"
    cmd quit; gone $sup


    # 21. audio playback is opt-in; its route (audioconn) is built once before the session, released after, never fatal
    start $sh; rm -f "$shm/ctx"/cpvideoctx*; : > "$dev/android0"
    until_state "connected $sup 1" 10
    [ ! -e "$shm/audioconn.log" ] && [ "$(cat "$shm/opt.log")" = "rt= audio= play= ldmm=" ] && ok "$sh: audio playback is off by default: no audioconn, no player" || fail "$sh: audio default: $(cat "$shm/audioconn.log" 2>/dev/null) $(cat "$shm/opt.log")"
    cmd quit; gone $sup
    start $sh; rm -f "$shm/ctx"/cpvideoctx*; : > "$shm/carlink-audio-play-enable"; : > "$dev/android0"
    until_state "connected $sup 1" 10
    [ "$(cat "$shm/audioconn.log")" = "audioconn -b 0xb8" ] && [ "$(sed -n 1p "$shm/order.log" | cut -d' ' -f1)" = audioconn ] && grep -q "play=1 ldmm=1" "$shm/opt.log" && ok "$sh: the route is built once, before the session starts, and the player is switched on" || fail "$sh: audio on: '$(cat "$shm/audioconn.log")' order: $(cat "$shm/order.log") opt: $(cat "$shm/opt.log")"
    [ -f "$shm/carlink-audioconn.lock" ] && ok "$sh: the lock is held while the route is up" || fail "$sh: no lock"
    sleep 1.5; : > "$shm/carlink-stop"
    for _ in $(seq 100); do grep -q "route released" "$shm/log" 2>/dev/null && break; sleep 0.1; done
    [ "$(cat "$shm/audioconn.log")" = "audioconn -b 0xb8
audioconn -r 0xb8" ] && [ ! -e "$shm/carlink-audioconn.lock" ] && ok "$sh: the route is released when the session ends, and the lock is removed" || fail "$sh: release: '$(cat "$shm/audioconn.log")' $(tail -3 "$shm/log")"
    cmd quit; gone $sup
    start $sh FAKE_AUDIOCONN_RC=1; rm -f "$shm/ctx"/cpvideoctx*; : > "$shm/carlink-audio-play-enable"; : > "$dev/android0"
    until_state "connected $sup 1" 10
    grep -q "audioconn -b failed (status 1)" "$shm/log" && [ "$(cat "$shm/audioconn.log")" = "audioconn -b 0xb8" ] && [ ! -e "$shm/carlink-audioconn.lock" ] && ok "$sh: a failed route is logged, not retried, not released, the session runs" || fail "$sh: audioconn failure: $(tail -4 "$shm/log")"
    cmd quit; gone $sup
    start $sh FAKE_AUDIOCONN_HANG=1 CL_AUDIOCONN_WAIT=2; rm -f "$shm/ctx"/cpvideoctx*; : > "$shm/carlink-audio-play-enable"; : > "$dev/android0"
    until_state "connected $sup 1" 15
    grep -q "audioconn -b failed (status 124)" "$shm/log" && ok "$sh: a hanging audioconn is stopped after the time limit and the session still runs" || fail "$sh: audioconn hang: $(tail -4 "$shm/log")"
    cmd quit; gone $sup
    start $sh CL_AUDIOCONN=/nonexistent/audioconn; rm -f "$shm/ctx"/cpvideoctx*; : > "$shm/carlink-audio-play-enable"; : > "$dev/android0"
    until_state "connected $sup 1" 10
    grep -q "audioconn not found" "$shm/log" && ok "$sh: no audioconn on the car: logged, the session runs without sound" || fail "$sh: no audioconn: $(tail -4 "$shm/log")"
    cmd quit; gone $sup
    start $sh; rm -f "$shm/ctx"/cpvideoctx*; : > "$shm/carlink-audio-play-enable"; : > "$shm/carlink-audioconn.lock"; : > "$dev/android0"
    until_state "connected $sup 1" 10
    [ ! -e "$shm/audioconn.log" ] && grep -q "not starting another" "$shm/log" && ok "$sh: with the lock present no second audioconn is started" || fail "$sh: lock: $(cat "$shm/audioconn.log" 2>/dev/null) $(tail -3 "$shm/log")"
    cmd quit; gone $sup
    start $sh; rm -f "$shm/ctx"/cpvideoctx*; : > "$shm/carlink-audio-play-enable"; : > "$dev/android0"
    until_state "connected $sup 1" 10
    cmd quit; gone $sup
    [ "$(tail -1 "$shm/audioconn.log")" = "audioconn -r 0xb8" ] && [ ! -e "$shm/carlink-audioconn.lock" ] && ok "$sh: quitting while the route is up releases it" || fail "$sh: quit release: '$(cat "$shm/audioconn.log")'"


    # 22. persistent post-mortem copies of the logs are opt-in, replaced not appended, rotated once per boot, and outlive the supervisor
    rm -rf "$t/keep"
    start $sh CL_KEEP="$t/keep" CL_KEEP_STEPS="0 1 1"; rm -f "$shm/ctx"/cpvideoctx*; : > "$dev/android0"
    until_state "connected $sup 1" 10; sleep 1.5; : > "$shm/carlink-stop"
    for _ in $(seq 100); do grep -q "session was stopped" "$shm/log" 2>/dev/null && break; sleep 0.1; done; sleep 2.5
    [ ! -e "$t/keep" ] && ok "$sh: no persistent log copies unless asked for" || fail "$sh: keep dir without opt-in"
    cmd quit; gone $sup
    rm -rf "$t/keep"; mkdir -p "$t/keep"; echo OLD > "$t/keep/supervisor.log"
    start $sh CL_KEEP="$t/keep" CL_KEEP_STEPS="0 1 1" CL_KEEP_LIVE=1 CARLINK_KEEP_LOG=1; rm -f "$shm/ctx"/cpvideoctx*; : > "$dev/android0"
    until_state "connected $sup 1" 10
    [ "$(cat "$t/keep/prev-supervisor.log" 2>/dev/null)" = OLD ] && ok "$sh: the previous boot's copy is kept as prev-*" || fail "$sh: rotation: $(ls "$t/keep")"
    for _ in $(seq 60); do grep -q "== live at" "$t/keep/supervisor.log" 2>/dev/null && break; sleep 0.1; done
    grep -q "== live at" "$t/keep/supervisor.log" && ok "$sh: the logs are also copied while a session is still running" || fail "$sh: no live copy: $(head -2 "$t/keep/supervisor.log")"
    : > "$shm/carlink-stop"
    for _ in $(seq 100); do grep -q "== session ended" "$t/keep/supervisor.log" 2>/dev/null && break; sleep 0.1; done
    grep -q "== session ended" "$t/keep/supervisor.log" && grep -q "session ended (status" "$t/keep/supervisor.log" && [ -s "$t/keep/session.log" ] && [ -s "$t/keep/shm.txt" ] && ! grep -q OLD "$t/keep/supervisor.log" && ok "$sh: after a session the log tails are copied to the persistent directory" || fail "$sh: keep copy: $(head -3 "$t/keep/supervisor.log" 2>/dev/null)"
    cmd quit; gone $sup; sleep 3.5
    grep -q "== shutdown +" "$t/keep/supervisor.log" && grep -q "shutting down" "$t/keep/supervisor.log" && ok "$sh: copies continue after the supervisor has gone" || fail "$sh: after quit: $(head -2 "$t/keep/supervisor.log")"
    [ "$(ls "$t/keep" | grep -c tmp)" = 0 ] && ok "$sh: no half-written copies are left" || fail "$sh: tmp files: $(ls "$t/keep")"
    rm -rf "$t/keep"

    # 13. the knob watcher is opt-in
    start $sh CARLINK_WATCH=; rm -f "$shm/ctx"/cpvideoctx*; : > "$dev/android0"
    until_state "connected $sup 1" 10; sleep 1
    [ ! -e "$shm/watch.log" ] && grep -q "knob watcher is off" "$shm/log" && ok "$sh: watcher not started by default" || fail "$sh: watcher default: $(cat "$shm/watch.log" 2>/dev/null)"
    cmd quit; gone $sup
    start $sh CARLINK_WATCH=; rm -f "$shm/ctx"/cpvideoctx*; : > "$shm/carlink-watch-enable"; : > "$dev/android0"
    until_state "connected $sup 1" 10; sleep 1
    [ -e "$shm/watch.log" ] && ok "$sh: watcher starts when carlink-watch-enable exists" || fail "$sh: watcher enable file ignored"
    cmd quit; gone $sup

    # 14. a ctl left behind by a dead supervisor holds the port: it is found by name and removed
    start $sh FAKE_CTL_LOCK="$t/ctl.lock" CL_BIND_TRIES=4
    cmd quit; gone $sup
    rm -rf "$shm"; mkdir -p "$shm"; : > "$shm/cmds"; echo "connecting 99998 0" > "$shm/nbt-aad-state"
    FAKE_CTL_ORPHAN="$t/ctl.lock" "$t/bin/fakectl" --serve 52790 >/dev/null 2>&1 &
    orphan=$!; sleep 0.5
    env SHM="$shm" CL_SHM="$shm" CL_DEV="$dev" CL_CTL="$t/bin/fakectl" CL_PORT=52790 FAKE_CTL_LOCK="$t/ctl.lock" FAKE_CMDS="$shm/cmds" CL_PIDIN="$t/bin/fakepidin" \
        CL_NO_STAGE=1 CL_CTX_ROOT="$shm/none" CL_LOG="$shm/log" CL_SESSION_CMD="$t/bin/fakesession" CL_WATCH_CMD="$t/bin/fakewatch" CL_LMC="$t/bin/fakelmc" CL_BIND_TRIES=4 $sh "$here/package/nbt-aad" > "$shm/out" 2>&1 &
    sup=$!
    until_state "connecting $sup 0" 15 && ! kill -0 "$orphan" 2>/dev/null && ok "$sh: stale ctl holding the port was removed, supervisor is up" || fail "$sh: stale ctl: state '$(state)' orphan alive=$(kill -0 "$orphan" 2>/dev/null && echo yes)"
    cmd quit; gone $sup
    kill "$orphan" 2>/dev/null

    # 9. the port is released a moment late: retried, then up
    start $sh FAKE_CTL_FAIL_ONCE="$t/failed-once-$sh"
    until_state "connecting $sup 0" 15 && ok "$sh: first bind fails, retry succeeds" || fail "$sh: bind retry: '$(state)'"
    cmd quit; gone $sup

    # 10. wireless CarLife: no USB node; waits for a hotspot phone to press Start, then runs the session with its address
    start $sh CARLINK_WIFI=1 CL_ARP="$t/bin/fakearp" CL_WIFI_WAIT_CMD="$t/bin/fakewifiwait" CL_WIFI_POLL=1 FAKE_DIE_AFTER=2
    until_state "connecting $sup 0" 5 && ok "$sh: wireless: up and waiting" || fail "$sh: wireless first state '$(state)'"
    for _ in $(seq 60); do grep -q "waiting for a phone on the car hotspot" "$shm/log" 2>/dev/null && break; sleep 0.1; done
    grep -q "hotspot clients: 172.16.224.101,172.16.224.103)" "$shm/log" && ok "$sh: wireless: hotspot clients come from arp on uap0, the car's own address left out" || fail "$sh: wireless clients: $(grep hotspot "$shm/log")"
    grep -q "^172.16.224.101,172.16.224.103 wait=1$" "$shm/wifiwait.log" && ok "$sh: wireless: the receiver is asked which client opened port 7240 (poll 1 s)" || fail "$sh: wifiwait args: $(cat "$shm/wifiwait.log")"
    sleep 1.5
    [ ! -e "$shm/session.log" ] && ok "$sh: wireless: no session before Start is pressed on the phone" || fail "$sh: wireless session without Start"
    : > "$shm/pressed"
    until_state "connected $sup 1" 8 && ok "$sh: wireless: Start pressed -> session -> connected, overlay 1" || fail "$sh: wireless state '$(state)'"
    grep -q "^wifi=172.16.224.103$" "$shm/wifi.log" && ok "$sh: wireless: the session gets CARLINK_WIFI_PHONE=172.16.224.103" || fail "$sh: wifi env: $(cat "$shm/wifi.log")"
    grep -q "wireless: phone 172.16.224.103 is ready" "$shm/log" && ok "$sh: wireless: the ready phone is logged" || fail "$sh: wireless ready log"
    for _ in $(seq 80); do [ "$(grep -c . "$shm/wifi.log" 2>/dev/null)" -ge 2 ] && break; sleep 0.1; done
    grep -q "wireless session ended: press Start connection on the phone to connect again" "$shm/log" && ok "$sh: wireless: an ended session asks to press Start again" || fail "$sh: wireless end log"
    ! grep -q "unplug" "$shm/log" && ok "$sh: wireless: never asks to unplug a phone" || fail "$sh: wireless asked to unplug"
    [ "$(grep -c . "$shm/wifi.log")" -ge 2 ] && ok "$sh: wireless: a new session starts when the phone is ready again" || fail "$sh: wireless second session"
    cmd quit; gone $sup && ok "$sh: wireless: quits cleanly" || fail "$sh: wireless quit"
    leftovers && fail "$sh: wireless leftovers" || ok "$sh: wireless: no helpers left"

    # 11. a fixed phone address in carlink-wifi-phone replaces the arp list
    start $sh CARLINK_WIFI=1 CL_ARP="$t/bin/fakearp" CL_WIFI_WAIT_CMD="$t/bin/fakewifiwait" CL_WIFI_POLL=1
    echo 172.16.224.120 > "$shm/carlink-wifi-phone"
    for _ in $(seq 60); do grep -q "^172.16.224.120 " "$shm/wifiwait.log" 2>/dev/null && break; sleep 0.1; done
    grep -q "^172.16.224.120 wait=1$" "$shm/wifiwait.log" && ok "$sh: wireless: carlink-wifi-phone overrides the arp list" || fail "$sh: wifi-phone file: $(cat "$shm/wifiwait.log" 2>/dev/null)"
    cmd quit; gone $sup
done
# the upstream app parses the state with "%23s %d %d"
read -r a b c < <(echo "connected 123 1"); [ "$a" = connected ] && [ "$b" = 123 ] && [ "$c" = 1 ] || fail "state format"
[ $bad -eq 0 ] && echo "carlife menu supervisor tests ok"
exit $bad
