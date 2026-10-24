# Ibex Demo System: QEMU + Verilator

Run every command from the **repository root** (`~/Ibex_verilator`), not from
`verilator/`:

```bash
cd ~/Ibex_verilator
source ./env.sh
```

The source trees, native tools, firmware, and simulator binaries may already be
built in a prepared workspace. For another checkout, see the dependency section
below.

## Default simulator (QEMU TCG CPU + Verilator peripherals)

This is the **normal demo path** (`ibexdemo-cosim`):

- **CPU and RAM:** QEMU TCG (fast functional Ibex).
- **GPIO, UART, timer:** cycle-accurate Verilator RTL in `Vcosim_periph_server`.
- **Link:** two processes on a **Unix socket** (`cosim_protocol.h`), not a single
  linked binary.

Build once, then launch:

```bash
./ibex-vp-cosim/build_periph_cosim.sh
./ibex-vp-cosim/run_cosim.sh
```

Expected: `Hello World!` on the UART console. Exit with **Ctrl+A, then X**.

QEMU TCG executes firmware; MMIO to `0x80000000` / `0x80001000` / `0x80002000`
is forwarded to Verilator. Timer IRQs and UART bytes cross the socket. Guest
`mcycle` on this path is **QEMU's counter**, not Ibex RTL pipeline time.

Equivalent launcher (same machine): `./ibex-vp-cosim/run_periph_cosim.sh`.

Socket override: `IBEXDEMO_COSIM_SOCKET=/tmp/my.sock ./ibex-vp-cosim/run_cosim.sh`.

## TCG plugin matmul (host accelerator, no Verilator)

A **QEMU TCG plugin** (`ibex-vp-cosim/tcg_plugin/matmul_plugin.so`) intercepts a
guest store to doorbell **`0x0011C014`**. The plugin reads matrix pointers from a
small RAM mailbox (`matmul_accel.h`), runs **4x4 integer GEMM on the host**, writes
`C` back, and sets a done flag. Guest firmware is in `sw/matmul_accel/`.

This is for **offload / hook demos**. It is not cycle-accurate and does not use
Verilator. QEMU must be built with plugins (`build_matmul_plugin.sh` handles that
when needed).

```bash
./ibex-vp-cosim/run_matmul_plugin.sh
```

Expected console:

```text
[matmul plugin] loaded; kick store at 0x0011C014
guest: kick host matmul
[matmul plugin] host 4x4 GEMM  A@... B@... -> C@...
guest: C sum 000003B8 expect 000003B8
PASS
```

For a real MMIO accelerator (always present without `-plugin`), use a QEMU
SysBus device (same idea as the peripheral bridge) or RTL in Verilator.

## Matrix multiply on Ibex RTL (Verilator)

Use this path to measure **Ibex `mcycle`**, executed cycles, and performance
counter CSVs on the **full SoC** model (`Vtop_verilator`), not the default
TCG+periph sim:

```bash
./ibex-vp-cosim/run_matmul.sh        # 8x8 NxN GEMM
./ibex-vp-cosim/run_matmul.sh 16     # other power-of-two N
./ibex-vp-cosim/run_matmul_sweep.sh  # N = 8..256; writes sw/matmul/*.csv
```

Firmware: `ibex-vp-cosim/sw/matmul/`. Results: `matmul_sweep.csv`, `matmul_pcount.csv`.

**Rule of thumb:** Ibex cycle counts → `run_matmul.sh`. Plugin doorbell demo →
`run_matmul_plugin.sh`.

## Full-SoC RTL (CPU in Verilator)

```bash
source ./env.sh
./ibex-vp-cosim/run_full_soc.sh
```

The launcher starts Verilator, loads the hello-world ELF into RTL RAM, waits for
the socket, and starts QEMU. Expected console output:

```text
Full SoC runs in Verilator; QEMU provides stepping and the UART console.
Logs: .../ibex-vp-cosim/build/soc-run.XXXXXX
Hello World! 00000001   Input Value: 00000000
Hello World! 00000002   Input Value: 00000000
```

Type a character such as `Z` to demonstrate UART input. The RTL UART receives it,
the Ibex firmware handles its RX interrupt, and the firmware echoes it through
the RTL UART and QEMU console. The hexadecimal counter is a firmware timer tick.
GPIO input pins are tied to zero in the full-SoC testbench.

Exit with **Ctrl+A, then X**. Ctrl+C also stops the launcher and its child
processes; QEMU's console can forward Ctrl+C to the guest, so use Ctrl+A, X for
the normal demo exit. The launcher cleans up its socket and children and preserves
`verilator.log` and `uart0.log` in the printed log directory.

First output appeared in a few seconds on the prepared Mac; other hosts and
firmware can take longer. This simulation runs in batches of 100,000 RTL cycles
and is not synchronized to wall-clock time.

Use a different firmware ELF, a custom socket, or additional QEMU arguments:

```bash
./ibex-vp-cosim/run_full_soc.sh /absolute/path/to/firmware.elf
IBEXDEMO_COSIM_SOCKET=/tmp/my-ibex-soc.sock ./ibex-vp-cosim/run_full_soc.sh
./ibex-vp-cosim/run_full_soc.sh "$ELF" -S
```

With `-S`, press Ctrl+A, C to open the QEMU monitor and enter `cont`; switch back
to the UART console with Ctrl+A, C. Pausing QEMU stops new stepping requests.
QEMU GDB and register commands do not expose the RTL CPU; use RTL waveforms for
debugging. This integration does not perform instruction lockstep comparison.

## Rebuild the integration

```bash
source ./env.sh
./ibex-vp-cosim/build_full_soc.sh
```

The builder applies the provided patches when needed, copies the bridge sources
into the external trees, rebuilds QEMU, and builds the full-SoC Verilator model
with FuseSoC. It can be rerun after editing `ibex-vp-cosim/full_soc/` or the shared
protocol header. Maintain those files in this repository; edits to their copies
in the external trees will be replaced by the builder.

The prepared native environment and firmware build adjustments are documented
in [LOCAL_SETUP.md](LOCAL_SETUP.md). If you change firmware sources, rebuild them
with the commands there before launching again.

## Dependencies for a fresh checkout

External source trees and tool installations are ignored by Git. You need:

- Git, a C/C++ compiler, Make, CMake, Python 3, Ninja, Meson, pkg-config, and Rust.
- Verilator and the lowRISC FuseSoC/Edalize Python dependencies.
- GLib, Pixman, zlib, libelf, and LZ4 development libraries.
- A bare-metal RISC-V GCC toolchain supporting RV32IMC, CSR, and instruction fence
  instructions, available through the `riscv32-unknown-elf-*` tool names.

Fetch the external sources only if the directories do not already exist:

```bash
git clone --recursive https://github.com/lowRISC/ibex-demo-system.git ibex-demo-system
git clone --branch ot-10.2.0 https://github.com/lowRISC/qemu.git qemu
source ./env.sh
python3 -m pip install -r ibex-demo-system/python-requirements.txt
```

Use a Python environment appropriate for your host. On this Mac it is
`local/env/`. Tested source revisions and tool versions are in `LOCAL_SETUP.md`.
For current GCC and CMake, build the firmware with:

```bash
cmake -S "$ROOT/ibex-demo-system/sw/c" -B "$ROOT/ibex-demo-system/sw/c/build" \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  '-DCMAKE_C_FLAGS=-march=rv32imc_zicsr_zifencei -mabi=ilp32 -mcmodel=medany -Wall -fvisibility=hidden -ffreestanding' \
  '-DCMAKE_ASM_FLAGS=-march=rv32imc_zicsr_zifencei -mabi=ilp32'
cmake --build "$ROOT/ibex-demo-system/sw/c/build" --target demo
./ibex-vp-cosim/build_full_soc.sh
```

The builder configures QEMU if needed, and never downloads dependencies itself.
Fresh builds require libraries discoverable by your compiler and pkg-config.
It adds the local macOS libelf/LZ4 paths when `local/env/lib/libelf.a` is present.

## Other simulation modes

### QEMU only

This is the fast functional baseline; QEMU emulates both the CPU and peripherals:

```bash
source ./env.sh
"$QEMU" -M ibexdemo -nographic -kernel "$ELF"
```

### Standalone full-SoC Verilator

The same RTL simulator works without a QEMU connection when the socket option
is omitted. UART goes to `uart0.log` and the testbench's pseudo-terminal:

```bash
source ./env.sh
cd "$ROOT/ibex-demo-system/build/lowrisc_ibex_demo_system_0/sim-verilator"
./Vtop_verilator --meminit=ram,"$ELF"
```

Read `uart0.log` from another terminal and stop with Ctrl+C.

The GPIO-only server `Vcosim_gpio_server` remains for `scripts/test_gpio_client.py`.

## Verification

```bash
source ./env.sh
python3 ibex-vp-cosim/scripts/test_full_soc.py
```

The seven integration tests cover real RTL firmware output, UART interrupt echo,
absence of a QEMU CPU, start-paused behavior, fragmented socket requests, bounded
cycle execution, invalid requests/responses, disconnects, and launcher cleanup.
They pass on the prepared Apple Silicon Mac. Build and test logs are in `deps/`.

## Troubleshooting

- **Missing tools or libraries:** source `env.sh` and review `LOCAL_SETUP.md`.
- **Old simulator binaries:** run `ibex-vp-cosim/build_full_soc.sh`. QEMU's machine
  list must contain `ibexdemo-soc-cosim`; the Verilator help must show
  `--qemu-cosim-socket=PATH`.
- **No UART output:** inspect the printed run directory's `verilator.log`; confirm
  the same ELF works in standalone RTL mode and QEMU has not been paused with `-S`.
- **Socket already exists:** stop its owning process before removing a stale
  socket. By default each launch gets its own socket directory under `/tmp`.
- **Protocol errors or response timeout:** rebuild both simulators together. The
  QEMU client rejects incompatible responses and stops if no response arrives
  within ten seconds; very slow hosts or waveform-heavy runs may hit that limit.
- **Using `-kernel` with full-SoC QEMU:** pass the ELF to `run_cosim.sh` instead.
  Verilator loads and executes it; this QEMU machine has no CPU or emulated RAM.

## Integration source map

- `ibex-vp-cosim/full_soc/soc_cosim.h`: RTL stepping server and UART byte queues.
- `ibex-vp-cosim/full_soc/ibexdemo_soc_cosim.c`: QEMU machine and asynchronous client.
- `ibex-vp-cosim/include/cosim_soc_protocol.h`: versioned, little-endian socket protocol.
- `patches/0003-full-soc-verilator-hooks.patch`: simulator registration and UART DPI hooks.
- `ibex-vp-cosim/build_full_soc.sh`: patch application and simulator builds.
- `ibex-vp-cosim/run_cosim.sh`: default periph cosim launch (wraps `run_periph_cosim.sh`).
- `ibex-vp-cosim/tcg_plugin/`: TCG matmul plugin and doorbell header.

Protocol details: [ibex-vp-cosim/README.md](ibex-vp-cosim/README.md).
Presentation commands: [CHEATSHEET.md](CHEATSHEET.md).

For the separately proposed QEMU-CPU instruction/cycle synchronization mode, see
[the strict lock-step design](docs/LOCKSTEP_DESIGN.md). It specifies per-edge
handshakes, MMIO completion stalls, clock accounting, and required QEMU core
changes. It is a design, not an implemented mode or a claim that instruction
counting reproduces physical Ibex CPU timing.
