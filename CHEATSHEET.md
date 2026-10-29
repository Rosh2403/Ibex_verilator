# Ibex demo commands (prepared Apple Silicon workspace)

From the repository root, in every terminal:

```bash
source ./env.sh
```

## Matmul accelerator daemon (QEMU TCG + plugin)

```bash
./ibex-vp-cosim/run_matmul_plugin.sh
```

QEMU TCG runs the Ibex firmware. A store to `0x0011C014` is caught by
`build/matmul_plugin.so`, which sends `A` and `B` to the matmul accelerator
daemon (a separate process on `/tmp/ibex-matmul-accel.sock`) and writes `C`
back. Expected: four `ok checksum` lines, `rejected`, `PASS`, then the daemon's
log and `Result: PASS`. Not cycle-accurate.

Show the daemon in its own terminal:

```bash
./ibex-vp-cosim/matmul_daemon.sh foreground   # terminal A, Ctrl+C stops
./ibex-vp-cosim/run_matmul_plugin.sh          # terminal B, repeat freely
```

Background daemon:

```bash
./ibex-vp-cosim/matmul_daemon.sh start        # detached
./ibex-vp-cosim/matmul_daemon.sh status       # pid, GEMMs served
./ibex-vp-cosim/matmul_daemon.sh log -f       # follow its log
./ibex-vp-cosim/matmul_daemon.sh stop
```

Debug with GDB:

```bash
./ibex-vp-cosim/run_matmul_plugin.sh --gdb    # terminal A: VP paused, GDB port 1234
riscv32-unknown-elf-gdb ibex-vp-cosim/build/matmul_accel.elf -ex 'target remote :1234'   # terminal B
```

Compare the daemon with the direct (in-QEMU) version:

```bash
python3 ibex-vp-cosim/scripts/bench_npu.py --npu-only   # time per request, split by phase
./ibex-vp-cosim/run_matmul_plugin.sh --direct           # demo with the math inside QEMU
```

Tests: `python3 ibex-vp-cosim/scripts/test_matmul_daemon.py` (24 tests).

## Full-SoC QEMU + Verilator (RTL CPU)

```bash
./ibex-vp-cosim/run_full_soc.sh
```

Expected: `Hello World!` lines from the RTL CPU; type `Z` to see the UART echo.
Exit with **Ctrl+A, then X**. Rebuild with `./ibex-vp-cosim/build_full_soc.sh`.
Tests: `python3 ibex-vp-cosim/scripts/test_full_soc.py` (7 tests).

## Matrix multiply on Ibex RTL (prints `mcycle`)

```bash
./ibex-vp-cosim/run_matmul.sh        # 8x8; pass 16, 32, ... for other sizes
```

Runs on standalone full-SoC Verilator for Ibex RTL cycle counts.

## QEMU only

```bash
"$QEMU" -M ibexdemo -nographic -kernel "$ELF"
```

Expected: `Hello World!` lines. Exit with **Ctrl+A, then X**.

## Standalone full-SoC Verilator

```bash
cd "$ROOT/ibex-demo-system/build/lowrisc_ibex_demo_system_0/sim-verilator"
./Vtop_verilator --meminit=ram,"$ELF"
```

In another terminal:

```bash
tail -F "$ROOT/ibex-demo-system/build/lowrisc_ibex_demo_system_0/sim-verilator/uart0.log"
```

Expected: `Hello World!` in `uart0.log`. Stop with Ctrl+C.

## QEMU TCG CPU + Verilator GPIO/UART/timer (not built on this Mac)

```bash
./ibex-vp-cosim/build_periph_cosim.sh
./ibex-vp-cosim/run_cosim.sh
```

`patches/0004-tcg-periph-cosim-qemu.patch` does not yet apply to the installed
QEMU tree, so this mode is not available here.

## What is running?

```bash
./ibex-vp-cosim/matmul_daemon.sh status
pgrep -fl 'matmul_daemon|Vtop_verilator|Vcosim_|qemu-system-riscv32'
```

Build notes: [LOCAL_SETUP.md](LOCAL_SETUP.md). Full guide: [README.md](README.md).
