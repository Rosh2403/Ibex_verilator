#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
QEMU="$ROOT/qemu/build/qemu-system-riscv32"
VSIM="$ROOT/ibex-demo-system/build/lowrisc_ibex_demo_system_0/sim-verilator/Vtop_verilator"
SOCKET="${IBEXDEMO_COSIM_SOCKET:-/tmp/ibexdemo-verilator.sock}"
ELF="${1:-$ROOT/ibex-demo-system/sw/c/build/demo/hello_world/demo}"

export LD_LIBRARY_PATH="${ROOT}/local/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export PATH="${ROOT}/local/bin:${ROOT}/toolchain/bin:${HOME}/.local/bin:$PATH"

if [[ ! -x "$VSIM" ]]; then
  echo "Build full SoC Verilator sim (FuseSoC), then rebuild if you added qemu-cosim:"
  echo "  cd $ROOT/ibex-demo-system"
  echo "  fusesoc --cores-root=. run --target=sim --tool=verilator --setup --build lowrisc:ibex:demo_system"
  exit 1
fi
if [[ ! -x "$QEMU" ]]; then
  echo "Build QEMU: ninja -C $ROOT/qemu/build qemu-system-riscv32"
  exit 1
fi
if [[ ! -f "$ELF" ]]; then
  echo "Missing ELF: $ELF"
  exit 1
fi

rm -f "$SOCKET"
"$VSIM" --meminit=ram,"$ELF" --qemu-cosim-socket="$SOCKET" &
VPID=$!
cleanup() {
  kill "$VPID" 2>/dev/null || true
  wait "$VPID" 2>/dev/null || true
  rm -f "$SOCKET"
}
trap cleanup EXIT

for _ in $(seq 1 200); do
  if [[ -S "$SOCKET" ]]; then
    sleep 0.5
    break
  fi
  sleep 0.1
done

exec "$QEMU" -M ibexdemo-cosim -nographic -serial mon:stdio "${@:2}"
