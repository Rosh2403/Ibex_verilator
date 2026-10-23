#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/env.sh"

apply_once() {
  local tree="$1" patch="$2"
  if git -C "$tree" apply --reverse --check "$patch" 2>/dev/null; then
    return
  fi
  if git -C "$tree" apply --check "$patch" 2>/dev/null; then
    git -C "$tree" apply "$patch"
    return
  fi
  if [[ "$(basename "$patch")" == "0001-"* ]] &&
     [[ -f "$tree/hw/ibexdemo/ibexdemo_cosim_bridge.c" ]]; then
    echo "Skipping $(basename "$patch") (GPIO cosim bridge already in tree)"
    return
  fi
  if [[ "$(basename "$patch")" == "0003-"* ]] &&
     [[ -f "$tree/dv/verilator/soc_cosim.h" ]] &&
     grep -q uartdpi_set_cosim_callbacks "$tree/vendor/lowrisc_ip/dv/dpi/uartdpi/uartdpi.h" 2>/dev/null; then
    echo "Skipping $(basename "$patch") (Verilator cosim hooks already in tree)"
    return
  fi
  echo "Cannot apply $(basename "$patch") in $tree" >&2
  git -C "$tree" apply --check "$patch"
  exit 1
}

copy_changed() {
  if ! cmp -s "$1" "$2"; then
    cp "$1" "$2"
  fi
}

for tree in qemu ibex-demo-system; do
  if [[ ! -d "$ROOT/$tree/.git" ]]; then
    echo "Missing source tree: $ROOT/$tree" >&2
    exit 1
  fi
done

apply_once "$ROOT/qemu" "$ROOT/patches/0001-Add-ibexdemo-cosim-machine-and-Verilator-MMIO-bridge.patch"
apply_once "$ROOT/qemu" "$ROOT/patches/0002-full-soc-qemu-build.patch"
apply_once "$ROOT/ibex-demo-system" "$ROOT/patches/0003-full-soc-verilator-hooks.patch"

copy_changed "$ROOT/ibex-vp-cosim/full_soc/ibexdemo_soc_cosim.c" "$ROOT/qemu/hw/riscv/ibexdemo_soc_cosim.c"
copy_changed "$ROOT/ibex-vp-cosim/include/cosim_soc_protocol.h" "$ROOT/qemu/hw/riscv/cosim_soc_protocol.h"
copy_changed "$ROOT/ibex-vp-cosim/full_soc/soc_cosim.h" "$ROOT/ibex-demo-system/dv/verilator/soc_cosim.h"
copy_changed "$ROOT/ibex-vp-cosim/include/cosim_soc_protocol.h" "$ROOT/ibex-demo-system/dv/verilator/cosim_soc_protocol.h"

mkdir -p "$ROOT/qemu/build"
if [[ ! -f "$ROOT/qemu/build/build.ninja" ]]; then
  (cd "$ROOT/qemu/build" && ../configure --target-list=riscv32-softmmu \
    --without-default-features --enable-tcg --enable-tools --disable-rust)
fi
ninja -C "$ROOT/qemu/build" -j "${JOBS:-8}" qemu-system-riscv32

(cd "$ROOT/ibex-demo-system" && fusesoc --cores-root=. run --target=sim \
  --tool=verilator --setup lowrisc:ibex:demo_system)

SIM="$ROOT/ibex-demo-system/build/lowrisc_ibex_demo_system_0/sim-verilator"
export IBEX_LOCAL_ENV="$ROOT/local/env"
if [[ -f "$IBEX_LOCAL_ENV/lib/libelf.a" ]]; then
  cat >> "$SIM/config.mk" <<'CONFIG'
VERILATOR_OPTIONS += -CFLAGS "-I$(IBEX_LOCAL_ENV)/include -I$(IBEX_LOCAL_ENV)/include/libelf"
VERILATOR_OPTIONS += -LDFLAGS "-L$(IBEX_LOCAL_ENV)/lib -Wl,-rpath,$(IBEX_LOCAL_ENV)/lib -llz4"
CONFIG
fi
make -B -C "$SIM" -j "${JOBS:-8}"
echo "Full-SoC integration built. Run: $ROOT/ibex-vp-cosim/run_cosim.sh"
