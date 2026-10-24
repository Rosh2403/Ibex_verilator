#!/usr/bin/env bash
# Build the TCG matmul plugin and guest ELF. Reconfigure QEMU only if plugins
# are missing. GLib lives in $ROOT/local/usr (see env.sh PKG_CONFIG_PATH).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/env.sh"
QEMU_SRC="$ROOT/qemu"
QEMU_BUILD="$QEMU_SRC/build"
PLUGIN_DIR="$ROOT/ibex-vp-cosim/tcg_plugin"
SW="$ROOT/ibex-demo-system/sw/c"
OUT="$SW/build/demo/matmul_accel"
CC="${CC:-riscv32-unknown-elf-gcc}"
HOSTCC="${HOSTCC:-gcc}"
N="${N:-4}"

need_qemu_plugins() {
  [[ ! -x "$QEMU_BUILD/qemu-system-riscv32" ]] && return 0
  [[ ! -f "$QEMU_BUILD/config-host.h" ]] && return 0
  grep -q '^#define CONFIG_PLUGIN' "$QEMU_BUILD/config-host.h" || return 0
  return 1
}

if need_qemu_plugins; then
  if ! pkg-config --exists glib-2.0; then
    echo "pkg-config cannot find glib-2.0. source ./env.sh first." >&2
    echo "Expected: $ROOT/local/usr/lib/x86_64-linux-gnu/pkgconfig/glib-2.0.pc" >&2
    exit 1
  fi
  (cd "$QEMU_BUILD" && "$QEMU_SRC/configure" --target-list=riscv32-softmmu \
    --without-default-features --enable-tcg --enable-tools --disable-rust \
    --enable-plugins)
  ninja -C "$QEMU_BUILD" -j "${JOBS:-8}" qemu-system-riscv32
fi

GLIB_CFLAGS="$(pkg-config --cflags glib-2.0)"
GLIB_LIBS="$(pkg-config --libs glib-2.0)"
$HOSTCC -shared -fPIC -fvisibility=hidden \
  $GLIB_CFLAGS \
  -I"$QEMU_SRC/include/qemu" \
  -I"$PLUGIN_DIR" \
  -o "$PLUGIN_DIR/matmul_plugin.so" \
  "$PLUGIN_DIR/matmul_plugin.c" \
  $GLIB_LIBS
echo "Built $PLUGIN_DIR/matmul_plugin.so"

MARCH="-march=rv32imc -mabi=ilp32 -mcmodel=medany"
CFLAGS="$MARCH -Wall -fvisibility=hidden -ffreestanding -O2 -DN=${N}"
ASFLAGS="$MARCH"
LDFLAGS="-nostartfiles -T $SW/../common/link.ld"
mkdir -p "$OUT"
$CC $CFLAGS -I"$SW/common" -I"$PLUGIN_DIR" \
  -c "$ROOT/ibex-vp-cosim/sw/matmul_accel/main.c" -o "$OUT/main.o"
$CC $CFLAGS -I"$SW/common" -c "$SW/common/demo_system.c" -o "$OUT/demo_system.o"
$CC $CFLAGS -I"$SW/common" -c "$SW/common/uart.c" -o "$OUT/uart.o"
$CC $ASFLAGS -I"$SW/common" -c "$SW/common/crt0.S" -o "$OUT/crt0.o"
$CC $LDFLAGS $CFLAGS -o "$OUT/matmul_accel" \
  "$OUT/crt0.o" "$OUT/demo_system.o" "$OUT/uart.o" "$OUT/main.o"
echo "Built $OUT/matmul_accel"
