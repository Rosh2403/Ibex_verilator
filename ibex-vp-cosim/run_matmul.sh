#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/env.sh"
bash "$ROOT/ibex-vp-cosim/build_matmul.sh"
ELF="$ROOT/ibex-demo-system/sw/c/build/demo/matmul/matmul"
VSIM="$ROOT/ibex-demo-system/build/lowrisc_ibex_demo_system_0/sim-verilator/Vtop_verilator"

echo "=== TCG CPU + RTL peripherals (default simulator) ==="
export IBEXDEMO_COSIM_SOCKET="${IBEXDEMO_COSIM_SOCKET:-/tmp/ibexdemo-matmul.sock}"
timeout --signal=KILL 90 "$ROOT/ibex-vp-cosim/run_periph_cosim.sh" "$ELF" \
  </dev/null || true

if [[ -x "$VSIM" ]]; then
  echo
  echo "=== Full-SoC Verilator (Ibex RTL mcycle) ==="
  LOG=$(mktemp -d /tmp/ibex-matmul.XXXXXX)
  (cd "$LOG" && timeout --signal=KILL 60 "$VSIM" --meminit=ram,"$ELF") \
    >"$LOG/sim.log" 2>&1 || true
  if [[ -s "$LOG/uart0.log" ]]; then
    cat "$LOG/uart0.log"
  fi
  rg -n "Executed cycles|Instructions Retired|^Cycles:" "$LOG/sim.log" || cat "$LOG/sim.log"
  echo "Verilator log dir: $LOG"
else
  echo "Skip RTL cycle run: $VSIM not built. Run ibex-vp-cosim/build_full_soc.sh." >&2
fi
