#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/env.sh"
N="${1:-8}"
bash "$ROOT/ibex-vp-cosim/build_matmul.sh" "$N"
ELF="$ROOT/ibex-demo-system/sw/c/build/demo/matmul/n${N}/matmul"
VSIM="$ROOT/ibex-demo-system/build/lowrisc_ibex_demo_system_0/sim-verilator/Vtop_verilator"

if [[ ! -x "$VSIM" ]]; then
  echo "Skip RTL cycle run: $VSIM not built. Run ibex-vp-cosim/build_full_soc.sh." >&2
  exit 1
fi

echo "=== Full-SoC Verilator ${N}x${N} ==="
LOG=$(mktemp -d /tmp/ibex-matmul-n${N}.XXXXXX)
(cd "$LOG" && timeout --signal=KILL 90 "$VSIM" --meminit=ram,"$ELF") \
  >"$LOG/sim.log" 2>&1 || true
if [[ -s "$LOG/uart0.log" ]]; then
  cat "$LOG/uart0.log"
fi
rg -n "Executed cycles|Instructions Retired|^Cycles:" "$LOG/sim.log" || cat "$LOG/sim.log"
echo "Verilator log dir: $LOG"
