#!/usr/bin/env bash
# Offline check of build/live-input-candidate/src/carlife_qnx_ctl.c: UDP ping/pong, message filter, socket layout
# fallback, library fallback, port conflict, argument rules, exit when the supervisor or reader is gone.
set -u
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
src=$root/build/live-input-candidate/src
tests=$root/tools/tests
work=$(mktemp -d); trap 'kill $(jobs -p) 2>/dev/null; rm -rf "$work"' EXIT
build() { # build SRC_DIR OUTPUT
    gcc -O1 -g -std=gnu11 -w -Ddlopen=t_dlopen -Ddlsym=t_dlsym -D__get_errno_ptr=t_errno_ptr -I"$1" \
        -c "$tests/ctl-impl.c" -o "$work/impl.o" || exit 1
    gcc -O1 -g -std=gnu11 -Wall -Wextra -c "$tests/ctl-host.c" -o "$work/host.o" || exit 1
    gcc "$work/impl.o" "$work/host.o" -o "$2" || exit 1
}
build "$src" "$work/ctl"
bad=0
fail() { echo "FAIL: $1"; bad=1; }
port=$((20000 + RANDOM % 20000))
udp() { python3 -I - "$@" <<'PY'
import socket, sys
port = int(sys.argv[1]); want_reply = sys.argv[2] == "reply"
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.settimeout(1.0)
for m in sys.argv[3:]:
    s.sendto(m.encode("latin-1").replace(b"\\n", b"\n"), ("127.0.0.1", port))
if want_reply:
    try: print(s.recv(16).decode(), end="")
    except socket.timeout: print("TIMEOUT", end="")
PY
}
start() { # extra env..., output to $work/out
    : > "$work/out"; env "$@" "$work/ctl" --serve "$port" > "$work/out" 2>&1 & ctl=$!
    for _ in $(seq 50); do grep -q "bound UDP" "$work/out" 2>/dev/null && return 0; kill -0 $ctl 2>/dev/null || return 1; sleep 0.1; done; return 1
}

# 1. ping/pong and the message filter (Linux layout, default library)
start X=1 || fail "ctl did not start"
[ "$(udp $port reply ping)" = pong ] && echo "PASS: ping is answered with pong" || fail "no pong"
[ "$(udp $port reply 'ping\n')" = pong ] && echo "PASS: ping with a newline is answered too" || fail "no pong for ping\\n"
udp $port noreply show 'hide\n' 'gear 2' 'pdc 1' 'quit' 'SHOW' 'rm -rf /' '' 'gear 2;ls' 'abcdefghijklmnopqrstuvwxyz0123456' 'pdc 0'
sleep 0.5
expected=$'show\nhide\ngear 2\npdc 1\nquit\npdc 0'
got=$(grep -v '^ctl:' "$work/out")
[ "$got" = "$expected" ] && echo "PASS: only well-formed commands reach stdout, in order" || { fail "stdout was: $got"; }
grep -q "linux address layout" "$work/out" && echo "PASS: Linux layout accepted when BSD is refused" || fail "layout log"
kill $ctl 2>/dev/null; wait $ctl 2>/dev/null

# 2. BSD (QNX) layout is tried first
start MOCK_QNX=1 || fail "ctl did not start (qnx layout)"
grep -q "bsd address layout" "$work/out" && [ "$(udp $port reply ping)" = pong ] && echo "PASS: BSD layout with a length byte (QNX)" || fail "bsd layout"
kill $ctl 2>/dev/null; wait $ctl 2>/dev/null

# 3. library fallback and missing socket API
start MOCK_LIBS=libsocket.so.2 && grep -q "socket functions from libsocket.so.2" "$work/out" && echo "PASS: falls back through libsocket.so.2" || fail "library fallback"
kill $ctl 2>/dev/null; wait $ctl 2>/dev/null
env MOCK_LIBS=none "$work/ctl" --serve "$port" > "$work/out" 2>&1; rc=$?
[ $rc -eq 4 ] && echo "PASS: no socket library -> exit 4" || fail "missing library rc=$rc"

# 4. port in use
start X=1; first=$ctl
env X=1 "$work/ctl" --serve "$port" > "$work/out2" 2>&1; rc=$?
[ $rc -eq 3 ] && echo "PASS: second instance on the same port exits 3" || fail "port conflict rc=$rc"
kill $first 2>/dev/null; wait $first 2>/dev/null

# 5. arguments
for args in "" "--serve" "--serve abc" "--serve 80" "--serve 70000" "--serve 5x" "--serve 52790 extra" "--other 52790"; do
    "$work/ctl" $args > /dev/null 2>&1; rc=$?
    [ $rc -eq 2 ] || fail "arguments '$args' rc=$rc"
done
echo "PASS: bad arguments exit 2"

# 6. exits when the supervisor is gone (SIGKILL leaves no orphan holding the port)
rm -f "$work/pid"
sh -c "'$work/ctl' --serve $port > '$work/out' 2>&1 & echo \$! > '$work/pid'; sleep 30" & sup=$!
sleep 1; ctlpid=$(cat "$work/pid"); kill -9 $sup; sleep 3.5
kill -0 "$ctlpid" 2>/dev/null && { fail "ctl survived its supervisor"; kill "$ctlpid"; } || echo "PASS: ctl exits within seconds of its supervisor dying"

# 7. exits when nobody reads stdout
mkfifo "$work/fifo"; ( "$work/ctl" --serve "$port" > "$work/fifo" 2>/dev/null; echo $? > "$work/rc7" ) & 
exec 8< "$work/fifo"; sleep 0.5; exec 8<&-
udp $port noreply show; sleep 1.5
[ -f "$work/rc7" ] && echo "PASS: ctl exits when its reader is gone" || fail "ctl kept running without a reader"
if [ "${CTL_NO_MUTATION-}" != 1 ]; then
  echo "== mutation checks (each must make this script fail)"
  mut() { # NAME SED
      mkdir "$work/m-$1"; cp "$src"/carlife_qnx_ctl.c "$work/m-$1/"; sed -i "$2" "$work/m-$1/carlife_qnx_ctl.c"
      cmp -s "$src/carlife_qnx_ctl.c" "$work/m-$1/carlife_qnx_ctl.c" && { echo "FAIL: mutation $1 changed nothing"; bad=1; return; }
      mkdir "$work/mt-$1"; cp "$tests"/ctl.sh "$work/mt-$1/"
      # run the same checks against the mutated source
      if sed "s|^src=.*|src=$work/m-$1|" "$tests/ctl.sh" > "$work/mt-$1/ctl.sh" && \
         CTL_NO_MUTATION=1 bash -c "cd '$tests' && sed -n '1,\$p' '$work/mt-$1/ctl.sh' | sed 's|^root=.*|root=$root|;s|^tests=.*|tests=$tests|' | bash" > "$work/mt-$1/out" 2>&1; then
          cat "$work/mt-$1/out"; echo "FAIL: mutation $1 was not caught"; bad=1
      else echo "PASS: $1 caught"; fi
  }
  mut no-parent-check 's/if (getppid() != parent)/if (0)/'
  mut no-pong 's/send_(fd, "pong", 4, 0, from, from_size);//'
  mut open-filter 's/if (!message_ok(text, size))/if (0)/'
  mut no-bsd-first 's/a\[0\] = 16; a\[1\] = AF_INET_;/a[0] = 16; a[1] = 0;/'
fi
[ $bad -eq 0 ] && echo "ctl tests ok"; exit $bad
