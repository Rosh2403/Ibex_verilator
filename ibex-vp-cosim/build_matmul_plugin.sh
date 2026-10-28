#!/usr/bin/env bash
# Build the matmul accelerator daemon, the QEMU TCG plugin that talks to it,
# and the Ibex firmware that kicks it. Reconfigures QEMU with --enable-plugins
# (keeping its existing options) only if the current build lacks plugins.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/env.sh"
QEMU_SRC="$ROOT/qemu"
QEMU_BUILD="$QEMU_SRC/build"

qemu_configured_with_plugins() {
  grep -q '^#define CONFIG_PLUGIN' "$QEMU_BUILD/config-host.h" 2>/dev/null
}

if ! qemu_configured_with_plugins; then
  if ! pkg-config --exists glib-2.0; then
    echo "pkg-config cannot find glib-2.0; source ./env.sh first." >&2
    exit 1
  fi
  mkdir -p "$QEMU_BUILD"
  if [[ -x "$QEMU_BUILD/config.status" ]]; then
    echo "Reconfiguring QEMU with TCG plugin support (one-time full rebuild)..."
    # config.status re-runs configure with the original options plus ours.
    (cd "$QEMU_BUILD" && ./config.status --enable-plugins)
  else
    echo "Configuring QEMU with TCG plugin support..."
    (cd "$QEMU_BUILD" && ../configure --target-list=riscv32-softmmu \
      --without-default-features --enable-tcg --enable-tools --disable-rust \
      --enable-plugins)
  fi
fi
# Incremental: a no-op when QEMU is up to date, a full build the first time.
ninja -C "$QEMU_BUILD" -j "${JOBS:-8}" qemu-system-riscv32

make -s -C "$ROOT/ibex-vp-cosim" matmul
B="$ROOT/ibex-vp-cosim/build"
echo "Built $B/matmul_daemon, $B/matmul_plugin.so, $B/matmul_accel.elf"
