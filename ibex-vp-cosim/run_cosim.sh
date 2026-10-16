#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
COSIM_DIR="$ROOT/ibex-vp-cosim"
QEMU="$ROOT/qemu/build/qemu-system-riscv32"
VERILATOR_SERVER="$COSIM_DIR/build/Vcosim_gpio_server"
SOCKET="${IBEXDEMO_COSIM_SOCKET:-/tmp/ibexdemo-cosim.sock}"
ELF="${1:-$ROOT/ibex-demo-system/sw/c/build/demo/hello_world/demo}"

export LD_LIBRARY_PATH="${ROOT}/local/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export PATH="${ROOT}/local/bin:${ROOT}/toolchain/bin:${HOME}/.local/bin:$PATH"

if [[ ! -x "$VERILATOR_SERVER" ]]; then
  echo "Build Verilator cosim server: make -C $COSIM_DIR"
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
"$VERILATOR_SERVER" --socket "$SOCKET" &
VPID=$!
cleanup() {
  kill "$VPID" 2>/dev/null || true
  wait "$VPID" 2>/dev/null || true
  rm -f "$SOCKET"
}
trap cleanup EXIT

for _ in $(seq 1 50); do
  [[ -S "$SOCKET" ]] && break
  sleep 0.1
done

exec "$QEMU" -M ibexdemo-cosim -nographic -kernel "$ELF" "${@:2}"
