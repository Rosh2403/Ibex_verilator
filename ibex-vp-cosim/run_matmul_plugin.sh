#!/usr/bin/env bash
# Demo: QEMU TCG runs the Ibex firmware; the TCG plugin forwards each matmul
# doorbell to the matmul accelerator daemon, a separate host process.
#
#   run_matmul_plugin.sh [extra QEMU args]
#
# Uses a running daemon if one answers on $IBEX_MATMUL_SOCKET; otherwise
# starts one and stops it afterwards (KEEP_DAEMON=1 leaves it running).
# QEMU exits with the firmware's result: 0 = PASS. QEMU_TIMEOUT (default 60 s)
# kills a hung run.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/env.sh"
B="$ROOT/ibex-vp-cosim/build"
CTL="$ROOT/ibex-vp-cosim/matmul_daemon.sh"
export IBEX_MATMUL_SOCKET="${IBEX_MATMUL_SOCKET:-/tmp/ibex-matmul-accel.sock}"
SOCK="$IBEX_MATMUL_SOCKET"
LOG="$SOCK.log"

bash "$ROOT/ibex-vp-cosim/build_matmul_plugin.sh"

started=0
qemu_pid=
cleanup() {
  if [[ -n "$qemu_pid" ]] && kill -0 "$qemu_pid" 2>/dev/null; then
    kill -KILL "$qemu_pid" 2>/dev/null || true
  fi
  if [[ "$started" == 1 && "${KEEP_DAEMON:-0}" != 1 ]]; then
    "$CTL" stop >/dev/null || true
  fi
}
trap cleanup EXIT
trap 'exit 130' INT TERM

if "$B/matmul_daemon" --socket "$SOCK" --ping >/dev/null 2>&1; then
  echo "Using running matmul daemon on $SOCK"
else
  "$CTL" start
  started=1
fi
log_start=$(($(wc -l <"$LOG" 2>/dev/null || echo 0) + 1))

echo "QEMU TCG (Ibex firmware) + TCG plugin -> matmul daemon. Not cycle-accurate."
"$QEMU" -M ibexdemo -nographic -kernel "$B/matmul_accel.elf" \
  -plugin "$B/matmul_plugin.so,socket=$SOCK" "$@" </dev/null &
qemu_pid=$!

deadline=$((SECONDS + ${QEMU_TIMEOUT:-60}))
while kill -0 "$qemu_pid" 2>/dev/null; do
  if ((SECONDS >= deadline)); then
    echo "QEMU still running after ${QEMU_TIMEOUT:-60} s; killing it" >&2
    kill -KILL "$qemu_pid" 2>/dev/null || true
    break
  fi
  sleep 0.1
done
rc=0
wait "$qemu_pid" || rc=$?

echo "--- matmul daemon log ($LOG) ---"
tail -n "+$log_start" "$LOG" 2>/dev/null || echo "(no daemon log; foreground daemon?)"
if [[ "$rc" == 0 ]]; then
  echo "Result: PASS"
else
  echo "Result: FAIL (QEMU exit $rc)"
fi
exit "$rc"
