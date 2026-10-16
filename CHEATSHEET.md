# Ibex virtual platform — cheat sheet

Copy-paste in a terminal (`~/Ibex_verilator` = repo root).

## Environment (every new shell)

```bash
export ROOT="$HOME/Ibex_verilator"
export PATH="$ROOT/verilator/bin:$ROOT/toolchain/bin:$ROOT/local/bin:$HOME/.local/bin:$PATH"
export LD_LIBRARY_PATH="$ROOT/local/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
ELF="$ROOT/ibex-demo-system/sw/c/build/demo/hello_world/demo"
```

## Try it (after builds)

```bash
# 1) Fast QEMU (~seconds)
timeout 10 "$ROOT/qemu/build/qemu-system-riscv32" -M ibexdemo -nographic -kernel "$ELF"

# 2) Full RTL Verilator (UART in uart0.log; wait up to ~60s)
cd "$ROOT/ibex-demo-system/build/lowrisc_ibex_demo_system_0/sim-verilator"
timeout 60 ./Vtop_verilator --meminit=ram,"$ELF"; cat uart0.log

# 3) Cycle-accurate cosim (UART on console; first line ~30–60s)
"$ROOT/ibex-vp-cosim/run_cosim.sh"
```

**Good output:** `Hello World! 00000001   Input Value: 00000000` (timer hex counts up; GPIO often `00000000`).

## Build (once)

```bash
# Firmware
cd "$ROOT/ibex-demo-system/sw/c/build" && cmake .. && make demo

# QEMU
cd "$ROOT/qemu/build" && ninja qemu-system-riscv32

# Vtop_verilator (needs verilator on PATH)
cd "$ROOT/ibex-demo-system"
fusesoc --cores-root=. run --target=sim --tool=verilator --setup --build lowrisc:ibex:demo_system
```

Full steps: [README.md](README.md).

## Suggested GitHub repo description

```text
Ibex Demo virtual platform: QEMU ibexdemo, FuseSoC Verilator SoC, and cycle-accurate cosim (run_cosim.sh). See README.md / CHEATSHEET.md.
```
