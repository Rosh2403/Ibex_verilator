#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/env.sh"
VSIM="$ROOT/ibex-demo-system/build/lowrisc_ibex_demo_system_0/sim-verilator/Vtop_verilator"
ELF="${1:-$ELF}"
if [[ $# -gt 0 ]]; then shift; fi

if [[ ! -x "$VSIM" || ! -x "$QEMU" ]]; then
  echo "Build the full-SoC integration: bash $ROOT/ibex-vp-cosim/build_full_soc.sh" >&2
  exit 1
fi
if [[ ! -f "$ELF" ]]; then
  echo "Missing firmware ELF: $ELF" >&2
  exit 1
fi
if [[ "$("$QEMU" -machine help)" != *ibexdemo-soc-cosim* ||
      "$("$VSIM" --help)" != *--qemu-cosim-socket=* ]]; then
  echo "Simulator binaries lack full-SoC support. Run ibex-vp-cosim/build_full_soc.sh." >&2
  exit 1
fi

VPID=""
QPID=""
SOCKET_DIR=""
SOCKET="${IBEXDEMO_COSIM_SOCKET:-}"
cleanup() {
  trap - EXIT INT TERM
  for process in "$QPID" "$VPID"; do
    if [[ -n "$process" ]] && kill -0 "$process" 2>/dev/null; then
      kill "$process" 2>/dev/null || true
      for ((attempt=0; attempt<40; attempt++)); do
        kill -0 "$process" 2>/dev/null || break
        sleep 0.05
      done
      kill -KILL "$process" 2>/dev/null || true
      wait "$process" 2>/dev/null || true
    fi
  done
  if [[ -n "$SOCKET_DIR" ]]; then
    rm -f "$SOCKET"
    rmdir "$SOCKET_DIR" 2>/dev/null || true
  fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

if [[ -z "$SOCKET" ]]; then
  SOCKET_DIR="$(mktemp -d /tmp/ibexdemo-soc.XXXXXX)"
  SOCKET="$SOCKET_DIR/soc.sock"
fi
if [[ -e "$SOCKET" || -S "$SOCKET" ]]; then
  echo "Socket already exists: $SOCKET. Stop its owner before removing a stale socket." >&2
  exit 1
fi
if [[ "$SOCKET" != /* || "$SOCKET" == *,* ]]; then
  echo "Use an absolute socket path without commas." >&2
  exit 1
fi
mkdir -p "$ROOT/ibex-vp-cosim/build"
LOG_DIR="$(mktemp -d "$ROOT/ibex-vp-cosim/build/soc-run.XXXXXX")"
ELF="$(cd "$(dirname "$ELF")" && pwd)/$(basename "$ELF")"
echo "Full SoC runs in Verilator; QEMU provides stepping and the UART console."
echo "Logs: $LOG_DIR"
(cd "$LOG_DIR" && exec "$VSIM" --meminit=ram,"$ELF" \
  --qemu-cosim-socket="$SOCKET") > "$LOG_DIR/verilator.log" 2>&1 &
VPID=$!

for ((attempt=0; attempt<200; attempt++)); do
  [[ -S "$SOCKET" ]] && break
  if ! kill -0 "$VPID" 2>/dev/null; then
    cat "$LOG_DIR/verilator.log" >&2
    exit 1
  fi
  sleep 0.05
done
if [[ ! -S "$SOCKET" ]]; then
  echo "Verilator did not create its socket. See $LOG_DIR/verilator.log" >&2
  exit 1
fi

"$QEMU" -M "ibexdemo-soc-cosim,socket=$SOCKET" -nographic "$@" <&0 &
QPID=$!
set +e
wait "$QPID"
RESULT=$?
set -e
QPID=""
for ((attempt=0; attempt<40; attempt++)); do
  kill -0 "$VPID" 2>/dev/null || break
  sleep 0.05
done
exit "$RESULT"
