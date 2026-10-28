# Ibex Demo System: QEMU + Verilator + accelerator daemon

Run every command from the **repository root** (`~/Ibex_verilator`) after
sourcing the environment:

```bash
cd ~/Ibex_verilator
source ./env.sh
```

The source trees, native tools, firmware, and simulator binaries may already be
built in a prepared workspace. For another checkout, see
[Dependencies for a fresh checkout](#dependencies-for-a-fresh-checkout).

## What runs where

Each mode splits the Ibex Demo System differently between QEMU, Verilator, and
helper processes. The processes talk over Unix sockets; nothing is linked into
one binary.

| Mode | Command | CPU | Other hardware | Processes | Status on the prepared Mac |
| --- | --- | --- | --- | --- | --- |
| [Matmul accelerator daemon](#matmul-accelerator-daemon-qemu-tcg-plugin) | `run_matmul_plugin.sh` | QEMU TCG | matmul in `matmul_daemon` | QEMU (+ plugin), daemon | built, tests pass |
| [Full-SoC RTL](#full-soc-rtl-cpu-in-verilator) | `run_full_soc.sh` | Verilator RTL | Verilator RTL | QEMU (console only), `Vtop_verilator` | built, tests pass |
| [Matmul on Ibex RTL](#matrix-multiply-on-ibex-rtl-verilator) | `run_matmul.sh` | Verilator RTL | Verilator RTL | `Vtop_verilator` | uses the full-SoC build |
| [TCG CPU + RTL peripherals](#qemu-tcg-cpu--verilator-peripherals) | `run_cosim.sh` | QEMU TCG | GPIO/UART/timer in Verilator | QEMU, `Vcosim_periph_server` | **not built** (see section) |
| [QEMU only](#qemu-only) | `"$QEMU" -M ibexdemo …` | QEMU TCG | QEMU models | QEMU | works |

All `run_*.sh` and `build_*.sh` scripts live in `ibex-vp-cosim/`.

## Background processes, sockets, and logs

Only the matmul accelerator is a true daemon: it detaches from the terminal and
outlives the command that started it, so several QEMU runs can share it. The
Verilator servers are children of their launchers and stop with them.

| Process | Started by | Socket | Logs | Stopped by |
| --- | --- | --- | --- | --- |
| `matmul_daemon` | `matmul_daemon.sh start`, or `run_matmul_plugin.sh` if none is running | `/tmp/ibex-matmul-accel.sock` (`$IBEX_MATMUL_SOCKET`) | `SOCKET.log`, pidfile `SOCKET.pid` | `matmul_daemon.sh stop`; `run_matmul_plugin.sh` stops only a daemon it started |
| `Vtop_verilator` (full-SoC server) | `run_full_soc.sh` | new directory under `/tmp` per launch (`$IBEXDEMO_COSIM_SOCKET`) | printed run directory: `verilator.log`, `uart0.log` | the launcher on exit |
| `Vcosim_periph_server` | `run_cosim.sh` / `run_periph_cosim.sh` | `/tmp/ibexdemo-cosim.sock` (`$IBEXDEMO_COSIM_SOCKET`) | terminal | the launcher on exit |
| `Vcosim_gpio_server` | by hand (GPIO smoke test) | `--socket PATH` | terminal | Ctrl+C |

To check what is running:

```bash
./ibex-vp-cosim/matmul_daemon.sh status      # pid and GEMMs served, or "no daemon"
pgrep -fl 'matmul_daemon|Vtop_verilator|Vcosim_|qemu-system-riscv32'
```

The matmul daemon's socket is owner-only (mode 0600). A socket left behind by
a crash is harmless: the matmul daemon replaces a stale socket on its next
start, `run_cosim.sh` removes its socket before starting, and `run_full_soc.sh`
uses a new directory for each launch.

## Matmul accelerator daemon (QEMU TCG plugin)

The matmul accelerator runs as **its own host process**, a daemon on a Unix
socket. QEMU TCG runs the Ibex firmware. A **TCG plugin** loaded into QEMU
watches guest stores. When the firmware writes the doorbell at **`0x0011C014`**,
the plugin reads `N`, `A`, and `B` from guest RAM, sends them to the daemon,
waits for `C`, writes `C` back, and sets `STATUS`. The plugin does no math.

```text
 qemu-system-riscv32 -M ibexdemo                      matmul_daemon (separate process)
 +----------------------------------+                +-------------------------------+
 | Ibex firmware (TCG)              |                | Unix socket server            |
 |   write N, A, B, C to mailbox    |  GEMM request  | one thread per QEMU client    |
 |   store 1 -> 0x0011C014 (KICK) --+--> plugin ---->|   C = A * B (int32, wrapping) |
 |   read STATUS, check C           |<-- plugin <----|   logs every request          |
 +----------------------------------+   C + timing   +-------------------------------+
```

The guest is held at the kicking store until the daemon answers, so results are
deterministic. Timing is not modeled; this is a functional offload demo. It
needs no QEMU source patch, only QEMU built with plugin support.

### One-command demo

```bash
./ibex-vp-cosim/run_matmul_plugin.sh
```

The launcher builds everything, starts the daemon if none is running, runs QEMU,
prints the daemon's log for this run, and stops the daemon it started. The first
run on a QEMU build without plugin support reconfigures QEMU with
`--enable-plugins`, which is one full QEMU rebuild. Expected:

```text
matmul daemon started: pid 55602, socket /tmp/ibex-matmul-accel.sock, log ...
[matmul plugin] connected to daemon pid 55602 at /tmp/ibex-matmul-accel.sock ...
guest: kick 4x4 ... [matmul plugin] kick #1: 4x4 GEMM ... by daemon: 64 MACs, ...
ok  checksum FFFE8FC9
...
guest: kick 32x32 ... [matmul plugin] kick #4: 32x32 GEMM ... 32768 MACs, ...
ok  checksum 001F11EC
guest: kick N=33 (expect rejection) ... [matmul plugin] kick #5: rejected N=33 ...
rejected
PASS
--- matmul daemon log (/tmp/ibex-matmul-accel.sock.log) ---
... [matmul-daemon] client 1 seq 5: GEMM 32x32 (32768 MACs) in 3750 ns [total 4]
Result: PASS
```

The firmware checks every element of `C` against its own software GEMM and
exits QEMU with status 0 on PASS, 1 on FAIL. Launcher options:

| Variable | Effect |
| --- | --- |
| `KEEP_DAEMON=1` | leave a daemon the launcher started running afterwards |
| `QEMU_TIMEOUT=N` | kill QEMU after N seconds (default 60) |
| `IBEX_MATMUL_SOCKET=PATH` | socket for the launcher, daemon, and plugin |

Extra arguments are passed to QEMU, for example
`./ibex-vp-cosim/run_matmul_plugin.sh -d plugin`.

### Two-terminal demo (the daemon as a visible service)

Terminal A, the accelerator in the foreground with live logs:

```bash
./ibex-vp-cosim/matmul_daemon.sh foreground
```

Terminal B, any number of QEMU runs against it:

```bash
./ibex-vp-cosim/run_matmul_plugin.sh
./ibex-vp-cosim/run_matmul_plugin.sh      # same daemon; its request total keeps growing
```

Stop terminal A with Ctrl+C.

### Managing the daemon

```bash
./ibex-vp-cosim/matmul_daemon.sh start       # detach; returns once the socket is listening
./ibex-vp-cosim/matmul_daemon.sh status      # pid and GEMMs served
./ibex-vp-cosim/matmul_daemon.sh log         # print its log (log -f follows it)
./ibex-vp-cosim/matmul_daemon.sh restart
./ibex-vp-cosim/matmul_daemon.sh stop        # SIGTERM; removes socket and pidfile
./ibex-vp-cosim/matmul_daemon.sh foreground  # run attached to this terminal
```

`stop` finds the daemon by asking it for its pid over the socket, so it never
signals an unrelated process that reused an old pid. A second `start` while one
is running is a no-op.

### Running the pieces by hand

```bash
B=ibex-vp-cosim/build
$B/matmul_daemon --socket /tmp/ibex-matmul-accel.sock          # foreground
$B/matmul_daemon --socket /tmp/ibex-matmul-accel.sock --daemon # detached
$B/matmul_daemon --socket /tmp/ibex-matmul-accel.sock --ping   # query it

"$QEMU" -M ibexdemo -nographic -kernel $B/matmul_accel.elf \
  -plugin $B/matmul_plugin.so,socket=/tmp/ibex-matmul-accel.sock,timeout_ms=5000
```

QEMU refuses to start if no daemon answers on the socket. If the daemon is
restarted while QEMU runs, the plugin reconnects once and resends. If the daemon
stops answering within `timeout_ms`, the guest gets an error code instead of
QEMU hanging.

Protocol, mailbox layout, and error codes:
[docs/MATMUL_DAEMON.md](docs/MATMUL_DAEMON.md). Presenter notes for a code
walkthrough: [docs/MATMUL_DEEP_DIVE.md](docs/MATMUL_DEEP_DIVE.md).

## Full-SoC RTL (CPU in Verilator)

```bash
./ibex-vp-cosim/run_full_soc.sh
```

The Ibex CPU, RAM, GPIO, timer, UART, PWM, and other peripherals execute in the
RTL model. QEMU's `ibexdemo-soc-cosim` machine requests clock cycles and relays
UART bytes over a Unix socket; it instantiates **no emulated CPU**. The launcher
starts Verilator, loads the hello-world ELF into RTL RAM, waits for the socket,
and starts QEMU. Expected console output:

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

## Matrix multiply on Ibex RTL (Verilator)

Use this path to measure **Ibex `mcycle`**, executed cycles, and performance
counter CSVs on the **full SoC** model (`Vtop_verilator`):

```bash
./ibex-vp-cosim/run_matmul.sh        # 8x8 NxN GEMM
./ibex-vp-cosim/run_matmul.sh 16     # other power-of-two N
./ibex-vp-cosim/run_matmul_sweep.sh  # N = 8..256; writes sw/matmul/*.csv
```

Firmware: `ibex-vp-cosim/sw/matmul/`. Results: `matmul_sweep.csv`, `matmul_pcount.csv`.

**Rule of thumb:** Ibex cycle counts → `run_matmul.sh`. Offloading to the
accelerator daemon → `run_matmul_plugin.sh`.

## QEMU TCG CPU + Verilator peripherals

This is the `run_cosim.sh` target on the `ibex-matmul` branch (`ibexdemo-cosim`):

- **CPU and RAM:** QEMU TCG (fast functional Ibex).
- **GPIO, UART, timer:** cycle-accurate Verilator RTL in `Vcosim_periph_server`.
- **Link:** two processes on a Unix socket (`cosim_protocol.h`).

```bash
./ibex-vp-cosim/build_periph_cosim.sh
./ibex-vp-cosim/run_cosim.sh          # same as run_periph_cosim.sh
```

Expected: `Hello World!` on the UART console. Exit with **Ctrl+A, then X**.
QEMU TCG executes firmware; MMIO to `0x80000000` / `0x80001000` / `0x80002000`
is forwarded to Verilator. Timer IRQs and UART bytes cross the socket. Guest
`mcycle` on this path is **QEMU's counter**, not Ibex RTL pipeline time.
Socket override: `IBEXDEMO_COSIM_SOCKET=/tmp/my.sock ./ibex-vp-cosim/run_cosim.sh`.

**Status on the prepared Mac:** `Vcosim_periph_server` is not built, and
`patches/0004-tcg-periph-cosim-qemu.patch` does not apply cleanly to the
installed QEMU tree. Until that is resolved, `run_cosim.sh` exits with a build
message; use the full-SoC or matmul daemon demos.

## Other simulation modes

### QEMU only

The fast functional baseline; QEMU emulates both the CPU and peripherals:

```bash
"$QEMU" -M ibexdemo -nographic -kernel "$ELF"
```

Expected: `Hello World!` lines. Exit with **Ctrl+A, then X**.

### Standalone full-SoC Verilator

The same RTL simulator works without a QEMU connection when the socket option
is omitted. UART goes to `uart0.log` and the testbench's pseudo-terminal:

```bash
cd "$ROOT/ibex-demo-system/build/lowrisc_ibex_demo_system_0/sim-verilator"
./Vtop_verilator --meminit=ram,"$ELF"
```

Read `uart0.log` from another terminal and stop with Ctrl+C.

The GPIO-only server `Vcosim_gpio_server` remains for `scripts/test_gpio_client.py`.

## Build

| What | Command | Notes |
| --- | --- | --- |
| Matmul daemon, plugin, firmware | `./ibex-vp-cosim/build_matmul_plugin.sh` | enables QEMU plugins once, keeping existing configure options; then `make -C ibex-vp-cosim matmul` (incremental) |
| Full-SoC QEMU machine + Verilator model | `./ibex-vp-cosim/build_full_soc.sh` | applies patches 0001–0003, rebuilds QEMU, builds `Vtop_verilator` with FuseSoC |
| TCG + RTL peripherals | `./ibex-vp-cosim/build_periph_cosim.sh` | applies patch 0004 (see status above) |

`build_full_soc.sh` and `build_periph_cosim.sh` configure QEMU only when no
QEMU build exists, so they keep plugin support once `build_matmul_plugin.sh` has
enabled it. Each builder copies its bridge
sources from this repository into the external trees; maintain those files here,
because edits to the copies are replaced on the next build.

The prepared native environment and firmware build adjustments are documented
in [LOCAL_SETUP.md](LOCAL_SETUP.md). If you change hello-world firmware sources,
rebuild them with the commands there before launching again. The matmul
firmware rebuilds automatically through `make`.

## Dependencies for a fresh checkout

External source trees and tool installations are ignored by Git. You need:

- Git, a C/C++ compiler, Make, CMake, Python 3, Ninja, Meson, pkg-config, and Rust.
- Verilator and the lowRISC FuseSoC/Edalize Python dependencies.
- GLib, Pixman, zlib, libelf, and LZ4 development libraries. GLib is also
  needed to build the TCG plugin.
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
For current GCC and CMake, build the firmware and simulators with:

```bash
cmake -S "$ROOT/ibex-demo-system/sw/c" -B "$ROOT/ibex-demo-system/sw/c/build" \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  '-DCMAKE_C_FLAGS=-march=rv32imc_zicsr_zifencei -mabi=ilp32 -mcmodel=medany -Wall -fvisibility=hidden -ffreestanding' \
  '-DCMAKE_ASM_FLAGS=-march=rv32imc_zicsr_zifencei -mabi=ilp32'
cmake --build "$ROOT/ibex-demo-system/sw/c/build" --target demo
./ibex-vp-cosim/build_full_soc.sh
./ibex-vp-cosim/build_matmul_plugin.sh
```

The builders never download dependencies. Fresh builds require libraries
discoverable by your compiler and pkg-config. `build_full_soc.sh` adds the local
macOS libelf/LZ4 paths when `local/env/lib/libelf.a` is present.

## Verification

```bash
python3 ibex-vp-cosim/scripts/test_matmul_daemon.py   # 19 tests, ~3 s
python3 ibex-vp-cosim/scripts/test_full_soc.py        # 7 tests, ~3 s
```

**Matmul daemon (19 tests):**

- Daemon lifecycle: detach, pidfile, owner-only socket, SIGTERM cleanup,
  refusing a second daemon, stale-socket recovery.
- Protocol: GEMM correctness for N = 1..64, fragmented requests, every error
  code, six concurrent clients.
- End to end in QEMU: firmware PASS, QEMU refusing to start without a daemon,
  firmware failing cleanly without the plugin, reconnecting after a daemon
  restart, a hung daemon timing out into a guest error.
- The launcher and control scripts.

Each test uses its own temporary socket, so it doesn't disturb a running demo
daemon.

**Full SoC (7 tests):** real RTL firmware output, UART interrupt echo, absence
of a QEMU CPU, start-paused behavior, fragmented socket requests, bounded cycle
execution, invalid requests/responses, disconnects, and launcher cleanup.

Both suites pass on the prepared Apple Silicon Mac. Build logs are in `deps/`.

## Troubleshooting

**Matmul daemon demo**

- **`cannot connect to matmul daemon at …` and QEMU exits:** no daemon on that
  socket. Run `./ibex-vp-cosim/matmul_daemon.sh start`, and check that
  `IBEX_MATMUL_SOCKET` is the same in both terminals.
- **QEMU rejects `-plugin`, or the plugin fails to load:** QEMU was built
  without plugin support. Run `./ibex-vp-cosim/build_matmul_plugin.sh`.
- **`a daemon is already listening on …`:** one is already running; reuse it, or
  `matmul_daemon.sh stop` first.
- **`… exists and is not a socket`:** the daemon will not delete a regular file
  at the socket path. Remove it yourself or pick another `IBEX_MATMUL_SOCKET`.
- **`socket path too long`:** macOS limits Unix socket paths to 103 bytes; use a
  short path under `/tmp`.
- **Guest prints `no response (is the plugin loaded?)`:** QEMU ran without
  `-plugin …matmul_plugin.so`.
- **Guest prints `accelerator error 3`:** the daemon disappeared or did not answer
  within `timeout_ms`. Check `matmul_daemon.sh log`.
- **What the daemon did:** `matmul_daemon.sh log` (or `-d plugin` on QEMU for the
  plugin side).

**Verilator cosim modes**

- **Missing tools or libraries:** source `env.sh` and review `LOCAL_SETUP.md`.
- **Old simulator binaries:** run `ibex-vp-cosim/build_full_soc.sh`. QEMU's machine
  list must contain `ibexdemo-soc-cosim`; the Verilator help must show
  `--qemu-cosim-socket=PATH`.
- **`run_cosim.sh` asks to build the TCG peripheral cosim:** that mode is not
  built here; see [its status](#qemu-tcg-cpu--verilator-peripherals).
- **No UART output:** inspect the printed run directory's `verilator.log`; confirm
  the same ELF works in standalone RTL mode and QEMU has not been paused with `-S`.
- **Socket already exists:** stop its owning process before removing a stale
  socket. By default each full-SoC launch gets its own socket directory under `/tmp`.
- **Protocol errors or response timeout:** rebuild both simulators together. The
  QEMU client rejects incompatible responses and stops if no response arrives
  within ten seconds; very slow hosts or waveform-heavy runs may hit that limit.
- **Using `-kernel` with full-SoC QEMU:** pass the ELF to `run_full_soc.sh`
  instead. Verilator loads and executes it; this QEMU machine has no CPU or
  emulated RAM.

## Source map

**Matmul accelerator daemon**

- `ibex-vp-cosim/matmul_daemon/matmul_daemon.c`: the daemon.
- `ibex-vp-cosim/tcg_plugin/matmul_plugin.c`: TCG plugin, the daemon's client.
- `ibex-vp-cosim/tcg_plugin/matmul_accel.h`: guest mailbox layout and status codes.
- `ibex-vp-cosim/include/matmul_daemon_protocol.h`: plugin-to-daemon socket protocol.
- `ibex-vp-cosim/sw/matmul_accel/main.c`: Ibex firmware that offloads and checks GEMMs.
- `ibex-vp-cosim/matmul_daemon.sh`: start/stop/restart/status/log/foreground.
- `ibex-vp-cosim/run_matmul_plugin.sh`, `build_matmul_plugin.sh`, `Makefile`
  (`matmul` targets): demo launcher and builds.
- `ibex-vp-cosim/scripts/test_matmul_daemon.py`: tests.

**Full-SoC RTL**

- `ibex-vp-cosim/full_soc/soc_cosim.h`: RTL stepping server and UART byte queues.
- `ibex-vp-cosim/full_soc/ibexdemo_soc_cosim.c`: QEMU machine and asynchronous client.
- `ibex-vp-cosim/include/cosim_soc_protocol.h`: versioned, little-endian socket protocol.
- `patches/0003-full-soc-verilator-hooks.patch`: simulator registration and UART DPI hooks.
- `ibex-vp-cosim/build_full_soc.sh`, `run_full_soc.sh`: builds and launcher.
- `ibex-vp-cosim/scripts/test_full_soc.py`: tests.

**TCG CPU + RTL peripherals**

- `ibex-vp-cosim/periph/ibexdemo_periph_bridge.c`: QEMU MMIO bridge.
- `ibex-vp-cosim/cosim_periph_main.cc`, `rtl/cosim_periph_top.sv`: Verilator server.
- `ibex-vp-cosim/include/cosim_protocol.h`: socket protocol.
- `patches/0004-tcg-periph-cosim-qemu.patch`, `build_periph_cosim.sh`,
  `run_cosim.sh` / `run_periph_cosim.sh`.

**Ibex RTL matmul:** `ibex-vp-cosim/sw/matmul/`, `build_matmul.sh`,
`run_matmul.sh`, `run_matmul_sweep.sh`.

## Further reading

- [CHEATSHEET.md](CHEATSHEET.md): presentation commands.
- [docs/MATMUL_DAEMON.md](docs/MATMUL_DAEMON.md): matmul daemon protocol and behavior.
- [docs/MATMUL_DEEP_DIVE.md](docs/MATMUL_DEEP_DIVE.md): presenter notes for a code walkthrough.
- [ibex-vp-cosim/README.md](ibex-vp-cosim/README.md): full-SoC protocol details.
- [docs/LOCKSTEP_DESIGN.md](docs/LOCKSTEP_DESIGN.md): a proposed QEMU-CPU
  instruction/cycle synchronization mode. It specifies per-edge handshakes, MMIO
  completion stalls, clock accounting, and required QEMU core changes. It is a
  design, not an implemented mode or a claim that instruction counting
  reproduces physical Ibex CPU timing.
