# Ibex Demo virtual platform (QEMU + Verilator)

Run the [lowRISC Ibex Demo System](https://github.com/lowRISC/ibex-demo-system) on your
machine as a **virtual platform**: same bare-metal `hello_world` firmware, three ways
to simulate it.

| Mode | Command (after build) | What it is |
|------|------------------------|------------|
| **1. QEMU** | `qemu-system-riscv32 -M ibexdemo` | Fast functional CPU + peripherals (try this first) |
| **2. Verilator** | `Vtop_verilator --meminit=ram,<elf>` | Cycle-accurate full SoC RTL (slow; UART in `uart0.log`) |
| **3. Cosim** | `ibex-vp-cosim/run_cosim.sh` | Same RTL as (2); QEMU only provides serial console |

> **Cheat sheet:** [CHEATSHEET.md](CHEATSHEET.md) — env + three run commands on one page.

### One-liner try (QEMU, after build)

```bash
export ROOT="$HOME/Ibex_verilator" PATH="$ROOT/verilator/bin:$ROOT/toolchain/bin:$ROOT/local/bin:$HOME/.local/bin:$PATH" LD_LIBRARY_PATH="$ROOT/local/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" && timeout 10 "$ROOT/qemu/build/qemu-system-riscv32" -M ibexdemo -nographic -kernel "$ROOT/ibex-demo-system/sw/c/build/demo/hello_world/demo"
```

## Quick start (try the simulator)

Assumes this repo lives at `~/Ibex_verilator` and you already built QEMU and the demo ELF
(see [Build once](#build-once) if not).

```bash
export ROOT="$HOME/Ibex_verilator"
export PATH="$ROOT/verilator/bin:$ROOT/toolchain/bin:$ROOT/local/bin:$HOME/.local/bin:$PATH"
export LD_LIBRARY_PATH="$ROOT/local/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

ELF="$ROOT/ibex-demo-system/sw/c/build/demo/hello_world/demo"

# --- Fastest: QEMU Ibex (seconds) ---
timeout 10 "$ROOT/qemu/build/qemu-system-riscv32" \
  -M ibexdemo -nographic -kernel "$ELF"
```

You should see repeating lines like:

```text
Hello World! 00000001   Input Value: 00000000
Hello World! 00000002   Input Value: 00000000
```

**What the numbers mean**

| Part | Meaning |
|------|---------|
| `00000001`, `00000002`, … | Hex **timer tick** from the demo firmware (not CPU cycles). |
| `Input Value: 00000000` | Hex **GPIO inputs**; often zero in sim unless pins are driven. |

Press Ctrl+C to stop (or wait for `timeout`).

**Cycle-accurate run (needs `Vtop_verilator` built):**

```bash
"$ROOT/ibex-vp-cosim/run_cosim.sh"
```

First `Hello World!` can take **30–60 seconds** (RTL is slow). Output appears on the
same terminal. Stop with Ctrl+C.

---

## Repository layout

```text
Ibex_verilator/
  CHEATSHEET.md       # Copy-paste env + run commands
  ibex-demo-system/   # SoC RTL, FuseSoC build, demo software
  qemu/               # QEMU with ibexdemo + ibexdemo-cosim machines
  verilator/          # Verilator compiler (add verilator/bin to PATH)
  ibex-vp-cosim/      # run_cosim.sh and cosim protocol headers
  toolchain/          # riscv32-unknown-elf-gcc (unpack lowRISC toolchain here)
  local/              # Optional: extra libs/tools from local bootstrap
```

---

## Prerequisites

- **RISC-V GCC**: `riscv32-unknown-elf-gcc` in `toolchain/bin/`
- **CMake** + **make**: demo firmware (`~/.local/bin` is fine for `cmake`)
- **Verilator** on `PATH`: use `$ROOT/verilator/bin` after building from `verilator/`
- **QEMU build**: Python 3, **ninja**, **meson** (QEMU’s `build/pyvenv` if needed)
- **Full RTL sim**: FuseSoC + Python deps from `ibex-demo-system/python-requirements.txt`

---

## Build once

Set paths for every build terminal:

```bash
export ROOT="$HOME/Ibex_verilator"
export PATH="$ROOT/verilator/bin:$ROOT/toolchain/bin:$ROOT/local/bin:$HOME/.local/bin:$PATH"
export LD_LIBRARY_PATH="$ROOT/local/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
```

### 1. Demo firmware (required for all modes)

```bash
cd "$ROOT/ibex-demo-system"
mkdir -p sw/c/build && cd sw/c/build
cmake .. && make demo
```

ELF:

```text
$ROOT/ibex-demo-system/sw/c/build/demo/hello_world/demo
```

### 2. QEMU (required for modes 1 and 3)

```bash
cd "$ROOT/qemu"
mkdir -p build && cd build
../configure --target-list=riscv32-softmmu --without-default-features --enable-tcg --enable-tools
ninja qemu-system-riscv32
```

Binary: `$ROOT/qemu/build/qemu-system-riscv32`

If `configure` fails on the Python venv, see notes in `qemu/python/scripts/mkvenv.py`.

### 3. Verilator full SoC (required for modes 2 and 3)

```bash
cd "$ROOT/ibex-demo-system"
fusesoc --cores-root=. run --target=sim --tool=verilator --setup --build lowrisc:ibex:demo_system
```

Binary:

```text
$ROOT/ibex-demo-system/build/lowrisc_ibex_demo_system_0/sim-verilator/Vtop_verilator
```

This tree’s `ibex_demo_system.core` already passes **`-Wno-UNOPTFLAT`** to Verilator.
If an older `config.mk` still fails on `UNOPTFLAT`, add that flag there and run:

```bash
make -C "$ROOT/ibex-demo-system/build/lowrisc_ibex_demo_system_0/sim-verilator" -j"$(nproc)"
```

`verible-verilog-syntax` missing during FuseSoC setup is OK (regex fallback).

---

## Run and verify

Use the same `ROOT`, `PATH`, `LD_LIBRARY_PATH`, and `ELF` as in [Quick start](#quick-start-try-the-simulator).

### Mode 1 — QEMU only (recommended first test)

```bash
timeout 10 "$ROOT/qemu/build/qemu-system-riscv32" \
  -M ibexdemo -nographic -kernel "$ELF"
```

**Pass:** `Hello World!` lines with increasing timer hex and `Input Value:` on the console.

### Mode 2 — Verilator only (full RTL)

```bash
SIM="$ROOT/ibex-demo-system/build/lowrisc_ibex_demo_system_0/sim-verilator"
cd "$SIM"
rm -f uart0.log
timeout 60 ./Vtop_verilator --meminit=ram,"$ELF"
cat uart0.log
```

**Pass:** `uart0.log` contains `Hello World!` / `Input Value` (may take up to ~60s wall time).
Startup text on the terminal (`Simulation running…`) is from the testbench, not the firmware.

### Mode 3 — Cycle-accurate cosim (RTL + QEMU console)

Starts `Vtop_verilator` with `--qemu-cosim-socket` and QEMU `-M ibexdemo-cosim`
(QEMU does **not** emulate the CPU; it steps Verilator and forwards UART).

```bash
"$ROOT/ibex-vp-cosim/run_cosim.sh"
# optional: different ELF or extra QEMU args
# "$ROOT/ibex-vp-cosim/run_cosim.sh" "$ELF"
```

**Pass:** same UART lines as mode 1 on the console; first line may take 30–60s.

```text
  QEMU (-M ibexdemo-cosim)              Vtop_verilator (full SoC)
  serial / -nographic        <---UART---  stepped via /tmp/ibexdemo-verilator.sock
```

More detail: [ibex-vp-cosim/README.md](ibex-vp-cosim/README.md).

### Optional — GPIO socket smoke test (developers)

Legacy GPIO-only Verilator server (not used by `run_cosim.sh`):

```bash
make -C "$ROOT/ibex-vp-cosim"
cd "$ROOT/ibex-vp-cosim"
rm -f /tmp/ibexdemo-cosim.sock
./build/Vcosim_gpio_server --socket /tmp/ibexdemo-cosim.sock &
sleep 0.3
python3 scripts/test_gpio_client.py
kill %1 2>/dev/null
```

**Pass:** `GPIO OUT readback: 0x0000beef`.

---

## Troubleshooting

| Symptom | Likely fix |
|---------|------------|
| `verilator: not found` during FuseSoC | `export PATH="$ROOT/verilator/bin:$PATH"` before `fusesoc`. |
| `cmake: not found` | `export PATH="$HOME/.local/bin:$PATH"` or install CMake. |
| FuseSoC `UNOPTFLAT` / exit code 4 | Re-run setup with current `ibex_demo_system.core`, or add `-Wno-UNOPTFLAT` to `sim-verilator/config.mk` and `make` there. |
| QEMU fault at boot | QEMU build should use 128 KiB RAM (`SRAM_MAIN_SIZE 0x20000` in `qemu/hw/riscv/ibexdemo.c`). |
| Cosim: `verilator iface: connect` | Start `run_cosim.sh` (starts Verilator first); check `/tmp/ibexdemo-verilator.sock`. |
| Cosim: no UART for a long time | Normal; wait 30–60s for first `Hello World!`. |
| Verilator: empty `uart0.log` after a few seconds | Use `timeout 60` or longer; read `uart0.log` in the sim directory. |
| Missing `libelf` / `lz4` for `Vtop_verilator` | Use libs under `local/usr` and `LD_LIBRARY_PATH` as above. |
| FuseSoC / `mako` errors | `pip install -r ibex-demo-system/python-requirements.txt` |

---

## Optional: FST traces (Verilator)

```bash
cd "$ROOT/ibex-demo-system/build/lowrisc_ibex_demo_system_0/sim-verilator"
./Vtop_verilator --meminit=ram,"$ELF" -t
# then open sim.fst with GTKWave
```
