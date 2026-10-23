#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/env.sh"

copy_changed() {
  mkdir -p "$(dirname "$2")"
  if [[ ! -f "$2" ]] || ! cmp -s "$1" "$2"; then
    cp "$1" "$2"
  fi
}

PATCH="$ROOT/patches/0004-tcg-periph-cosim-qemu.patch"
if git -C "$ROOT/qemu" apply --reverse --check "$PATCH" 2>/dev/null; then
  :
elif git -C "$ROOT/qemu" apply --check "$PATCH" 2>/dev/null; then
  git -C "$ROOT/qemu" apply "$PATCH"
elif grep -q TYPE_IBEXDEMO_PERIPH_BRIDGE "$ROOT/qemu/hw/riscv/ibexdemo.c" 2>/dev/null; then
  echo "Skipping $(basename "$PATCH") (TCG periph machine already in tree)"
else
  echo "Cannot apply $(basename "$PATCH") in qemu" >&2
  git -C "$ROOT/qemu" apply --check "$PATCH"
  exit 1
fi

copy_changed "$ROOT/ibex-vp-cosim/include/cosim_protocol.h" \
  "$ROOT/qemu/include/hw/ibexdemo/cosim_protocol.h"
copy_changed "$ROOT/ibex-vp-cosim/periph/ibexdemo_periph_bridge.h" \
  "$ROOT/qemu/include/hw/ibexdemo/ibexdemo_periph_bridge.h"
copy_changed "$ROOT/ibex-vp-cosim/periph/ibexdemo_periph_bridge.c" \
  "$ROOT/qemu/hw/ibexdemo/ibexdemo_periph_bridge.c"

if [[ ! -f "$ROOT/qemu/build/build.ninja" ]]; then
  (cd "$ROOT/qemu/build" && ../configure --target-list=riscv32-softmmu \
    --without-default-features --enable-tcg --enable-tools --disable-rust)
fi
ninja -C "$ROOT/qemu/build" -j "${JOBS:-8}" qemu-system-riscv32
make -C "$ROOT/ibex-vp-cosim" periph -j "${JOBS:-8}"
echo "TCG peripheral cosim built. Run: $ROOT/ibex-vp-cosim/run_periph_cosim.sh"
