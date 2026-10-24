# Full-SoC QEMU + Verilator integration

```bash
source ./env.sh
./ibex-vp-cosim/build_periph_cosim.sh
./ibex-vp-cosim/run_cosim.sh
```

Default simulator is QEMU TCG + Verilator GPIO/UART/timer. Full-SoC RTL is
`run_full_soc.sh`. Ibex RTL matmul: `run_matmul.sh` / `run_matmul_sweep.sh`.
Host 4x4 via TCG plugin: `run_matmul_plugin.sh`. See [README.md](../README.md).

## Execution and UART

`full_soc/soc_cosim.h` registers a FuseSoC simulation-controller extension. Before
execution it binds a Unix socket and accepts one QEMU client. The normal upstream
reset, ELF loading, clocks, termination limits, tracing, and finalization still run.
At each batch boundary the extension returns UART bytes and waits for the next
stepping request; it never advances the RTL freely between requests.

The UART DPI hooks intercept decoded RTL TX bytes and inject incoming bytes into
the DPI UART serializer. Consequently keyboard input traverses the RTL UART RX
logic and reaches the Ibex firmware's interrupt handler. UART output is also
recorded in `uart0.log`. The existing pseudo-terminal is used in standalone mode.

`full_soc/ibexdemo_soc_cosim.c` defines QEMU's **`ibexdemo-soc-cosim`** machine.
It has no QEMU CPU or RAM. A real-time timer issues batches of 100,000 cycles,
and nonblocking socket handlers assemble responses without blocking the QEMU
monitor while the RTL executes. QEMU pause prevents new requests; at most one
already-issued batch can finish after pausing. A response deadline of ten seconds
detects stalled or incompatible peers.

The machine uses serial backend zero (`-serial ...`). `-kernel` and `-cpu` are
rejected because execution belongs to Verilator. QEMU CPU debugging, snapshots,
and device reset do not model RTL state; restart the launcher to reset the SoC.
This integration is a simulation bridge, not a QEMU/RTL lockstep checker.

## Socket protocol

`include/cosim_soc_protocol.h` defines packed little-endian messages. One request
is outstanding at a time. Requests are 16 bytes: magic (32 bits), command
(32 bits), parameter (64 bits). Responses have a 12-byte header: magic (32 bits),
UART byte count (32 bits), flags (8 bits), and three reserved zero bytes, followed
by at most 512 UART bytes.

- `PING` (4): parameter must equal protocol version 1. It checks compatibility.
- `STEP` (1): run 1–1,000,000 complete RTL clock cycles. The initial reset cycles
  count toward the requested budget. The response contains available UART TX bytes.
- `UART_RX` (3): parameter is a single byte, 0–255, queued for serial delivery.
- `SHUTDOWN` (2): parameter must be zero; acknowledge and stop the simulator.

`STOPPED` (flag 1) reports shutdown or natural RTL/cycle-limit termination;
`ERROR` (flag 2) reports a rejected request. Invalid magic, commands, step sizes,
or version numbers cause `ERROR | STOPPED` and failure. UART queues are bounded
to 4,096 bytes. A disconnected client ends its server session and removes the
socket. TCP and remote hosts are not supported.

`IBEXDEMO_COSIM_SOCKET` overrides the launcher's per-run socket path. Direct
connections default to `/tmp/ibexdemo-verilator.sock` on the QEMU side and require
`Vtop_verilator --qemu-cosim-socket=/tmp/ibexdemo-verilator.sock` on the RTL side.
The launcher passes the selected path to both programs. Logs live in a unique
directory under `ibex-vp-cosim/build/`.

## Source management

The external `qemu/` and `ibex-demo-system/` trees are ignored by Git. Keep bridge
changes in `full_soc/` and `include/`, and upstream integration edits in the
tracked `patches/` files. The builder applies patches idempotently and copies
the maintained files into the external trees before rebuilding. Conflicting
upstream edits cause patch application to stop with an error.

## TCG CPU + RTL peripherals

`ibexdemo-cosim` runs QEMU TCG as the CPU and Verilator as GPIO, UART, and timer:

```bash
./ibex-vp-cosim/build_periph_cosim.sh
./ibex-vp-cosim/run_periph_cosim.sh
```

`Vcosim_periph_server` uses `include/cosim_protocol.h` with `IDLE` / `UART_RX`
commands and IRQ plus UART bytes in each response. The GPIO-only
`Vcosim_gpio_server` remains for `scripts/test_gpio_client.py`.

Extensions to the original GPIO-only bridge: [docs/extend_pwm_irq.md](docs/extend_pwm_irq.md).
