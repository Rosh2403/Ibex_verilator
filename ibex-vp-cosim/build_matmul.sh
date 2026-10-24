#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/env.sh"
N="${1:-8}"
SW="$ROOT/ibex-demo-system/sw/c"
OUT="$SW/build/demo/matmul/n${N}"
CC="${CC:-riscv32-unknown-elf-gcc}"
MARCH="-march=rv32imc -mabi=ilp32 -mcmodel=medany"
CFLAGS="$MARCH -Wall -fvisibility=hidden -ffreestanding -O2 -DN=${N}"
ASFLAGS="$MARCH"
LDFLAGS="-nostartfiles -T $SW/../common/link.ld"

mkdir -p "$OUT"
$CC $CFLAGS -I"$SW/common" -c "$ROOT/ibex-vp-cosim/sw/matmul/main.c" -o "$OUT/main.o"
$CC $CFLAGS -I"$SW/common" -c "$SW/common/demo_system.c" -o "$OUT/demo_system.o"
$CC $CFLAGS -I"$SW/common" -c "$SW/common/uart.c" -o "$OUT/uart.o"
$CC $ASFLAGS -I"$SW/common" -c "$SW/common/crt0.S" -o "$OUT/crt0.o"
$CC $LDFLAGS $CFLAGS -o "$OUT/matmul" "$OUT/crt0.o" "$OUT/demo_system.o" "$OUT/uart.o" "$OUT/main.o"
echo "Built $OUT/matmul"
