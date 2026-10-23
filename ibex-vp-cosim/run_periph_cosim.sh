#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/env.sh"
VSIM="$ROOT/ibex-vp-cosim/build/Vcosim_periph_server"
ELF="${1:-$ELF}"
if [[ $# -gt 0 ]]; then shift; fi
SOCKET="${IBEXDEMO_COSIM_SOCKET:-/tmp/ibexdemo-cosim.sock}"
export IBEXDEMO_COSIM_SOCKET="$SOCKET"

if [[ ! -x "$VSIM" || ! -x "$QEMU" ]]; then
  echo "Build TCG peripheral cosim: bash $ROOT/ibex-vp-cosim/build_periph_cosim.sh" >&2
  exit 1
fi
if [[ ! -f "$ELF" ]]; then
  echo "Missing firmware ELF: $ELF" >&2
  exit 1
fi
if [[ "$("$QEMU" -machine help)" != *ibexdemo-cosim* ]]; then
  echo "QEMU is missing ibexdemo-cosim. Run ibex-vp-cosim/build_periph_cosim.sh." >&2
  exit 1
fi

VPID=""
QPID=""
cleanup() {
  trap - EXIT INT TERM
  for process in "$QPID" "$VPID"; do
    if [[ -n "$process" ]] && kill -0 "$process" 2>/dev/null; then
      kill "$process" 2>/dev/null || true
      wait "$process" 2>/dev/null || true
    fi
  done
  rm -f "$SOCKET"
}
trap cleanup EXIT
trap 'exit 130' INT

rm -f "$SOCKET"
echo "QEMU TCG runs the CPU; Verilator runs GPIO, UART, and timer RTL."
echo "Socket: $SOCKET"
"$VSIM" --socket "$SOCKET" &
VPID=$!

for ((attempt=0; attempt<200; attempt++)); do
  [[ -S "$SOCKET" ]] && break
  if ! kill -0 "$VPID" 2>/dev/null; then
    wait "$VPID" || true
    exit 1
  fi
  sleep 0.05
done
if [[ ! -S "$SOCKET" ]]; then
  echo "Verilator did not create $SOCKET" >&2
  exit 1
fi

"$QEMU" -M ibexdemo-cosim -nographic -kernel "$ELF" "$@" <&0 &
QPID=$!
set +e
wait "$QPID"
RESULT=$?
set -e
QPID=""
exit "$RESULT"
