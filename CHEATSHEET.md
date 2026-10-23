# Ibex demo commands (prepared Apple Silicon workspace)

From the repository root, in every terminal:

```bash
source ./env.sh
```

## Full-SoC QEMU + Verilator (main demo)

```bash
./ibex-vp-cosim/run_cosim.sh
```

Expected: `Hello World!` lines from the RTL CPU. Type `Z` to show the firmware
echoing UART input. Exit with **Ctrl+A, then X**; the launcher cleans up both
processes and the socket. The printed log directory contains `uart0.log` and
`verilator.log`.

To rebuild or verify:

```bash
./ibex-vp-cosim/build_full_soc.sh
python3 ibex-vp-cosim/scripts/test_full_soc.py
```

## QEMU only

```bash
"$QEMU" -M ibexdemo -nographic -kernel "$ELF"
```

Expected: `Hello World!` lines. Exit with **Ctrl+A, then X**.

## QEMU TCG CPU + Verilator GPIO/UART/timer

```bash
./ibex-vp-cosim/build_periph_cosim.sh
./ibex-vp-cosim/run_periph_cosim.sh
```

Expected: `Hello World!` lines. QEMU TCG executes firmware; GPIO, UART, and
timer MMIO go to Verilator RTL. Exit with **Ctrl+A, then X**.

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
All four modes were verified on this Mac. The full-SoC machine is
`ibexdemo-soc-cosim`; the GPIO-only machine is `ibexdemo-cosim`.

Build notes: [LOCAL_SETUP.md](LOCAL_SETUP.md). Full guide: [README.md](README.md).
