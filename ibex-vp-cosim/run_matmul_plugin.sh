#!/usr/bin/env bash
# QEMU TCG CPU (ibexdemo) + TCG plugin host GEMM. No Verilator.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/env.sh"
bash "$ROOT/ibex-vp-cosim/build_matmul_plugin.sh"
ELF="$ROOT/ibex-demo-system/sw/c/build/demo/matmul_accel/matmul_accel"
PLUGIN="$ROOT/ibex-vp-cosim/tcg_plugin/matmul_plugin.so"

echo "QEMU TCG + matmul plugin (not Verilator RTL)."
timeout --signal=KILL 30 \
  "$QEMU" -M ibexdemo -nographic -kernel "$ELF" \
  -plugin "$PLUGIN" \
  </dev/null
