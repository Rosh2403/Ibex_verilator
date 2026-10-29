# Other simulation modes (full-SoC RTL, Verilator peripherals, QEMU only)

This is the complete guide to every simulation mode in the repository. The main
[README](../README.md) covers only the daemon-based NPU connected through the
QEMU TCG plugin; read this page for the full-SoC RTL and other Verilator modes.

This repository connects the [lowRISC Ibex Demo System](https://github.com/lowRISC/ibex-demo-system)
to QEMU and Verilator in several ways. Its main demos are:

- **Matmul accelerator daemon:** QEMU runs the Ibex firmware, and a QEMU TCG
  plugin offloads matrix multiplies to a separate accelerator process.
- **Full-SoC RTL:** the whole Ibex SoC runs in Verilator, and QEMU provides the
  console.

**Contents:** [Quick start](#quick-start) ·
[Set up from scratch](#set-up-from-scratch) · [Demos](#demos) ·
[Processes, sockets, and logs](#processes-sockets-and-logs) ·
[Rebuilding](#rebuilding-after-changes) · [Tests](#tests) ·
[Troubleshooting](#troubleshooting) · [Source map](#source-map)

## Quick start

On a machine that is already set up, from the repository root:

```bash
source ./env.sh                          # do this in every new terminal
./ibex-vp-cosim/run_matmul_plugin.sh     # matmul daemon demo; ends with "Result: PASS"
./ibex-vp-cosim/run_full_soc.sh          # full-SoC RTL demo; exit with Ctrl+A, then X
```

Not set up yet? Follow the next section.

## Set up from scratch

These steps start from an empty directory on **Ubuntu or Debian (x86-64)**.
Notes for macOS follow at the end of the section.

There are two levels:

| You want | Do steps | Needs Verilator? | Rough build time |
| --- | --- | --- | --- |
| Matmul daemon demo only | 1–6, then 9 | no | QEMU build, 10–20 minutes |
| Everything (adds full-SoC RTL) | 1–9 | yes | add 15–30 minutes for Verilator and the RTL model |

Run all commands from the repository root unless a step says otherwise.

### 1. Install system packages

```bash
sudo apt update
sudo apt install -y git curl xz-utils build-essential cmake ninja-build pkg-config \
  python3 python3-venv python3-pip python3-tomli rustc \
  libglib2.0-dev libpixman-1-dev zlib1g-dev libfdt-dev libelf-dev \
  autoconf flex bison help2man perl libfl-dev
```

- QEMU needs GLib, Pixman, zlib, and libfdt. This QEMU branch also wants a Rust
  compiler while configuring, even though Rust devices are disabled.
- The last line (`autoconf` … `libfl-dev`) and `libelf-dev` are needed only to
  build Verilator and the full-SoC model.

### 2. Clone this repository

```bash
git clone -b ibex-matmul-daemon https://github.com/Rosh2403/Ibex_verilator.git
cd Ibex_verilator
source ./env.sh
```

`env.sh` sets `ROOT`, `QEMU`, and `ELF`, and puts the folders the next steps
create on your `PATH`: `toolchain/bin`, `local/env/bin`, and `verilator/bin`.
Source it again in every new terminal.

### 3. Install the RISC-V toolchain

Use lowRISC's prebuilt GCC, the toolchain the Ibex Demo System recommends. It
unpacks into `toolchain/`, where `env.sh` looks for it:

```bash
mkdir -p toolchain
curl -L https://github.com/lowRISC/lowrisc-toolchains/releases/download/20220524-1/lowrisc-toolchain-gcc-rv32imcb-20220524-1.tar.xz \
  | tar -xJ -C toolchain --strip-components=1
riscv32-unknown-elf-gcc --version        # should print a version, not "command not found"
```

Any bare-metal RV32IMC GCC with the `riscv32-unknown-elf-` prefix also works.
The builds detect whether your GCC needs the newer `rv32imc_zicsr_zifencei`
ISA name or plain `rv32imc`.

### 4. Get the QEMU and Ibex Demo System sources

These are pinned to the revisions this repository's patches were made for:

```bash
git clone https://github.com/lowRISC/qemu.git qemu
git -C qemu checkout d7655281a649abb339f5267f0b9e05329b3cee6d          # ot-10.2.0 base
git clone --recursive https://github.com/lowRISC/ibex-demo-system.git ibex-demo-system
git -C ibex-demo-system checkout d37beb753e19482eb054aefac73a27289bf72e47
git -C ibex-demo-system submodule update --init --recursive
```

Both directories are ignored by this repository's Git. The build scripts patch
them in place.

### 5. Create the Python environment

FuseSoC and Edalize, which build the RTL model, come from pip. The environment
goes in `local/env/`, which `env.sh` puts on `PATH`:

```bash
python3 -m venv local/env
local/env/bin/pip install -r ibex-demo-system/python-requirements.txt
hash -r                                   # pick up the new python3 and fusesoc
```

### 6. Configure QEMU with plugin support

Configure once with TCG plugins enabled. The build scripts then keep these
options:

```bash
mkdir -p qemu/build
(cd qemu/build && ../configure --target-list=riscv32-softmmu \
  --without-default-features --enable-tcg --enable-tools --disable-rust \
  --enable-plugins)
```

There is no separate QEMU build step. The scripts in steps 8 and 9 compile QEMU
the first time they run, which is the longest part of the setup.

### 7. Build Verilator (full-SoC RTL only)

Skip this step if you only want the matmul daemon demo. The tested version is
5.052. It is built in place in `verilator/`, where `env.sh` finds it:

```bash
git clone https://github.com/verilator/verilator.git verilator
cd verilator
git checkout v5.052
autoconf && ./configure && make -j"$(nproc)"
cd ..
source ./env.sh                           # sets VERILATOR_ROOT now that it exists
verilator --version                       # Verilator 5.052 ...
```

### 8. Build the hello-world firmware and the full-SoC model (full-SoC RTL only)

```bash
# Pick the ISA name your GCC understands (newer GCC needs the _zicsr_zifencei form).
if echo 'int x;' | riscv32-unknown-elf-gcc -march=rv32imc_zicsr_zifencei -mabi=ilp32 \
     -x c -c -o /dev/null - 2>/dev/null; then MARCH=rv32imc_zicsr_zifencei; else MARCH=rv32imc; fi

cmake -S ibex-demo-system/sw/c -B ibex-demo-system/sw/c/build \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  "-DCMAKE_C_FLAGS=-march=$MARCH -mabi=ilp32 -mcmodel=medany -Wall -fvisibility=hidden -ffreestanding" \
  "-DCMAKE_ASM_FLAGS=-march=$MARCH -mabi=ilp32"
cmake --build ibex-demo-system/sw/c/build --target demo
ls "$ELF"                                 # the hello-world ELF the demos load

./ibex-vp-cosim/build_full_soc.sh
```

`build_full_soc.sh` applies `patches/0001`–`0003`, builds QEMU (and its new
`ibexdemo-soc-cosim` machine), and builds `Vtop_verilator` with FuseSoC.

### 9. Build and run the matmul daemon demo, then test

```bash
./ibex-vp-cosim/run_matmul_plugin.sh
```

The first run builds QEMU if step 8 didn't, then builds the daemon, the plugin,
and the matmul firmware. It should end with `Result: PASS`. Then run the tests:

```bash
python3 ibex-vp-cosim/scripts/test_matmul_daemon.py    # expect: Ran 24 tests ... OK
python3 ibex-vp-cosim/scripts/test_full_soc.py         # after step 8; expect: Ran 7 tests ... OK
./ibex-vp-cosim/run_full_soc.sh                        # after step 8; Ctrl+A, then X to exit
```

### Setup notes

- **Already-set-up checkout:** skip any step whose result already exists, and
  never re-clone over `qemu/` or `ibex-demo-system/`. Re-running the build
  scripts is safe.
- **Where things live:** everything is installed inside the repository
  (`toolchain/`, `local/`, `verilator/`, `qemu/`, `ibex-demo-system/`). Nothing
  is installed system-wide except the `apt` packages, and no shell profile is
  edited.
- **macOS (Apple Silicon):** lowRISC's toolchain is Linux-only. Unpack the
  [xPack RISC-V GCC](https://github.com/xpack-dev-tools/riscv-none-elf-gcc-xpack/releases)
  into `toolchain/` the same way, then give it the expected names:

  ```bash
  (cd toolchain/bin && for t in riscv-none-elf-*; do
     ln -sf "$t" "riscv32-unknown-elf-${t#riscv-none-elf-}"; done)
  ```

  The other dependencies need macOS equivalents. The prepared Mac used a
  micromamba environment in `local/env/` with libelf built from source; its
  exact versions and build adjustments are in [LOCAL_SETUP.md](../LOCAL_SETUP.md).
  These macOS steps haven't been re-run from an empty machine.

## What runs where

Each mode splits the Ibex Demo System differently between QEMU, Verilator, and
helper processes. The processes talk over Unix sockets; nothing is linked into
one binary.

| Mode | Command | CPU runs in | Other hardware | Status on the prepared Mac |
| --- | --- | --- | --- | --- |
| [Matmul accelerator daemon](#matmul-accelerator-daemon) | `run_matmul_plugin.sh` | QEMU TCG | matmul in `matmul_daemon` | built, tests pass |
| [Full-SoC RTL](#full-soc-rtl-cpu-in-verilator) | `run_full_soc.sh` | Verilator | Verilator | built, tests pass |
| [Matmul on Ibex RTL](#matrix-multiply-on-ibex-rtl) | `run_matmul.sh` | Verilator | Verilator | uses the full-SoC build |
| [TCG CPU + RTL peripherals](#qemu-tcg-cpu--verilator-peripherals) | `run_cosim.sh` | QEMU TCG | GPIO/UART/timer in Verilator | **not built** |
| [QEMU only](#qemu-only) | `"$QEMU" -M ibexdemo …` | QEMU TCG | QEMU models | works |

All `run_*.sh` and `build_*.sh` scripts are in `ibex-vp-cosim/`.

## Demos

### Matmul accelerator daemon

The matmul accelerator is **its own host process**, a daemon listening on a Unix
socket. QEMU runs the Ibex firmware. A **TCG plugin** loaded into QEMU watches
guest stores. When the firmware writes the doorbell address **`0x0011C014`**,
the plugin reads matrices `A` and `B` from guest RAM, sends them to the daemon,
waits for `C`, writes `C` back, and sets a status word. The plugin does no math.

```text
 qemu-system-riscv32 -M ibexdemo                      matmul_daemon (separate process)
 +----------------------------------+                +-------------------------------+
 | Ibex firmware (TCG)              |                | Unix socket server            |
 |   write N, A, B, C to mailbox    |  GEMM request  | one thread per QEMU client    |
 |   store 1 -> 0x0011C014 (KICK) --+--> plugin ---->|   C = A * B (int32, wrapping) |
 |   read STATUS, check C           |<-- plugin <----|   logs every request          |
 +----------------------------------+   C + timing   +-------------------------------+
```

The guest waits at the doorbell store until the daemon answers, so results are
deterministic. Accelerator timing isn't modeled; this is a functional offload
demo. It needs no QEMU source patch, only QEMU built with plugin support.

**One command:**

```bash
./ibex-vp-cosim/run_matmul_plugin.sh
```

This builds what's needed, starts a daemon if none is running, runs QEMU, prints
the daemon's log for this run, and stops the daemon it started. Expected:

```text
matmul daemon started: pid 55602, socket /tmp/ibex-matmul-accel.sock, log ...
[matmul plugin] connected to daemon pid 55602 at /tmp/ibex-matmul-accel.sock ...
guest: kick 4x4 ... [matmul plugin] kick #1: 4x4 GEMM by daemon: 39.2 us = read A,B 11.42 + socket 26.00 + ...
ok  checksum FFFE8FC9
...
guest: kick 32x32 ... [matmul plugin] kick #4: 32x32 GEMM by daemon: 21.8 us = read A,B 1.38 + ...
ok  checksum 001F11EC
guest: kick N=33 (expect rejection) ... [matmul plugin] kick #5: rejected N=33 ...
rejected
PASS
--- matmul daemon log (/tmp/ibex-matmul-accel.sock.log) ---
... [matmul-daemon] client 1 seq 5: GEMM 32x32 (32768 MACs) in 3750 ns [total 4]
Result: PASS
```

The firmware checks every element of `C` against its own calculation and exits
QEMU with status 0 on PASS, 1 on FAIL.

| Setting | Effect |
| --- | --- |
| `KEEP_DAEMON=1` | leave a daemon the launcher started running afterwards |
| `QEMU_TIMEOUT=N` | kill QEMU after N seconds (default 60) |
| `IBEX_MATMUL_SOCKET=PATH` | socket used by the launcher, daemon, and plugin |

Extra arguments go to QEMU, for example `./ibex-vp-cosim/run_matmul_plugin.sh -d plugin`.

**Two terminals, to show the daemon as a separate service:**

```bash
./ibex-vp-cosim/matmul_daemon.sh foreground   # terminal A: live daemon log; Ctrl+C stops
./ibex-vp-cosim/run_matmul_plugin.sh          # terminal B: run as often as you like
```

Each run shows up as a new client in terminal A, and the request total keeps
growing.

**Managing a background daemon:**

```bash
./ibex-vp-cosim/matmul_daemon.sh start        # detach; returns once the socket is listening
./ibex-vp-cosim/matmul_daemon.sh status       # pid and GEMMs served
./ibex-vp-cosim/matmul_daemon.sh log          # print its log (log -f follows it)
./ibex-vp-cosim/matmul_daemon.sh restart
./ibex-vp-cosim/matmul_daemon.sh stop         # SIGTERM; removes socket and pidfile
```

`stop` asks the daemon for its pid over the socket, so it never signals an
unrelated process that reused an old pid. `start` does nothing if a daemon is
already running.

**Running the pieces by hand:**

```bash
B=ibex-vp-cosim/build
$B/matmul_daemon --socket /tmp/ibex-matmul-accel.sock --daemon   # or omit --daemon for foreground
$B/matmul_daemon --socket /tmp/ibex-matmul-accel.sock --ping     # query it

"$QEMU" -M ibexdemo -nographic -kernel $B/matmul_accel.elf \
  -plugin $B/matmul_plugin.so,socket=/tmp/ibex-matmul-accel.sock,timeout_ms=5000
```

QEMU refuses to start if no daemon answers on the socket. If the daemon restarts
while QEMU runs, the plugin reconnects once and resends. If the daemon doesn't
answer within `timeout_ms`, the guest gets an error code instead of QEMU
hanging.

More detail: [docs/MATMUL_DAEMON.md](MATMUL_DAEMON.md) (protocol, mailbox,
error codes) and [docs/MATMUL_DEEP_DIVE.md](MATMUL_DEEP_DIVE.md) (presenter
notes for a code walkthrough).

### Full-SoC RTL (CPU in Verilator)

```bash
./ibex-vp-cosim/run_full_soc.sh
```

The Ibex CPU, RAM, GPIO, timer, UART, PWM, and other peripherals all run in the
RTL model. QEMU's `ibexdemo-soc-cosim` machine has **no emulated CPU**: it asks
Verilator to run clock cycles and relays UART bytes over a Unix socket. The
launcher starts Verilator, loads the hello-world ELF into RTL RAM, waits for the
socket, and starts QEMU. Expected:

```text
Full SoC runs in Verilator; QEMU provides stepping and the UART console.
Logs: .../ibex-vp-cosim/build/soc-run.XXXXXX
Hello World! 00000001   Input Value: 00000000
Hello World! 00000002   Input Value: 00000000
```

- Type a character such as `Z`: the RTL UART receives it, the firmware handles
  the interrupt, and echoes it back. The hex counter is a firmware timer tick.
  GPIO inputs are tied to zero.
- Exit with **Ctrl+A, then X**. The launcher cleans up its socket and child
  processes and keeps `verilator.log` and `uart0.log` in the printed `Logs:`
  directory.
- The simulation runs in batches of 100,000 RTL cycles and isn't tied to
  wall-clock time. First output takes a few seconds.

Other firmware, a custom socket, or extra QEMU arguments:

```bash
./ibex-vp-cosim/run_full_soc.sh /absolute/path/to/firmware.elf
IBEXDEMO_COSIM_SOCKET=/tmp/my-ibex-soc.sock ./ibex-vp-cosim/run_full_soc.sh
./ibex-vp-cosim/run_full_soc.sh "$ELF" -S     # start paused
```

With `-S`, press Ctrl+A, C to open the QEMU monitor, enter `cont`, and press
Ctrl+A, C to return to the console. QEMU's debugger and register commands can't
see the RTL CPU; use RTL waveforms for debugging.

### Matrix multiply on Ibex RTL

Measures **Ibex `mcycle`**, executed cycles, and performance counters on the
full-SoC model:

```bash
./ibex-vp-cosim/run_matmul.sh        # 8x8
./ibex-vp-cosim/run_matmul.sh 16     # other power-of-two sizes
./ibex-vp-cosim/run_matmul_sweep.sh  # N = 8..256; writes ibex-vp-cosim/sw/matmul/*.csv
```

Use this for Ibex cycle counts. Use `run_matmul_plugin.sh` for offloading to the
accelerator daemon.

### QEMU TCG CPU + Verilator peripherals

QEMU runs the CPU; GPIO, UART, and timer run as RTL in `Vcosim_periph_server`,
connected over `/tmp/ibexdemo-cosim.sock`:

```bash
./ibex-vp-cosim/build_periph_cosim.sh
./ibex-vp-cosim/run_cosim.sh          # same as run_periph_cosim.sh
```

**Not available on the prepared Mac:** `Vcosim_periph_server` isn't built, and
`patches/0004-tcg-periph-cosim-qemu.patch` doesn't apply cleanly to the
installed QEMU tree. `run_cosim.sh` exits with a build message until that is
fixed.

### QEMU only

```bash
"$QEMU" -M ibexdemo -nographic -kernel "$ELF"
```

Prints `Hello World!` lines. Exit with **Ctrl+A, then X**.

### Standalone full-SoC Verilator

The RTL model also runs without QEMU; UART output goes to `uart0.log`:

```bash
cd "$ROOT/ibex-demo-system/build/lowrisc_ibex_demo_system_0/sim-verilator"
./Vtop_verilator --meminit=ram,"$ELF"     # in another terminal: tail -F uart0.log
```

Stop with Ctrl+C.

## Processes, sockets, and logs

Only the matmul accelerator is a true daemon: it detaches and outlives the
command that started it, so several QEMU runs can share it. The Verilator
servers are child processes of their launchers and stop with them.

| Process | Started by | Socket | Logs |
| --- | --- | --- | --- |
| `matmul_daemon` | `matmul_daemon.sh start`, or `run_matmul_plugin.sh` if none is running | `/tmp/ibex-matmul-accel.sock` (`$IBEX_MATMUL_SOCKET`) | `SOCKET.log`; pidfile `SOCKET.pid` |
| `Vtop_verilator` | `run_full_soc.sh` | new directory under `/tmp` per launch | printed `Logs:` directory |
| `Vcosim_periph_server` | `run_cosim.sh` | `/tmp/ibexdemo-cosim.sock` | terminal |
| `Vcosim_gpio_server` | by hand (GPIO smoke test) | `--socket PATH` | terminal |

See what's running:

```bash
./ibex-vp-cosim/matmul_daemon.sh status
pgrep -fl 'matmul_daemon|Vtop_verilator|Vcosim_|qemu-system-riscv32'
```

The matmul daemon's socket is owner-only (mode 0600), and the daemon replaces a
stale socket left by a crash on its next start.

## Rebuilding after changes

| You changed | Run |
| --- | --- |
| Daemon, plugin, or matmul firmware (`ibex-vp-cosim/matmul_daemon/`, `tcg_plugin/`, `sw/matmul_accel/`) | nothing; `run_matmul_plugin.sh` rebuilds them (or `make -C ibex-vp-cosim matmul`) |
| Full-SoC bridge (`ibex-vp-cosim/full_soc/`, `include/cosim_soc_protocol.h`) or patches 0001–0003 | `./ibex-vp-cosim/build_full_soc.sh` |
| Hello-world firmware in `ibex-demo-system/sw/c/` | `cmake --build ibex-demo-system/sw/c/build --target demo` |
| Peripheral cosim sources or patch 0004 | `./ibex-vp-cosim/build_periph_cosim.sh` |

The build scripts copy bridge sources from this repository into `qemu/` and
`ibex-demo-system/`. Edit the originals here, because copies in those trees are
replaced on the next build. Only `build_matmul_plugin.sh` reconfigures QEMU, and
only to add plugin support when it's missing; it keeps the other options.

## Tests

```bash
python3 ibex-vp-cosim/scripts/test_matmul_daemon.py   # 24 tests, a few seconds
python3 ibex-vp-cosim/scripts/test_full_soc.py        # 7 tests, a few seconds
```

- **Matmul daemon:** daemon lifecycle (detach, pidfile, owner-only socket,
  cleanup, refusing a second daemon, stale sockets); protocol correctness for
  N = 1..64 and every error case; six concurrent clients; QEMU end to end,
  including no daemon, no plugin, a daemon restart mid-run, and a hung daemon;
  the launcher and control scripts. Each test uses its own temporary socket, so
  it won't disturb a running demo daemon.
- **Full SoC:** RTL firmware output, UART interrupt echo, no QEMU CPU,
  start-paused behavior, fragmented socket requests, bounded cycle execution,
  invalid messages, disconnects, and launcher cleanup.

Both suites pass on the prepared Apple Silicon Mac.

## Troubleshooting

**Setup and builds**

- **`riscv32-unknown-elf-gcc: command not found`:** run `source ./env.sh`, and
  check that `toolchain/bin/riscv32-unknown-elf-gcc` exists (step 3).
- **`Invalid or unknown z ISA extension: 'zifencei'`** (or `unrecognized opcode
  csrr` the other way round): your GCC uses the other ISA naming. The matmul
  build detects this automatically (force it with
  `make -C ibex-vp-cosim RV_MARCH=rv32imc`); for the hello-world firmware, use
  the `MARCH` detection in step 8.
- **QEMU `configure` fails:** usually a missing package from step 1; the error
  names it. A missing `rustc` stops this QEMU branch even with Rust disabled.
- **`fusesoc: command not found`:** step 5 wasn't done, or `env.sh` wasn't
  sourced afterwards.
- **Old simulator binaries:** run `./ibex-vp-cosim/build_full_soc.sh`. QEMU's
  machine list (`"$QEMU" -M help`) must include `ibexdemo-soc-cosim`, and
  `Vtop_verilator --help` must show `--qemu-cosim-socket=PATH`.

**Matmul daemon demo**

- **`cannot connect to matmul daemon at …` and QEMU exits:** no daemon on that
  socket. Run `./ibex-vp-cosim/matmul_daemon.sh start`, and use the same
  `IBEX_MATMUL_SOCKET` in every terminal.
- **QEMU rejects `-plugin`, or the plugin won't load:** QEMU was built without
  plugin support. Run `./ibex-vp-cosim/build_matmul_plugin.sh`.
- **`a daemon is already listening on …`:** reuse it, or run
  `matmul_daemon.sh stop` first.
- **`… exists and is not a socket`:** the daemon won't delete a regular file at
  the socket path. Remove it or choose another `IBEX_MATMUL_SOCKET`.
- **`socket path too long`:** Unix socket paths are limited to about 100 bytes;
  use a short path under `/tmp`.
- **Guest prints `no response (is the plugin loaded?)`:** QEMU ran without
  `-plugin …matmul_plugin.so`.
- **Guest prints `accelerator error 3`:** the daemon disappeared or didn't answer
  within `timeout_ms`. Check `matmul_daemon.sh log`.

**Verilator modes**

- **No UART output:** check `verilator.log` in the printed `Logs:` directory,
  confirm the same ELF works in standalone Verilator, and make sure QEMU isn't
  paused with `-S`.
- **Protocol errors or response timeout:** rebuild both simulators together
  (`build_full_soc.sh`). QEMU stops if Verilator doesn't answer within ten
  seconds, which very slow hosts or waveform-heavy runs can hit.
- **`-kernel` with the full-SoC machine:** pass the ELF to `run_full_soc.sh`
  instead; that QEMU machine has no CPU or RAM.
- **`run_cosim.sh` asks you to build the peripheral cosim:** see
  [its status](#qemu-tcg-cpu--verilator-peripherals).

## Source map

**Matmul accelerator daemon**

- `ibex-vp-cosim/matmul_daemon/matmul_daemon.c`: the daemon.
- `ibex-vp-cosim/tcg_plugin/matmul_plugin.c`: the TCG plugin (the daemon's client).
- `ibex-vp-cosim/tcg_plugin/matmul_accel.h`: guest mailbox layout and status codes.
- `ibex-vp-cosim/include/matmul_daemon_protocol.h`: plugin-to-daemon socket protocol.
- `ibex-vp-cosim/sw/matmul_accel/main.c`: firmware that offloads and checks GEMMs.
- `ibex-vp-cosim/matmul_daemon.sh`: start, stop, restart, status, log, foreground.
- `ibex-vp-cosim/run_matmul_plugin.sh`, `build_matmul_plugin.sh`, `Makefile`: launcher and builds.
- `ibex-vp-cosim/scripts/test_matmul_daemon.py`: tests.

**Full-SoC RTL**

- `ibex-vp-cosim/full_soc/soc_cosim.h`: RTL stepping server and UART queues.
- `ibex-vp-cosim/full_soc/ibexdemo_soc_cosim.c`: QEMU machine and client.
- `ibex-vp-cosim/include/cosim_soc_protocol.h`: socket protocol.
- `patches/0001`–`0003`: QEMU and Verilator integration patches.
- `ibex-vp-cosim/build_full_soc.sh`, `run_full_soc.sh`, `scripts/test_full_soc.py`.

**Other modes**

- TCG + RTL peripherals: `ibex-vp-cosim/periph/`, `cosim_periph_main.cc`,
  `rtl/cosim_periph_top.sv`, `include/cosim_protocol.h`, `patches/0004`,
  `build_periph_cosim.sh`, `run_cosim.sh`.
- Ibex RTL matmul: `ibex-vp-cosim/sw/matmul/`, `build_matmul.sh`, `run_matmul.sh`,
  `run_matmul_sweep.sh`.

## Further reading

- [CHEATSHEET.md](../CHEATSHEET.md): demo commands on one page.
- [LOCAL_SETUP.md](../LOCAL_SETUP.md): the prepared Mac's exact tool versions and adjustments.
- [docs/MATMUL_DAEMON.md](MATMUL_DAEMON.md): matmul daemon protocol and behavior.
- [docs/MATMUL_DEEP_DIVE.md](MATMUL_DEEP_DIVE.md): presenter notes for a code walkthrough.
- [ibex-vp-cosim/README.md](../ibex-vp-cosim/README.md): full-SoC protocol details.
- [docs/LOCKSTEP_DESIGN.md](LOCKSTEP_DESIGN.md): a proposed (not implemented)
  QEMU-CPU instruction/cycle lock-step mode.
