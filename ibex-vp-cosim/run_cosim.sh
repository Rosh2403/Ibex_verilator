#!/usr/bin/env bash
# Default simulator: QEMU TCG CPU + Verilator GPIO/UART/timer.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
exec "$ROOT/ibex-vp-cosim/run_periph_cosim.sh" "$@"
