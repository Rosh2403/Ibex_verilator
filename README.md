# Ibex VP with a daemon-based NPU (QEMU TCG plugin)

A virtual platform (VP) for the lowRISC Ibex Demo System with an NPU that runs
as **its own host process**. The two are connected by a **QEMU TCG plugin**.

- **VP:** QEMU's `ibexdemo` machine runs the Ibex RISC-V firmware with TCG.
- **NPU:** `matmul_daemon`, a separate process that performs integer
  matrix multiplies (GEMM) for any number of VP instances.
- **Connection:** `matmul_plugin.so`, loaded into QEMU with `-plugin`. When the
  firmware writes the NPU doorbell, the plugin sends the matrices to the daemon
  over a Unix socket and writes the result back into guest memory.

```text
 qemu-system-riscv32 -M ibexdemo (the VP)              matmul_daemon (the NPU)
 +-----------------------------------+                +------------------------------+
 | Ibex firmware (TCG)               |                | Unix socket server           |
 |   write N, A, B, C to the mailbox |  GEMM request  | one thread per VP            |
 |   store 1 -> 0x0011C014 (doorbell)+--> plugin ---->|   C = A * B (int32, wrapping)|
 |   read STATUS, use C              |<-- plugin <----|   logs every request         |
 +-----------------------------------+   C + timing   +------------------------------+
```

The firmware waits at the doorbell store until the NPU answers, so results are
deterministic. NPU timing isn't modeled; results are bit-exact. No QEMU source
patch is needed, only QEMU built with plugin support.

**Contents:** [Quick start](#quick-start) · [Set up from scratch](#set-up-from-scratch) ·
[Running](#running-the-npu-and-the-vp) · [Debugging with GDB](#debugging-with-gdb) ·
[Performance](#performance) · [Programming the NPU](#programming-the-npu-from-firmware) ·
[Tests](#tests) · [Troubleshooting](#troubleshooting) · [Source map](#source-map)

## Quick start

On a machine that is already set up. Every command in this README runs from the
repository folder, so `cd` into it first in every new terminal:

```bash
cd ~/Ibex_verilator                      # the folder you cloned into
source ./env.sh
./ibex-vp-cosim/run_matmul_plugin.sh     # starts the NPU daemon, runs the VP; ends with "Result: PASS"
```

## Set up from scratch

From an empty directory on **Ubuntu or Debian (x86-64)**. The longest step is
the one-time QEMU build (10–20 minutes). Everything except the `apt` packages
is installed inside the repository.

**1. System packages**

```bash
sudo apt update
sudo apt install -y git curl xz-utils build-essential ninja-build pkg-config \
  python3 python3-venv python3-tomli rustc \
  libglib2.0-dev libpixman-1-dev zlib1g-dev libfdt-dev
```

QEMU needs GLib, Pixman, zlib, and libfdt. This QEMU branch also wants a Rust
compiler while configuring, even though Rust is disabled.

**2. This repository**

```bash
git clone -b ibex-matmul-daemon https://github.com/Rosh2403/Ibex_verilator.git
cd Ibex_verilator
source ./env.sh            # sets ROOT and QEMU; adds toolchain/bin to PATH
```

**3. RISC-V toolchain** (lowRISC's prebuilt GCC and GDB, unpacked into `toolchain/`)

```bash
mkdir -p toolchain
curl -L https://github.com/lowRISC/lowrisc-toolchains/releases/download/20220524-1/lowrisc-toolchain-gcc-rv32imcb-20220524-1.tar.xz \
  | tar -xJ -C toolchain --strip-components=1
riscv32-unknown-elf-gcc --version
```

Any bare-metal RV32IMC GCC named `riscv32-unknown-elf-*` works; the build
detects which ISA naming it uses.

**4. QEMU and the Ibex Demo System sources** (pinned revisions)

```bash
git clone https://github.com/lowRISC/qemu.git qemu
git -C qemu checkout d7655281a649abb339f5267f0b9e05329b3cee6d       # ot-10.2.0
git clone https://github.com/lowRISC/ibex-demo-system.git ibex-demo-system
git -C ibex-demo-system checkout d37beb753e19482eb054aefac73a27289bf72e47
```

From ibex-demo-system, the firmware only uses the startup code, small drivers
(`sw/c/common/`), and the linker script.

**5. Configure QEMU with TCG plugin support**

```bash
mkdir -p qemu/build
(cd qemu/build && ../configure --target-list=riscv32-softmmu \
  --without-default-features --enable-tcg --enable-tools --disable-rust \
  --enable-plugins)
```

**6. Build and run** (still inside `Ibex_verilator/`)

```bash
./ibex-vp-cosim/run_matmul_plugin.sh                  # first run builds QEMU; ends with "Result: PASS"
python3 ibex-vp-cosim/scripts/test_matmul_daemon.py   # expect: Ran 22 tests ... OK
```

**macOS (Apple Silicon):** use the
[xPack RISC-V GCC](https://github.com/xpack-dev-tools/riscv-none-elf-gcc-xpack/releases)
for step 3. Unpack it the same way, then add the expected names:

```bash
(cd toolchain/bin && for t in riscv-none-elf-*; do
   ln -sf "$t" "riscv32-unknown-elf-${t#riscv-none-elf-}"; done)
```

Install the step 1 libraries with your package manager. The prepared Mac's exact
setup is in [LOCAL_SETUP.md](LOCAL_SETUP.md).

## Running the NPU and the VP

### One command

```bash
./ibex-vp-cosim/run_matmul_plugin.sh
```

The launcher does the following:

1. Rebuilds what changed.
2. Starts an NPU daemon if none is running.
3. Runs the VP with the plugin.
4. Prints the daemon's log for this run.
5. Stops the daemon if it started it.

The test firmware runs 4×4, 8×8, 16×16 and 32×32 GEMMs on the NPU. It checks
every element against its own calculation, checks that an oversized request is
rejected, and exits QEMU with 0 on PASS. Expected:

```text
matmul daemon started: pid 55602, socket /tmp/ibex-matmul-accel.sock, log ...
[matmul plugin] connected to daemon pid 55602 at /tmp/ibex-matmul-accel.sock ...
guest: kick 4x4 ... [matmul plugin] kick #1: 4x4 GEMM ... by daemon: 64 MACs, ...
ok  checksum FFFE8FC9
...
guest: kick N=33 (expect rejection) ... [matmul plugin] kick #5: rejected N=33 ...
rejected
PASS
--- matmul daemon log (/tmp/ibex-matmul-accel.sock.log) ---
... [matmul-daemon] client 1 seq 5: GEMM 32x32 (32768 MACs) in 3750 ns [total 4]
Result: PASS
```

| Option | Effect |
| --- | --- |
| `--gdb` (first argument) | start the VP paused for GDB; see [Debugging with GDB](#debugging-with-gdb) |
| extra arguments | passed to QEMU, e.g. `-d plugin` |
| `KEEP_DAEMON=1` | leave a daemon the launcher started running |
| `QEMU_TIMEOUT=N` | kill the VP after N seconds (default 60; 0 = never) |
| `IBEX_MATMUL_SOCKET=PATH` | NPU socket (default `/tmp/ibex-matmul-accel.sock`) |

### The NPU daemon as a long-running service

Two terminals show the NPU and the VP as separate processes:

```bash
./ibex-vp-cosim/matmul_daemon.sh foreground   # terminal A: NPU with live log; Ctrl+C stops it
./ibex-vp-cosim/run_matmul_plugin.sh          # terminal B: run the VP as often as you like
```

To run it in the background:

```bash
./ibex-vp-cosim/matmul_daemon.sh start        # detaches; returns once the socket is listening
./ibex-vp-cosim/matmul_daemon.sh status       # pid and GEMMs served
./ibex-vp-cosim/matmul_daemon.sh log -f       # follow its log
./ibex-vp-cosim/matmul_daemon.sh restart
./ibex-vp-cosim/matmul_daemon.sh stop         # SIGTERM; removes socket and pidfile
```

- One daemon serves many VPs: each QEMU connection gets its own thread.
- The socket is owner-only (mode 0600). A socket left behind by a crash is
  replaced on the next start.
- `stop` asks the daemon for its pid over the socket, so it never signals an
  unrelated process.

### Running the pieces by hand

```bash
B=ibex-vp-cosim/build
$B/matmul_daemon --socket /tmp/ibex-matmul-accel.sock --daemon   # omit --daemon to stay in the foreground
$B/matmul_daemon --socket /tmp/ibex-matmul-accel.sock --ping     # query it

"$QEMU" -M ibexdemo -nographic -kernel $B/matmul_accel.elf \
  -plugin $B/matmul_plugin.so,socket=/tmp/ibex-matmul-accel.sock
```

| Plugin argument | Effect |
| --- | --- |
| `socket=PATH` | daemon socket (default `$IBEX_MATMUL_SOCKET` or `/tmp/ibex-matmul-accel.sock`) |
| `timeout_ms=N` | deadline for sending each request and receiving its reply (default 5000) |
| `log=kicks` / `log=summary` | one line per request (default), or per-size timing totals at exit |

How failures are handled:

- QEMU refuses to start if no daemon answers.
- If the daemon restarts while the VP runs, the plugin reconnects once and
  resends the request. This is safe because a GEMM has no side effects.
- If the daemon stops answering, the firmware sees an error status instead of
  QEMU hanging.

## Debugging with GDB

QEMU's GDB server works unchanged with the plugin and daemon attached.
Breakpoints, stepping, and memory and register access behave as without the
plugin. A request runs to completion inside the doorbell store, so stepping over
that store returns with the result already in memory.

Terminal A:

```bash
./ibex-vp-cosim/run_matmul_plugin.sh --gdb     # VP paused, GDB server on port 1234 (GDB_PORT=...)
```

Terminal B:

```bash
source ./env.sh
riscv32-unknown-elf-gdb ibex-vp-cosim/build/matmul_accel.elf -ex 'target remote :1234'
(gdb) break kick
(gdb) continue                                  # stops before the first NPU request (n=4)
(gdb) print/x *(unsigned int *)0x0011C010       # STATUS = 0x0 (idle)
(gdb) finish                                    # runs the request through the daemon
(gdb) print/x *(unsigned int *)0x0011C010       # STATUS = 0x1 (done)
(gdb) print c[0]@4                              # first results from the NPU
(gdb) continue                                  # runs to the end; QEMU exits
```

Any QEMU GDB option works the same way, for example
`run_matmul_plugin.sh -s -S`. While the VP is paused in GDB the daemon simply
waits. Ubuntu's `gdb-multiarch` also works if your toolchain has no GDB.

## Performance

The NPU daemon is compared with a **direct** implementation: the same plugin
built with `-DMATMUL_PLUGIN_DIRECT`, which runs the GEMM inside QEMU. Both
variants share the same instrumentation, mailbox handling, and GEMM kernel; only
where the math runs differs. Reproduce with:

```bash
python3 ibex-vp-cosim/scripts/bench_npu.py      # median of 5 runs each (--runs N)
```

Results on an Apple M3 Pro, median of 7 runs:

**Guest I/O and CPU workload with no NPU requests** (2 s of firmware compute
and UART output):

| Configuration | Time | vs no plugin |
| --- | --- | --- |
| No plugin | 2.034 s | 1.00× |
| Direct implementation | 2.075 s | 1.02× |
| NPU daemon | 2.119 s | 1.04× |

Guest I/O and CPU performance are the same for the daemon and direct versions:
both add 2–4% for watching guest stores, and the order between them changes
from run to run.

**Time per NPU request, as seen by the VP** (2,000 requests per size):

| Matrix | Direct implementation | NPU daemon | Daemon overhead |
| --- | --- | --- | --- |
| 4×4 | 3.0 µs | 14.1 µs | +11.1 µs |
| 8×8 | 3.0 µs | 14.2 µs | +11.1 µs |
| 16×16 | 3.5 µs | 16.0 µs | +12.5 µs |
| 32×32 | 6.3 µs | 27.0 µs | +20.6 µs |

Each daemon request costs an extra 11–21 µs for the socket round trip and for
copying the matrices between processes. The VP waits for each request, so the
daemon can't hide that cost. Per request, the direct implementation is faster.

The daemon is worth that cost when:

- the NPU model is heavy or separate, for example a Verilator RTL NPU, another
  language, or another machine;
- several VPs share one NPU;
- an NPU crash must not take down QEMU.

The daemon can only beat the direct version on speed if requests are
asynchronous, so the guest keeps running while the NPU computes on another
core. That mode isn't implemented.

## Programming the NPU from firmware

The NPU is driven through a mailbox in guest RAM (`ibex-vp-cosim/tcg_plugin/matmul_accel.h`):

| Address | Register | Meaning |
| --- | --- | --- |
| `0x0011C000` | `N` | matrix size, 1–32 |
| `0x0011C004` | `A` | address of row-major `int32` A |
| `0x0011C008` | `B` | address of B |
| `0x0011C00C` | `C` | address for the result |
| `0x0011C010` | `STATUS` | 0 idle, 1 done, 2 error |
| `0x0011C014` | `KICK` | any store starts the request |
| `0x0011C018` | `ERRCODE` | 1 bad N, 2 guest memory, 3 daemon unreachable or timed out, 4 rejected |

```c
#include "matmul_accel.h"
#define REG(addr) (*(volatile uint32_t *)(addr))

REG(MATMUL_ACCEL_N) = n;
REG(MATMUL_ACCEL_A) = (uint32_t)a;
REG(MATMUL_ACCEL_B) = (uint32_t)b;
REG(MATMUL_ACCEL_C) = (uint32_t)c;
REG(MATMUL_ACCEL_STATUS) = MATMUL_ACCEL_IDLE;
REG(MATMUL_ACCEL_KICK) = 1;                       /* C is ready after this store */
if (REG(MATMUL_ACCEL_STATUS) != MATMUL_ACCEL_DONE) { /* see ERRCODE */ }
```

Firmware must end below `0x0011C000`. Full example:
`ibex-vp-cosim/sw/matmul_accel/main.c`. The socket protocol between plugin and
daemon is in [docs/MATMUL_DAEMON.md](docs/MATMUL_DAEMON.md).

## Tests

```bash
python3 ibex-vp-cosim/scripts/test_matmul_daemon.py    # 22 tests, about 5 s
```

The tests cover:

- **Daemon lifecycle:** detaching, pidfile, owner-only socket, cleanup,
  refusing a second daemon, stale sockets.
- **Protocol:** correctness for N = 1–64 and every error case; six concurrent VPs.
- **VP end to end:** no daemon, no plugin, a daemon restart while running, and a
  hung daemon.
- **GDB:** a debugging session against the VP with the daemon attached.
- **Direct baseline:** produces the same results as the daemon.
- **Scripts:** the launcher and control script.

Each test uses its own socket, so the tests don't disturb a running daemon.

## Troubleshooting

- **`./ibex-vp-cosim/…: No such file or directory`:** you're not in the
  repository folder. Run `cd ~/Ibex_verilator` (or wherever you cloned it) and
  `source ./env.sh`, then try again.
- **`riscv32-unknown-elf-gcc: command not found`:** run `source ./env.sh`, and
  check that `toolchain/bin/` exists (step 3).
- **`Invalid or unknown z ISA extension: 'zifencei'`:** handled automatically.
  To force a naming, use `make -C ibex-vp-cosim RV_MARCH=rv32imc`.
- **QEMU `configure` fails:** a step 1 package is missing; the error names it.
- **QEMU rejects `-plugin`, or the plugin won't load:** QEMU lacks plugin
  support. Run `./ibex-vp-cosim/build_matmul_plugin.sh`; it reconfigures QEMU
  once.
- **`cannot connect to matmul daemon at …` and QEMU exits:** start the daemon
  (`matmul_daemon.sh start`), and use the same `IBEX_MATMUL_SOCKET` everywhere.
- **`a daemon is already listening on …`:** reuse it, or run
  `matmul_daemon.sh stop` first.
- **`… exists and is not a socket` / `socket path too long`:** pick another
  `IBEX_MATMUL_SOCKET`, short and under `/tmp`.
- **Firmware prints `no response (is the plugin loaded?)`:** QEMU ran without
  `-plugin …matmul_plugin.so`.
- **Firmware prints `accelerator error 3`:** the daemon went away or didn't answer
  within `timeout_ms`. Check `matmul_daemon.sh log`.
- **GDB can't connect:** the VP must be started with `--gdb` (or `-s -S`), and
  the port must match (`GDB_PORT`).

## Source map

| File | Role |
| --- | --- |
| `ibex-vp-cosim/matmul_daemon/matmul_daemon.c` | NPU daemon |
| `ibex-vp-cosim/tcg_plugin/matmul_plugin.c` | QEMU TCG plugin connecting the VP to the NPU (and the direct baseline with `-DMATMUL_PLUGIN_DIRECT`) |
| `ibex-vp-cosim/include/matmul_daemon_protocol.h` | plugin-to-daemon socket protocol |
| `ibex-vp-cosim/include/matmul_gemm.h` | GEMM kernel |
| `ibex-vp-cosim/tcg_plugin/matmul_accel.h` | firmware-facing mailbox |
| `ibex-vp-cosim/sw/matmul_accel/main.c` | test firmware |
| `ibex-vp-cosim/sw/npu_bench/main.c` | benchmark firmware |
| `ibex-vp-cosim/matmul_daemon.sh` | daemon start, stop, status, log, foreground |
| `ibex-vp-cosim/run_matmul_plugin.sh` | one-command launcher (`--gdb` for debugging) |
| `ibex-vp-cosim/build_matmul_plugin.sh`, `ibex-vp-cosim/Makefile` | builds |
| `ibex-vp-cosim/scripts/test_matmul_daemon.py`, `scripts/bench_npu.py` | tests and benchmark |

More documentation:

- [docs/MATMUL_DAEMON.md](docs/MATMUL_DAEMON.md): protocol and failure handling.
- [docs/MATMUL_DEEP_DIVE.md](docs/MATMUL_DEEP_DIVE.md): presenter notes for a code walkthrough.
- [docs/OTHER_MODES.md](docs/OTHER_MODES.md): the repository's other simulation
  modes (full-SoC RTL in Verilator and others), which this NPU setup doesn't need.
