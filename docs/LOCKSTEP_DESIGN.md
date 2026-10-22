# Strict QEMU–Verilator synchronization design

Status: design specification. The `ibexdemo-lockstep` machine and QEMU core hooks
described here are **not implemented**. The existing runnable
`ibexdemo-soc-cosim` machine has no QEMU CPU and is a different architecture.
The existing `ibexdemo-cosim` machine forwards GPIO MMIO but does not synchronize
every instruction, account for bus wait cycles, or forward RTL interrupts.

## Accuracy contract

Use one QEMU vCPU, single-threaded TCG, fixed instruction counting, a single
clocked Verilator endpoint, and one serialized synchronization channel.
Loose real-time timers, free-running endpoint threads, and asynchronous posted
writes are prohibited in this mode.

Required QEMU options for the proposed machine:

```text
qemu-system-riscv32 -M ibexdemo-lockstep,quantum=1 \
  -accel tcg,thread=single -smp 1 \
  -icount shift=0,sleep=off -nographic -kernel firmware.elf
```

This is a proposed command, not a command supported by the current binary.
The machine must reject incompatible accelerator, CPU-count, instruction-count,
and quantum settings at startup instead of silently degrading synchronization.

`shift=0` means one virtual **nanosecond per counted instruction**, before clock
bias is added. It does not mean an Ibex instruction physically takes one clock
cycle. Pipeline stalls, multiply/divide latency, instruction-fetch delays, and
interrupt-entry timing are not reproduced by this option. QEMU's local
`docs/devel/tcg-icount.rst` explicitly distinguishes instruction counting from
cycle-accurate CPU emulation.

The proposed host model is therefore an explicitly defined, non-pipelined,
one-instruction-per-base-tick CPU with additional bus-wait and idle ticks. It can
be synchronized exactly with peripheral RTL **under that model**. It is not an
exact timing model of the Ibex CPU. If matching physical Ibex CPU cycles is a
hard acceptance criterion, retain the CPU in RTL, as in the existing full-SoC
mode, or provide and validate a separate Ibex microarchitectural timing model.

For the original 50 MHz peripheral configuration, one hardware tick represents
20 ns of physical model time. QEMU's 1 ns instruction unit is a logical unit;
mapping it to a 20 ns RTL period must be explicit. Peripheral frequency/baud
parameters must use the physical RTL period. Do not equate a shift-zero unit to
both 1 ns and a 50 MHz clock period. QEMU-owned guest timers in this mode must use
the same documented scaling or be disabled in favor of the RTL timers.

## Counters and ownership

Maintain separate counters:

- `I`: QEMU's instruction count, with explicit treatment of the current in-flight
  instruction. Do not call it an architectural retirement counter.
- `C`: committed common clock boundaries, incremented only after an RTL edge ACK.
- `W`: extra ticks spent waiting for a bus response; no additional instruction
  is executed during these ticks.
- `H`: ticks spent with the host halted in WFI; RTL clocks can still progress.
- `R`: reset ticks before host execution begins.

At a committed instruction boundary in the defined host model:

```text
C = R + I + W + H
QEMU virtual time = epoch_bias + I * 1 ns + (W + H + R) * 1 ns
RTL physical model time = C * rtl_period_ns
```

The initial instruction epoch and writable guest counters require explicit
offsets; guest writes to `mcycle` must not change the scheduler's private `C`.
`minstret`, `mcycle`, and `time` must not all be implemented as the same counter.
Route the cycle/time CSR views through the synchronized clock and preserve the
separate instruction counter. Audit the RISC-V counter helpers in `csr.c`.

It is impossible to require virtual time to equal only `I` while also advancing
hardware through multi-cycle bus waits or WFI without executing instructions.
The base instruction rate remains shift-zero; the synchronization layer must
account for additional non-instruction ticks using a supported clock-bias hook.
Wall-clock socket wait time never contributes to simulated time.

Only the vCPU-owned coordinator may commit `C` and clock bias. Main-loop threads
can enqueue controls or external input, but cannot independently step RTL, change
the common clock, or inject an untimestamped interrupt.
Guest-visible timer dispatch must also be gated while an instruction slot is
in flight: tentative icount advancement is not permission to expose an event
before its RTL edge ACK. Host UI/watchdog processing is separate from guest
logical event dispatch.

## QEMU scheduling hooks

The source provides useful integration points, but requires a QEMU core patch:

1. In `accel/tcg/tcg-accel-ops-icount.c`, cap the normal instruction budget to
   **one** in `icount_prepare_for_run()`. Preserve a smaller/zero budget required
   by an already-due event. Apply this cap at every replenishment; changing only
   a single initial budget is insufficient.
2. In `accel/tcg/tcg-accel-ops-rr.c`, after `tcg_cpu_exec()` and
   `icount_process_data()`, establish the synchronization barrier before another
   instruction budget can be granted. Snapshot instruction accounting before the
   slot and compare afterward: exceptions or exits can execute zero instructions.
3. Use `accel/tcg/cputlb.c`'s existing I/O preparation and
   `accel/tcg/translate-all.c`'s `cpu_io_recompile()` path. It makes MMIO occur in
   an instruction-count-precise I/O block; that alone does not synchronize RTL.
4. Add a guarded, locked external-tick accounting API in
   `accel/tcg/icount-common.c`. It accounts the active instruction, changes only
   external wait/idle bias, and invalidates/recomputes outstanding timer budgets.
   Never modify private counter fields from a device callback without this API.
5. Intercept the no-sleep clock-warp path in `icount_start_warp_timer()`.
   With `sleep=off`, QEMU can normally advance its bias directly to a timer
   deadline. That must be replaced by acknowledged RTL ticks in this mode.
6. Audit alternative execution paths, including `EXCP_ATOMIC` handling,
   single-stepping, reset, and debugger accesses. Route each through the same
   coordinator or reject it. A cap in the normal TCG path is not sufficient proof
   that every path observes the barrier. Initially support RV32IMC without atomic
   memory instructions, one vCPU, and no snapshots/hotplug.

Coordinator pseudocode, not an existing QEMU API:

```text
at committed boundary C:
    publish IRQ levels captured at C
    if host halted:
        tick_and_commit(C + 1, IDLE)
        process due logical events; reevaluate IRQ wakeup
        repeat without executing a host instruction

    begin one-instruction slot at C
    run TCG with budget <= 1
        MMIO can synchronously consume this slot's base tick and extra wait ticks
    account actual instructions executed
    if one instruction executed and slot's base tick was not consumed by MMIO:
        tick_and_commit(C + 1, CPU_BASE)
    if no instruction executed:
        do not invent an instruction tick
    verify clocks and sequence numbers
    process due logical events and publish IRQ levels
    only now grant the next instruction slot
```

Instruction budgets are decremented around translation-block execution, not
necessarily after architectural retirement. At MMIO entry, QEMU can already
report the current instruction as counted. Track the slot's starting committed
cycle and whether its base tick has been consumed. Do not blindly synchronize
RTL to a raw `icount_get()` value before asserting the bus request: doing so can
move the request one edge late and charge its base cycle twice.

For an MMIO instruction occupying `L` hardware ticks, charge one instruction
and `L - 1` extra wait ticks, not `L` new instructions. Deliver pending exceptions
and interrupts at the defined instruction boundary after completion.

`quantum=1` is the strict baseline. Do not enable `N > 1` merely because it is
small: an RTL IRQ at an intermediate edge could change the next CPU instruction.
A larger quantum is allowed only with a proven event-free lookahead interval
and no intervening observable MMIO, or a validated rollback mechanism. Otherwise
it must be rejected. One-instruction TBs are an additional debugging option,
not a substitute for this budget/barrier policy.

## Serialized IPC protocol

Use a separate protocol/version from `cosim_soc_protocol.h`. Each frame has an
explicit byte length, little-endian encoding, protocol version, session epoch,
monotonic sequence number, message type, and absolute committed cycle. Avoid
sending native C structs with implicit padding.

Only one request is outstanding. Required messages:

- `HELLO`: negotiate version, one-vCPU policy, quantum=1, initial epoch,
  RTL period, reset length, bus width, and byte order. Reject mismatches.
- `RESET`: clock reset for the agreed number of ticks while the host is stopped;
  return the committed cycle and initial interrupt levels before CPU execution.
- `BUS_SETUP`: stage transaction ID, physical address, read/write, access size,
  byte enables, and write data at the current boundary. This message does not
  advance the clock and does not acknowledge architectural MMIO completion.
- `TICK`: authorize exactly one full RTL clock period. Include the starting
  cycle and timestamped external input changes scheduled for that period.
- `TICK_ACK`: return the same sequence, the ending cycle `start + 1`, bus grant
  and completion events with their edge numbers, read data/status when complete,
  current IRQ levels, and timestamped output events.
- `BARRIER`: verify epoch and cycle equality without advancing either side.
- `SHUTDOWN`: acknowledge the final committed cycle and stop both endpoints.

The RTL endpoint rejects duplicate/stale/future sequences and a starting cycle
that differs from its own. QEMU rejects any ACK for the wrong epoch, transaction,
sequence, or cycle. A channel timeout, malformed frame, bus timeout, or endpoint
failure halts the run; it must never produce a fabricated MMIO value or permit
the host to resume. Do not automatically retry an uncertain write: it may already
have changed hardware state.
An RTL-reported bus fault is a modeled guest transaction error; a transport or
synchronization failure is a fatal simulation error. Do not turn the latter into
an ordinary guest exception and let firmware continue with an unsynchronized peer.

Implement full-length send/receive loops, explicit limits, and partial-frame
assembly. A watchdog uses host time only to report a failure, never to advance
the guest clock. Unix sockets are suitable for the first correctness version.
Shared memory can later use the same messages with acquire/release publication,
sequence counters, and process-shared wait/wakeup primitives. Changing transport
must not change cycle/edge traces or grant either endpoint permission to run ahead.

## MMIO stalls and exact edge handling

Map the RTL-owned region as QEMU `MemoryRegionOps` callbacks, not directly mapped
RAM. Use `read_with_attrs` and `write_with_attrs` with explicit access validation
and `MemTxResult` status. Disable overlapping QEMU emulations of RTL-owned timers,
UART, GPIO, PWM, and SPI so they cannot produce duplicate state or interrupts.

The TCG MMIO helper calls the device callback synchronously. The callback returns
only after the bus response handshake. While it waits, that guest instruction
cannot complete and the single vCPU cannot execute the next instruction. A write
must also wait for its completion response; transport receipt or address grant
alone is insufficient.

```text
read/write callback(guest_address, size, value):
    require active one-instruction slot and exclusive coordinator ownership
    validate width, alignment, address decode, and transaction type
    BUS_SETUP(current C, transaction ID, address, byte enables, data)

    while transaction not complete:
        TICK(start=C, count=1)
        wait synchronously for matching TICK_ACK
        validate ACK and commit C := C + 1
        consume the instruction's base tick once; account later ticks as waits
        record IRQ levels and all events at their exact edge timestamps
        service permitted due events without executing another instruction
        fail if the configured maximum bus latency is exceeded

    latch successful read data or bus fault from the completion edge
    publish final pending IRQ state before the next instruction slot
    return MEMTX_OK/data or the architecture's transaction-fault result
```

Wall-clock blocking in `recv()` is not bus-cycle accounting. The loop must
explicitly tick RTL, commit elapsed bus-wait ticks into the common clock, and
keep QEMU-owned logical events consistent. Do not add an extra generic STEP when
this callback returns if it has already consumed the instruction's base tick.

TCG's MMIO dispatch holds the Big QEMU Lock in this source tree. A production
implementation should pin device lifetime and transaction state, release BQL
only around a transport wait when QEMU locking rules allow it, and reacquire it
before touching QEMU devices or IRQs. The independent RTL server must never depend
on a QEMU callback that cannot run while the vCPU is waiting. Maintain a strict
lock order and prohibit recursive guest MMIO from queued callbacks. Keeping BQL
held for a bounded initial prototype avoids guest concurrency but blocks monitor
progress during the wait; it is not a responsiveness solution.

### Native bus used by this repository

The current RTL is **not AXI or TileLink**. Its shared bus exposes
`host_req`, `host_gnt`, `host_we`, `host_addr`, `host_be`, `host_wdata`,
`host_rvalid`, `host_rdata`, and `host_err`. The crossbar expects the device
response one cycle after request acceptance. Do not claim arbitrary slave
backpressure support without changing the crossbar.

Provide an external master wrapper for that bus and either remove or hold the
replaced Ibex CPU in reset. The QEMU CPU is the sole executor of the demo firmware;
do not leave a second RTL CPU running the same firmware on the same peripheral
bus. If Ibex itself is intended as a separate accelerator, define it as a second
processor with explicit memory ownership and its own reset/clock contract.

For each native transaction:

1. At the low-clock boundary C, drive the request and stable address/control/data,
   then evaluate combinational logic.
2. Sample request/grant and response-valid/data/error immediately **before** the
   next rising edge. Drive the rising edge and evaluate RTL. Any sampled handshake
   is timestamped as consumed on that rising edge.
3. After a request is accepted, deassert it at the following low phase, so a write
   is not accepted repeatedly. Continue checking for its response on each edge.
4. A registered response becoming valid *after* edge `k` is available for sampling
   at edge `k+1`; do not mislabel it as a handshake consumed at edge `k`.
5. Complete the falling edge and evaluate before acknowledging a whole cycle.
   Respect Verilator's configured time precision and timed-event scheduling;
   settle any required internal events before consuming outputs or issuing ACK.

For a 32-bit little-endian register bus, align the bus word address and map
byte/halfword accesses into the correct byte enables and data lanes. Reject
unsupported unaligned/cross-word accesses rather than silently rounding them.
Return reads with the inverse lane extraction. All writes complete on the bus
response, even when `host_gnt` is combinationally asserted immediately.

### AXI and TileLink adapters, if added later

The same clock/IPC policy can wrap a different bus, but completion rules differ:

- AXI: an address handshake is not completion. For a write, track AW and W
  independently and wait for `BVALID && BREADY`; for a read, wait for the matching
  `RVALID && RREADY` response and `RLAST` where applicable. Sample before the
  consuming edge, hold payload stable under backpressure, and validate IDs/status.
- TileLink: accept the request on `A.valid && A.ready`, then wait for the matching
  source's `D.valid && D.ready` response. Validate opcode, denial/corruption, and
  multibeat completion. Initially forbid outstanding multiple transactions.

Do not reuse native-bus fixed-latency assumptions for these adapters. Acceptance
tests must include independent channel stalls and delayed responses.

## Interrupts, WFI, and external inputs

Every ACK carries RTL IRQ **levels**, not just unlatched pulses. Keep a timestamped
event trace; preserve pulse/pending semantics through the actual hardware's latch
rules rather than arbitrarily turning every pulse into a persistent interrupt.
Route timer and UART levels to the QEMU CPU interrupt inputs matching this board's
interrupt mapping. Apply the edge's IRQ state before allowing the next host
instruction. A stalled MMIO instruction does not take an interrupt halfway through
its synchronous callback; record intervening events and apply the defined host
instruction-boundary semantics on completion. This still does not model Ibex's
physical interrupt-entry pipeline latency.

For WFI, issue acknowledged idle ticks with no instruction execution, updating
clock bias until the actual RTL interrupt wakes the host. Otherwise a timer can
deadlock: the CPU waits for an interrupt while RTL waits for another instruction.
Generic sleep-off warp is disabled for this machine; only committed RTL ACKs may
advance idle time. Apply reset and guest pause through coordinator barriers.

Live keyboard input depends on host arrival timing and is not intrinsically
reproducible. Assign each input to an explicit future cycle at a barrier, record
that assignment, and replay the timestamped stream for deterministic tests.
Record RNG seeds and reset configuration. Initially exclude DMA, other bus masters,
and shared-memory writes outside the coordinator; supporting them requires a
common, ordered memory-transaction model as well.

## Validation and performance gate

Before calling this mode implemented, add tests that assert:

1. No instruction executes after a granted slot until the matching RTL ACK.
2. Every ACK advances exactly one RTL cycle; duplicates, skipped cycles, wrong
   epochs, and partial messages fail closed.
3. An MMIO read does not publish a result before response acceptance; a write
   changes hardware exactly once. Delayed bus transactions consume exactly one
   base instruction tick plus the measured wait ticks.
4. Simultaneous response/IRQ edges, faulting accesses, zero-instruction TCG exits,
   and replay after I/O recompilation do not double-count edges or instructions.
5. WFI wakes from an RTL timer with no fabricated instructions or unacknowledged
   QEMU clock warp. Guest cycle/time counters track waits while the instruction
   counter remains unchanged.
6. Identical firmware, inputs, seeds, and configuration produce the same ordered
   `(cycle, instruction ordinal, PC, bus event, IRQ levels)` trace across runs and
   socket/shared-memory implementations.
7. Full RTL reference runs are used to quantify real Ibex timing differences;
   deterministic synchronization alone must not be reported as CPU timing parity.

The current full-SoC test suite validates the separate CPU-in-RTL console bridge;
passing it does **not** validate this proposed host-CPU lock-step mode.

Benchmark instructions/s, RTL cycles/s, and time spent in IPC separately. One
socket round trip per instruction can be slower than a monolithic RTL simulation.
Shared memory or colocated execution can reduce transport costs; neither restores
CPU timing omitted by QEMU. Report measured speedups only, with the same firmware
and accuracy contract. Never increase the quantum or drop an ACK to meet a speed
target.

## Source audit map

The design was checked against these locally installed sources:

- `qemu/docs/devel/tcg-icount.rst`: instruction-count semantics and precise MMIO.
- `qemu/accel/tcg/tcg-accel-ops-icount.c`: budget setup and final accounting.
- `qemu/accel/tcg/tcg-accel-ops-rr.c`: execution, deadline, and atomic-step paths.
- `qemu/accel/tcg/icount-common.c`: instruction conversion, clock bias, idle warp.
- `qemu/accel/tcg/cputlb.c`: I/O preparation, BQL, and synchronous MMIO dispatch.
- `qemu/accel/tcg/translate-all.c`: recompilation at an I/O instruction.
- `qemu/target/riscv/csr.c`: cycle/instruction/time counter behavior to audit.
- `ibex-demo-system/vendor/lowrisc_ibex/shared/rtl/bus.sv`: native bus protocol.
- `ibex-demo-system/rtl/system/ibex_demo_system.sv`: CPU, bus, and IRQ wiring.
- `ibex-demo-system/rtl/system/gpio.sv`: registered response and write semantics.
