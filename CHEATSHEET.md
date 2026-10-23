# Ibex demo commands (prepared Apple Silicon workspace)

From the repository root, in every terminal:

```bash
source ./env.sh
```

## Default simulator: QEMU TCG CPU + Verilator GPIO/UART/timer

```bash
./ibex-vp-cosim/build_periph_cosim.sh
./ibex-vp-cosim/run_cosim.sh
```

QEMU TCG executes firmware; GPIO, UART, and timer are Verilator RTL.
Expected: `Hello World!` lines. Exit with **Ctrl+A, then X**.

8x8 matrix multiply (prints `mcycle`):

```bash
./ibex-vp-cosim/run_matmul.sh
```

That also runs the same ELF on standalone full-SoC Verilator for Ibex RTL
cycle counts.

## QEMU only

```bash
"$QEMU" -M ibexdemo -nographic -kernel "$ELF"
```

Expected: `Hello World!` lines. Exit with **Ctrl+A, then X**.

## Full-SoC QEMU + Verilator (RTL CPU)

```bash
./ibex-vp-cosim/build_full_soc.sh
./ibex-vp-cosim/run_full_soc.sh
```

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
The default machine is `ibexdemo-cosim` (TCG CPU + RTL peripherals).
Full-SoC RTL is `ibexdemo-soc-cosim`.

Build notes: [LOCAL_SETUP.md](LOCAL_SETUP.md). Full guide: [README.md](README.md).
