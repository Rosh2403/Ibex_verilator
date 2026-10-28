# Deep dive: matmul accelerator daemon behind a QEMU TCG plugin

Presenter notes. Each section is roughly one slide or one talking point, and
code references use `file:line` so you can open them live. Protocol reference
and test list: [MATMUL_DAEMON.md](MATMUL_DAEMON.md).

---

## 1. One-sentence summary

An Ibex firmware running in QEMU offloads matrix multiplies to an accelerator
that is a separate host process. A QEMU TCG plugin connects the two: it notices
the guest's doorbell write, ships the matrices over a Unix socket, and writes
the answer back before the guest runs its next instruction.

Three things to land with the audience:

1. **Separation.** The accelerator is its own process, which can run as a
   daemon. QEMU doesn't link it, and the plugin does no math.
2. **No QEMU source changes.** It uses only the public plugin API. The only
   requirement is QEMU built with `--enable-plugins`.
3. **Deterministic and robust.** The guest waits synchronously, and failures turn
   into guest-visible error codes instead of hangs. 22 automated tests cover it,
   including a GDB session against the running VP.

---

## 2. Where this fits in the repository

The repo has grown through several simulation modes (see `git log`):

| Commit | Mode | CPU runs in | Peripherals run in |
| --- | --- | --- | --- |
| `e4307ca` | QEMU + Verilator GPIO | QEMU TCG | GPIO in Verilator |
| `a3ac4c1` | Full-SoC cosim | Verilator RTL | Verilator RTL |
| `f938bb0`, `56a9e12` | TCG + RTL peripherals | QEMU TCG | GPIO/UART/timer in Verilator |
| `9de0957` | TCG plugin GEMM | QEMU TCG | matmul computed **inside the plugin** |
| this work | **TCG plugin + daemon** | QEMU TCG | matmul in a **separate daemon process** |

This work builds on `9de0957` from the `ibex-matmul` branch. That version did
the GEMM directly inside the plugin. The change moves the accelerator out of
QEMU's address space and turns the plugin into a thin client.

Why it matters: an out-of-process accelerator can later be replaced by anything
that speaks the same socket protocol, such as a Verilator RTL model, a Python
model, or a model on another machine. QEMU and the firmware stay unchanged.

---

## 3. Architecture

```text
 qemu-system-riscv32 -M ibexdemo  (process 1)          matmul_daemon  (process 2)
 +--------------------------------------------+        +------------------------------+
 | vCPU thread                                |        | main thread                  |
 |   Ibex firmware (TCG-translated RISC-V)    |        |   poll(listener, wake pipe)  |
 |     store 1 -> 0x0011C014  (doorbell)      |        |   accept -> spawn thread     |
 |        |                                   |        |                              |
 |        v  memory callback                  |  Unix  | client thread (one per QEMU) |
 |   matmul_plugin.so                         | socket |   read header + payload      |
 |     read mailbox + A, B from guest RAM     |------->|   validate                   |
 |     send GEMM, wait (deadline)             |        |   C = A * B                  |
 |     write C, ERRCODE, STATUS to guest RAM  |<-------|   reply GEMM_DONE + timing   |
 +--------------------------------------------+        +------------------------------+
```

Files and what they own:

| File | Role |
| --- | --- |
| `ibex-vp-cosim/tcg_plugin/matmul_accel.h` | guest ABI: mailbox addresses, status and error codes |
| `ibex-vp-cosim/sw/matmul_accel/main.c` | firmware that uses the accelerator and checks results |
| `ibex-vp-cosim/tcg_plugin/matmul_plugin.c` | QEMU plugin: doorbell detection, guest memory access, socket client |
| `ibex-vp-cosim/include/matmul_daemon_protocol.h` | wire protocol shared by plugin and daemon |
| `ibex-vp-cosim/matmul_daemon/matmul_daemon.c` | the accelerator daemon |
| `ibex-vp-cosim/matmul_daemon.sh` | start / stop / status / log / foreground |
| `ibex-vp-cosim/run_matmul_plugin.sh` | one-command demo |
| `ibex-vp-cosim/build_matmul_plugin.sh`, `Makefile` | builds, including enabling QEMU plugins |
| `ibex-vp-cosim/scripts/test_matmul_daemon.py` | 22 tests |
| `ibex-vp-cosim/scripts/bench_npu.py` | daemon vs direct (in-QEMU) benchmark |

There are two interfaces, and each has its own header. Keeping them apart is
deliberate:

- **Guest ↔ plugin:** `matmul_accel.h`. This is what firmware developers see.
- **Plugin ↔ daemon:** `matmul_daemon_protocol.h`. This is what accelerator
  developers see.

The guest never sees the socket, and the daemon never sees guest addresses.

---

## 4. Life of one request (the core walkthrough)

Present this as a sequence. It is the heart of the deep dive.

```text
Firmware                     Plugin (inside QEMU)                Daemon
--------                     --------------------                ------
write N, A, B, C, ERRCODE=0
write STATUS = IDLE
store 1 -> KICK ----------->  vcpu_mem_cb: addr == KICK && store?
                              lock; handle_kick()
                              read mailbox (N, A, B, C)
                              check 1 <= N <= 32
                              read A, B from guest RAM
                              send HEADER(GEMM, seq) + n + A + B --->  read header, check magic/version/len
                                                                        read payload, check n and length
                                                                        gemm(): C = A * B
                              <--- HEADER(GEMM_DONE, seq) + MACs + ns + C
                              check type/seq/len/n
                              write C, ERRCODE, STATUS = DONE
                              unlock; return
next instruction:
read STATUS  -> DONE (on the first read)
compare C with own GEMM
```

Code to show for each step:

1. **Firmware kick:** `sw/matmul_accel/main.c:49` (`kick()`). It writes the
   mailbox, then the doorbell, then polls `STATUS` with a bound
   (`POLL_LIMIT`, line 14). The bound only matters if the plugin is missing.
   Otherwise the first read already sees `DONE`.
2. **Doorbell detection:** `matmul_plugin.c:429` (`vcpu_tb_trans`). When QEMU
   translates a block of guest code, the plugin attaches a store-only memory
   callback to every instruction. At run time `vcpu_mem_cb` (line 416) ignores
   everything except a store to `0x0011C014`.
3. **Handling the kick:** `matmul_plugin.c:317` (`handle_kick`). It reads guest
   memory with `qemu_plugin_read_memory_vaddr`, validates, sends, receives,
   and writes back with `qemu_plugin_write_memory_vaddr`.
4. **Request/response on the wire:** `matmul_plugin.c:194` (`transact`). It
   assigns a sequence number, sends the header and payload, and waits for the
   reply under one deadline. It checks that the reply's magic, version and
   `seq` match and that it fits the buffer.
5. **Daemon side:** `matmul_daemon.c:199` (`client_thread`) frames and
   validates, and `matmul_daemon.c:141` (`handle_gemm`) does the work.
6. **Math:** `include/matmul_gemm.h:13` (`mmd_gemm`). It uses `uint32_t` arithmetic so
   overflow wraps exactly like RV32 integer multiply and add, uses i-k-j loop
   order so rows of B are read sequentially, and is timed with a nanosecond
   clock.
7. **Result check:** `main.c:69` (`run_size`) recomputes every element on the
   guest and prints a checksum.

Why the guest sees `DONE` immediately: the memory callback runs on the vCPU
thread. The guest cannot execute its next instruction until `handle_kick`
returns, so the accelerator looks instantaneous and the results are
deterministic.

---

## 5. Component deep dives

### 5.1 Guest ABI: `matmul_accel.h`

- A RAM mailbox at `0x0011C000`: `N`, `A`, `B`, `C`, `STATUS`, `KICK`,
  `ERRCODE`.
- `STATUS`: 0 idle, 1 done, 2 error. `ERRCODE` says why: bad N, guest memory,
  daemon I/O, or daemon rejected.
- The guest limit is N ≤ 32, below the daemon's 64. A `_Static_assert` in the
  plugin enforces guest limit ≤ daemon limit.

**Why a RAM mailbox and not a real MMIO device?** Plugins observe; they can't
add devices to the machine. A RAM mailbox needs no QEMU source change, and the
same firmware runs unchanged on stock `-M ibexdemo`. The cost is that without
the plugin, writes to the mailbox silently succeed. The firmware handles this
with its bounded poll and a clear "is the plugin loaded?" message.

### 5.2 Firmware: `sw/matmul_accel/main.c`

- `main.c:134`: refuses to run if the firmware's memory (`_bss_end`) reaches
  into the mailbox. Without this check a large program would quietly corrupt
  its own data.
- Test data comes from a small linear congruential generator in -128..127, so
  results are reproducible. The guest's own check can't overflow `int32`, which
  would be undefined behavior in C.
- Sizes 4, 8, 16 and 32, then a deliberate N=33 to prove the error path.
- `main.c:117` (`finish`) exits QEMU through the `sim_ctrl` register at
  `0x20000`. Bit 0 halts, and bits 7:1 are the exit code, so PASS gives exit 0
  and FAIL gives exit 1. The demo needs no timeout and scripts can check the
  result directly.

### 5.3 The plugin: `tcg_plugin/matmul_plugin.c`

**Lifecycle**

- `qemu_plugin_install` (line 457) parses `socket=` and `timeout_ms=`, then
  connects to the daemon. If that fails it returns -1, and **QEMU refuses to
  start** (line 499). Failing at startup is better than a guest error halfway
  through the demo.
- `connect_daemon` (line 235) does connect, `HELLO`/`HELLO_ACK`, and logs the
  daemon's PID and how many requests it has served. That makes it obvious in
  the demo that QEMU is talking to another process.
- `plugin_exit` prints counts of kicks, completed requests and failures.

**I/O design: every wait has a deadline**

- `wait_ready` (line 110) uses `poll` with a deadline. `send_full` (line 140)
  and `recv_full` (line 159) both use it, so a stuck daemon can't freeze QEMU.
- The socket is `O_NONBLOCK` (line 260). This came out of a bug found in
  testing; see section 7.
- SIGPIPE is suppressed (`SO_NOSIGPIPE` on macOS, `MSG_NOSIGNAL` on Linux),
  so writing to a dead daemon returns an error instead of killing QEMU.

**Error recovery**

- After any transport failure, `transact` drops the connection, because the
  byte stream is in an unknown state.
- `handle_kick` (line 367) retries **once** on a fresh connection. This is safe
  because a GEMM request is a pure function and has no side effects in the
  daemon. In other words, the request is idempotent.
- If both attempts fail, the guest sees `STATUS = ERROR`, `ERRCODE = 3`.

**Thread safety:** a mutex around the kick handler (line 424) protects the
socket and static buffers if the machine ever had more than one vCPU.

### 5.4 Protocol: `include/matmul_daemon_protocol.h`

- A 16-byte header (`magic`, `version`, `type`, `seq`, `len`) followed by
  `len` payload bytes. Everything is little-endian, with conversion helpers so
  it would still work on a big-endian host.
- `_Static_assert`s pin every struct size, so a compiler or padding change
  can't silently break the wire format.
- Five message types: `HELLO`, `HELLO_ACK`, `GEMM`, `GEMM_DONE`, `ERROR`.
- **Two classes of error, a deliberate design point:**
  - *Semantic* errors (bad N, unknown type): reply with `ERROR`, keep the
    connection.
  - *Framing* errors (bad magic or version, oversized or mismatched length):
    reply with `ERROR`, then **close**. Once framing is wrong, the next byte
    can't be trusted to start a header.

### 5.5 The daemon: `matmul_daemon/matmul_daemon.c`

**Startup** (`main`, line 533):

1. Ignore SIGPIPE (line 584). Install SIGTERM/SIGINT/SIGHUP handlers that use
   the self-pipe trick: the handler writes a byte to a pipe, which wakes the
   main loop's `poll`.
2. `daemonize` (line 433, called at 600): fork, `setsid`, fork again. The
   **original** process waits on a readiness pipe and exits 0 only after the
   grandchild writes `R`. So when `matmul_daemon --daemon` returns, the socket
   is already listening, and there is no race with QEMU connecting.
3. `open_listener` (line 355, called at 603) runs **before** stdio is
   redirected, so bind errors still reach your terminal.
   - `clear_stale_socket` (line 322): if something answers on the path, refuse
     ("already listening"). If connect is refused, the socket file is stale
     from a crash, so remove it. If the path isn't a socket at all, never
     delete it.
   - `umask(077)` (line 369) makes the socket owner-only (mode 0600), so other
     users on the machine can't send work.
   - It records the socket's device and inode numbers for safe cleanup later.
4. Redirect stdio to the log and write the pidfile (line 608), then signal ready.

**Serving** (line 623): `poll` on the listener plus the wake pipe. Each
connection gets a detached thread (`client_thread`), so several QEMU instances
can share one daemon. A mutex protects only the shared counters.

**Shutdown:** close the listener, then `remove_own_socket` (line 390), which
removes the socket only if its inode still matches the one this process
created. That way a daemon being stopped never deletes a newer daemon's
socket. The pidfile gets the same treatment (`remove_own_pidfile`, which checks
the PID inside).

**`--ping`** (line 486): a built-in client that does `HELLO` and prints the
PID and count served. The control script uses it as the source of truth.

### 5.6 Scripts and build

- `Makefile` targets `matmul`, `matmul-daemon`. These are incremental builds.
  The plugin is linked with `-Wl,-undefined,dynamic_lookup` on macOS because
  plugin API symbols resolve against the QEMU executable at load time.
- `build_matmul_plugin.sh`: if QEMU lacks `CONFIG_PLUGIN`, runs
  `./config.status --enable-plugins`, which re-runs configure with the
  **original** options plus plugins, then rebuilds. This is a one-time cost.
- `matmul_daemon.sh`: `start`, `stop`, `restart`, `status`, `log [-f]`,
  `foreground`. `stop` asks the daemon for its PID over the socket and only
  falls back to the pidfile after checking the process really is
  `matmul_daemon`. Stale PIDs never cause the wrong process to be killed.
- `run_matmul_plugin.sh`: builds, reuses a running daemon or starts one, runs
  QEMU with a watchdog (`QEMU_TIMEOUT`, default 60 s), prints the daemon log
  lines for this run, and stops the daemon only if it started it
  (`KEEP_DAEMON=1` overrides). Traps make sure Ctrl+C doesn't leave QEMU running.

---

## 6. Design decisions and trade-offs

| Decision | Why | Cost |
| --- | --- | --- |
| Accelerator in a separate process | Isolation, swappable models, crash containment, can be shared by several QEMUs | One socket round trip per request: +11–21 µs per request in the benchmark |
| Synchronous request inside the store callback | Deterministic; the guest always sees a finished result | No overlap of guest compute and accelerator compute |
| Unix socket, own binary protocol | Local, fast, file permissions for access control, no dependencies | Same machine only (TCP would be a small change) |
| RAM mailbox + store callback | Plugin API only, no QEMU patch | Without the plugin the doorbell does nothing (firmware detects it) |
| `uint32_t` wrap-around math | Bit-exact match with the guest's own result | None |
| Retry once on a fresh connection | Survives a daemon restart; safe because GEMM is idempotent | At most 2× timeout before the guest sees an error |
| Fail QEMU at startup if no daemon | Clear error before the guest runs | Daemon must be started first (the launcher does it) |
| Thread per client in the daemon | Simple; one slow client can't block others | A thread per QEMU instance (fine at this scale) |

Benchmark (`scripts/bench_npu.py`, Apple M3 Pro, median of 7 runs, 2,000
requests per size). The "direct" build is the same plugin compiled with
`-DMATMUL_PLUGIN_DIRECT`, which runs the same kernel inside QEMU:

| Matrix | Direct (in QEMU) | Daemon | Daemon overhead |
| --- | --- | --- | --- |
| 4×4 | 3.0 µs | 14.1 µs | +11.1 µs |
| 32×32 | 6.3 µs | 27.0 µs | +20.6 µs |

Guest work with no NPU requests is unaffected by the choice: both builds add
2–4% over running without a plugin. **Per request, the direct implementation is
faster; communication dominates for small matrices.** The daemon earns its
cost through isolation, swappable or heavy models, and sharing. It could only
win on speed with asynchronous requests (extension 3), which let the guest keep
running while the NPU computes. That is the usual offload trade-off and a good
discussion point.

---

## 7. Development story: problems found and fixed

These make good deep-dive anecdotes, because each shows a design choice
driven by evidence:

1. **QEMU had no plugin support.** The existing build had `#undef
   CONFIG_PLUGIN`. Fix: reconfigure in place with `config.status
   --enable-plugins` so every existing option is kept. I then reran the
   full-SoC tests to confirm nothing regressed.
2. **Timing showed "0 ns".** On macOS, `CLOCK_MONOTONIC` has microsecond
   resolution. Fix: use `CLOCK_UPTIME_RAW` on macOS, which has nanosecond
   resolution.
3. **QEMU froze when the daemon stopped responding.** The "hung daemon" test
   found it. A 32×32 request (~8 KB) fills macOS's 8 KiB Unix socket buffer,
   so `send()` blocked forever. The timeout covered only the receive side. A
   stack sample showed the vCPU thread stuck in `__sendto`. The first fix,
   `MSG_DONTWAIT`, still blocked, and a 10-line probe program proved **macOS
   ignores `MSG_DONTWAIT` on Unix stream sockets**. The final fix makes the
   socket `O_NONBLOCK` and puts sends under the same deadline. The lesson:
   write the failure-mode test, not just the happy path.
4. **Pidfile trust.** The first `stop` killed whatever PID was in the pidfile.
   If the daemon had died and the PID was reused, that could kill an unrelated
   process. Fix: ask the daemon for its PID over the socket, and verify the
   process name before using the fallback.
5. **A silent first build.** The launcher hid build output, so a first-run
   QEMU rebuild (several minutes) looked like a hang. Fix: show it.

---

## 8. Testing strategy

`python3 ibex-vp-cosim/scripts/test_matmul_daemon.py`: 22 tests in about 5 s.
Each test uses its own temporary socket, so tests can't interfere with each
other or with a running demo daemon.

| Layer | What is proven | Tests (line) |
| --- | --- | --- |
| Daemon lifecycle | detach, pidfile, 0600 socket, SIGTERM cleanup, refusing a second daemon, stale-socket recovery, never deleting a non-socket file | 164–205 |
| Protocol correctness | N = 1..64 against a Python reference with full-range int32 and wrap-around; requests sent 5 bytes at a time | 214–234 |
| Protocol errors | each error code, and which errors keep or close the connection | 246–265 |
| Concurrency | 6 clients × 15 random GEMMs at once; total count checked | 283 |
| QEMU end to end | firmware PASS via the daemon; QEMU refuses to start without a daemon; firmware fails cleanly without the plugin | 317–339 |
| Recovery | restart the daemon while QEMU is paused (`-S`, resumed via the monitor socket); the plugin logs `Broken pipe`, reconnects, PASS | 345 |
| Timeouts | a fake daemon that never answers: guest gets error 3 and QEMU exits instead of hanging | 375 |
| Benchmark baseline | the direct (in-QEMU) build produces the same checksums as the daemon; `log=summary` prints per-size timing | 408–419 |
| GDB | QEMU's GDB server with the daemon plugin: break in `kick()`, run the request, read STATUS and C, run to exit | 433 |
| Scripts | launcher starts and stops its own daemon; reuses an existing one; count accumulates | 475–486 |

Talking point: the recovery and timeout tests aren't for show. The timeout test
is what found bug 3.

---

## 9. Live demo script

Before presenting, run the demo once so everything is built:

```bash
cd ~/Ibex_verilator && source ./env.sh
./ibex-vp-cosim/run_matmul_plugin.sh        # expect "Result: PASS"
```

**Demo A: one command (about 1 minute)**

```bash
./ibex-vp-cosim/run_matmul_plugin.sh
```

Point out, in order:

- `matmul daemon started: pid …`: a separate process.
- `[matmul plugin] connected to daemon pid …`: the same PID; QEMU is talking to it.
- `guest: kick 4x4 ... [matmul plugin] kick #1 …`: the plugin line appears
  *inside* the guest's line, which shows the guest is paused during the request.
- `compute 3.8 us, round trip …`: communication costs more than the math. (The
  first request includes connection warm-up; the benchmark gives steady-state
  numbers.)
- `rejected` for N=33, then `PASS`, then the daemon's own log of the same requests.

**Demo B: two terminals (the daemon as a visible service)**

```bash
# Terminal A
./ibex-vp-cosim/matmul_daemon.sh foreground
# Terminal B, run twice
./ibex-vp-cosim/run_matmul_plugin.sh
./ibex-vp-cosim/run_matmul_plugin.sh
```

Point out the `client 1`, `client 2` connections and `[total 8]` growing across
runs: one long-lived accelerator, many simulations.

**Demo C: failure modes (optional, about 1 minute)**

```bash
# Stop terminal A with Ctrl+C, then:
"$QEMU" -M ibexdemo -nographic -kernel ibex-vp-cosim/build/matmul_accel.elf \
  -plugin ibex-vp-cosim/build/matmul_plugin.so,socket=/tmp/ibex-matmul-accel.sock
#   -> "cannot connect to matmul daemon ..." and QEMU exits

"$QEMU" -M ibexdemo -nographic -kernel ibex-vp-cosim/build/matmul_accel.elf
#   -> no plugin: "no response (is the plugin loaded?)", FAIL, exit 1

python3 ibex-vp-cosim/scripts/test_matmul_daemon.py   # 22 tests, about 5 s
python3 ibex-vp-cosim/scripts/bench_npu.py            # daemon vs direct, about 1 minute
```

---

## 10. Limitations and next steps

**Current limits (state these plainly):**

- **Functional only.** Guest time doesn't advance for accelerator work, so
  `mcycle` won't reflect it. The daemon reports compute time, but nothing feeds
  it back into the simulation.
- **Addresses are treated as physical.** That is correct for this M-mode,
  no-MMU firmware. With an MMU the plugin would need to translate addresses.
- **Callback overhead.** A store callback is attached to every guest
  instruction. Checking for store opcodes at translation time would cut that.
- **Same machine only.** It uses a Unix socket.

**Natural extensions:**

1. **RTL accelerator in Verilator behind the same protocol.** Swap the C
   `gemm()` for a Verilated matmul block. The plugin and firmware don't change.
   This is the strongest argument for the process split.
2. **Timing model.** Have the daemon return modeled cycles, then advance guest
   time (for example with `-icount`) or expose a cycle counter in the mailbox.
3. **Asynchronous mode.** Return from the doorbell immediately and complete
   later, so the guest can overlap work and practice interrupt-driven drivers.
4. **TCP transport** for an accelerator on another machine or an FPGA host.
5. **Shared memory** for large matrices instead of copying through the socket.

---

## 11. Likely questions

**Why not write a QEMU device model instead of a plugin?**
A device model means patching and rebuilding QEMU, and it ties the accelerator
to QEMU's internals. The plugin uses a stable, versioned API (plugin API v5),
and the accelerator stays independent of QEMU. A device model would be the
better choice for interrupts or real MMIO semantics.

**Is this cycle-accurate?**
No. It is functionally exact (bit-exact results) with no timing. For Ibex cycle
counts use the Verilator paths (`run_matmul.sh`). See the limits above.

**What happens if the daemon crashes mid-run?**
The next request gets a broken pipe. The plugin reconnects once, which works if
the daemon has been restarted. Otherwise the guest sees `STATUS=ERROR,
ERRCODE=3` and decides what to do. QEMU itself never crashes or hangs. Both
cases are tested.

**Can several QEMUs share one accelerator?**
Yes. The daemon serves each connection on its own thread, and the concurrency
test runs 6 clients at once.

**Is it secure?**
The socket is owner-only (0600), so other local users can't connect. Every
length and size is checked before use, and malformed framing closes the
connection. It is meant for a local development machine, not a hostile network.

**Why does a request take 14–27 µs through the daemon when the math takes 3–6 µs?**
Two process context switches plus socket copies. For small matrices the
overhead dominates. That is the same reason real systems batch work or use
shared memory, and it is extension 5 above.

**Why little-endian everywhere?**
RISC-V and both test hosts are little-endian, so conversions compile away. The
helpers keep it correct on a big-endian host for free.
