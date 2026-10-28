#!/usr/bin/env bash
# Demo: QEMU TCG runs the Ibex firmware; the TCG plugin forwards each matmul
# doorbell to the matmul accelerator daemon, a separate host process.
#
#   run_matmul_plugin.sh [--gdb] [extra QEMU args]
#
# Uses a running daemon if one answers on $IBEX_MATMUL_SOCKET; otherwise
# starts one and stops it afterwards (KEEP_DAEMON=1 leaves it running).
# QEMU exits with the firmware's result: 0 = PASS. QEMU_TIMEOUT (default 60 s;
# 0 = none) kills a hung run.
#
# --gdb starts QEMU paused with its GDB server on $GDB_PORT (default 1234) and
# no timeout; attach riscv32-unknown-elf-gdb from another terminal.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/env.sh"
B="$ROOT/ibex-vp-cosim/build"
CTL="$ROOT/ibex-vp-cosim/matmul_daemon.sh"
export IBEX_MATMUL_SOCKET="${IBEX_MATMUL_SOCKET:-/tmp/ibex-matmul-accel.sock}"
SOCK="$IBEX_MATMUL_SOCKET"
LOG="$SOCK.log"

use_gdb=0
if [[ "${1:-}" == "--gdb" ]]; then
  use_gdb=1
  shift
fi
GDB_PORT="${GDB_PORT:-1234}"
timeout_s="${QEMU_TIMEOUT:-60}"
qemu_extra=()
if [[ "$use_gdb" == 1 ]]; then
  qemu_extra=(-gdb "tcp::$GDB_PORT" -S)
  timeout_s="${QEMU_TIMEOUT:-0}"
fi
for arg in "$@"; do
  # A paused QEMU (-S) waits for a debugger; don't time it out by default.
  [[ "$arg" == "-S" ]] && timeout_s="${QEMU_TIMEOUT:-0}"
done

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
if [[ "$use_gdb" == 1 ]]; then
  echo "QEMU is paused, waiting for GDB on port $GDB_PORT. In another terminal:"
  echo "  source ./env.sh"
  echo "  riscv32-unknown-elf-gdb $B/matmul_accel.elf -ex 'target remote :$GDB_PORT'"
fi
"$QEMU" -M ibexdemo -nographic -kernel "$B/matmul_accel.elf" \
  -plugin "$B/matmul_plugin.so,socket=$SOCK" ${qemu_extra[@]+"${qemu_extra[@]}"} \
  "$@" </dev/null &
qemu_pid=$!

deadline=$((SECONDS + timeout_s))
while kill -0 "$qemu_pid" 2>/dev/null; do
  if ((timeout_s > 0 && SECONDS >= deadline)); then
    echo "QEMU still running after $timeout_s s; killing it" >&2
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
