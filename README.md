# Ibex Demo virtual platform (QEMU + Verilator)

This workspace runs the [lowRISC Ibex Demo System](https://github.com/lowRISC/ibex-demo-system) in three ways:

| Mode | What runs | Best for |
|------|-----------|----------|
| **QEMU only** | Fast functional `ibexdemo` machine | Quick software bring-up |
| **Verilator only** | Cycle-accurate full SoC RTL | RTL debug, waves |
| **Hybrid cosim** | QEMU CPU + Verilator GPIO RTL | Fast CPU with real GPIO peripheral |

All modes use the same bare-metal ELF (`hello_world` demo).

## Repository layout

```
Ibex_verilator/
  ibex-demo-system/   # SoC RTL and demo software
  qemu/               # lowRISC QEMU (ibexdemo + ibexdemo-cosim)
  verilator/          # Verilator compiler sources
  ibex-vp-cosim/      # GPIO cosim server and run_cosim.sh
  local/              # Optional: locally built tools/libs (if you used the bootstrap flow)
  toolchain/          # Optional: riscv32-unknown-elf GCC tarball
```

## Prerequisites

- **RISC-V toolchain**: `riscv32-unknown-elf-gcc` (e.g. [lowRISC toolchain](https://github.com/lowRISC/lowrisc-toolchains/releases) unpacked to `toolchain/`)
- **CMake** and **make** (to build `hello_world`)
- **Python 3** with lowRISC FuseSoC stack (only for full Verilator SoC build)
- **Verilator** on `PATH` (build from `verilator/` or use `local/bin`)
- **ninja** + **meson** (inside QEMU’s build venv) for QEMU

## Build once

### 1. Demo firmware

```bash
export PATH="/path/to/Ibex_verilator/toolchain/bin:$PATH"

cd ibex-demo-system
mkdir -p sw/c/build && cd sw/c/build
cmake .. && make demo
```

ELF path (used below):

```text
ibex-demo-system/sw/c/build/demo/hello_world/demo
```

### 2. QEMU (`ibexdemo` and `ibexdemo-cosim`)

```bash
cd qemu
# See qemu/python/scripts/mkvenv.py notes if configure fails on python venv.
mkdir -p build && cd build
../configure --target-list=riscv32-softmmu --without-default-features --enable-tcg --enable-tools
ninja qemu-system-riscv32
```

Binary: `qemu/build/qemu-system-riscv32`

### 3. Verilator full-system simulator (optional)

```bash
cd ibex-demo-system
# Use lowRISC FuseSoC from python-requirements.txt if fusesoc fails.
fusesoc --cores-root=. run --target=sim --tool=verilator --setup --build lowrisc:ibex:demo_system
```

If Verilator exits on `UNOPTFLAT` warnings, add `-Wno-UNOPTFLAT` to  
`build/lowrisc_ibex_demo_system_0/sim-verilator/config.mk` and run `make` in that directory.

Binary:  
`ibex-demo-system/build/lowrisc_ibex_demo_system_0/sim-verilator/Vtop_verilator`

### 4. Hybrid cosim GPIO server

```bash
make -C ibex-vp-cosim
```

Binary: `ibex-vp-cosim/build/Vcosim_gpio_server`

---

## Run and verify

Set library path if you installed deps under `local/`:

```bash
export ROOT=/path/to/Ibex_verilator
export LD_LIBRARY_PATH="$ROOT/local/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export PATH="$ROOT/toolchain/bin:$ROOT/local/bin:$PATH"
ELF="$ROOT/ibex-demo-system/sw/c/build/demo/hello_world/demo"
```

### A. QEMU only (fastest check)

```bash
timeout 5 "$ROOT/qemu/build/qemu-system-riscv32" \
  -M ibexdemo -nographic -kernel "$ELF"
```

**Working:** repeating lines on the console, for example:

```text
Hello World! 00000001   Input Value: 00000000
Hello World! 00000002   Input Value: 00000000
```

Press Ctrl+C to stop. `timeout` ends the demo after a few seconds.

### B. Verilator full SoC

```bash
cd "$ROOT/ibex-demo-system/build/lowrisc_ibex_demo_system_0/sim-verilator"
rm -f uart0.log
timeout 5 ./Vtop_verilator --meminit=ram,"$ELF"
cat uart0.log
```

**Working:** `uart0.log` contains the same `Hello World!` / `Input Value` text.  
UART is also attached to a pseudo-TTY; the simulator prints its path at startup.

### C. Hybrid QEMU + Verilator (GPIO in RTL)

One command starts the Verilator socket server and QEMU:

```bash
"$ROOT/ibex-vp-cosim/run_cosim.sh"
```

Or manually:

```bash
rm -f /tmp/ibexdemo-cosim.sock
"$ROOT/ibex-vp-cosim/build/Vcosim_gpio_server" --socket /tmp/ibexdemo-cosim.sock &
sleep 0.5
timeout 5 "$ROOT/qemu/build/qemu-system-riscv32" \
  -M ibexdemo-cosim -nographic -kernel "$ELF"
kill %1 2>/dev/null
```

**Working:** same `Hello World!` lines on the console.  
**Cosim-specific:** `Input Value` changes (e.g. `00000010`, `00000020`, …) because GPIO reads come from Verilated `gpio.sv`, not QEMU’s GPIO model.

Compare with **A** (QEMU-only): input values often stay `00000000`.

### D. GPIO bridge smoke test (no CPU)

Verifies the socket protocol and Verilator GPIO without QEMU:

```bash
cd "$ROOT/ibex-vp-cosim"
rm -f /tmp/ibexdemo-cosim.sock
./build/Vcosim_gpio_server --socket /tmp/ibexdemo-cosim.sock &
sleep 0.3
python3 scripts/test_gpio_client.py
kill %1 2>/dev/null
```

**Working:**

```text
GPIO OUT readback: 0x0000beef
```

---

## Architecture (hybrid mode)

```text
  QEMU (ibexdemo-cosim)                    Verilator
  ---------------------                    ---------
  Ibex CPU, RAM, UART, timer  --MMIO-->   gpio.sv @ 0x8000_0000
                    Unix socket /tmp/ibexdemo-cosim.sock
```

Details: [ibex-vp-cosim/README.md](ibex-vp-cosim/README.md) and [ibex-vp-cosim/docs/extend_pwm_irq.md](ibex-vp-cosim/docs/extend_pwm_irq.md).

## Troubleshooting

| Symptom | Likely fix |
|---------|------------|
| QEMU store fault at boot | Ensure QEMU build includes 128 KiB RAM (`SRAM_MAIN_SIZE 0x20000` in `qemu/hw/riscv/ibexdemo.c`). |
| `cosim bridge: connect` failed | Start `Vcosim_gpio_server` before QEMU, or use `run_cosim.sh`. |
| Verilator sim missing `libelf` / `lz4` | Install dev packages or use libs under `local/usr`. |
| FuseSoC / `mako` errors | `pip install` packages from `ibex-demo-system/python-requirements.txt`. |
| No UART output in Verilator sim | Read `uart0.log` in the sim working directory. |

## Optional: slower QEMU for debugging

```bash
./ibex-vp-cosim/run_cosim.sh "$ELF" -icount shift=7
```
