# Matmul accelerator daemon and TCG plugin

The matmul accelerator is a host process, `matmul_daemon`, separate from QEMU.
QEMU runs the Ibex firmware with TCG and loads `matmul_plugin.so`, a TCG plugin
that turns a guest doorbell store into a request to the daemon. Demo commands
are in the [README](../README.md#matmul-accelerator-daemon-qemu-tcg-plugin).
For a guided code walkthrough, see [MATMUL_DEEP_DIVE.md](MATMUL_DEEP_DIVE.md).

| Piece | Source | Built as |
| --- | --- | --- |
| Daemon | `ibex-vp-cosim/matmul_daemon/matmul_daemon.c` | `ibex-vp-cosim/build/matmul_daemon` |
| Plugin | `ibex-vp-cosim/tcg_plugin/matmul_plugin.c` | `ibex-vp-cosim/build/matmul_plugin.so` |
| Firmware | `ibex-vp-cosim/sw/matmul_accel/main.c` | `ibex-vp-cosim/build/matmul_accel.elf` |
| Guest mailbox | `ibex-vp-cosim/tcg_plugin/matmul_accel.h` | shared by plugin and firmware |
| Socket protocol | `ibex-vp-cosim/include/matmul_daemon_protocol.h` | shared by plugin and daemon |

Build all three with `ibex-vp-cosim/build_matmul_plugin.sh`, which also enables
plugins in QEMU when the current build lacks them, or with
`make -C ibex-vp-cosim matmul` once QEMU has plugins.

## Request flow

1. The firmware writes `N`, the addresses of `A`, `B`, and `C`, and
   `STATUS = IDLE` into the mailbox at `0x0011C000`, then stores to `KICK`
   (`0x0011C014`).
2. The plugin's memory callback sees the store to `KICK`. It runs on the vCPU
   thread, so the guest does not execute another instruction until it returns.
3. The plugin reads `A` and `B` from guest RAM and sends one `GEMM` message.
4. The daemon computes `C = A * B` with 32-bit wrap-around arithmetic, the same
   result the guest gets from the equivalent C loop, and replies with `C`, the
   multiply-accumulate count, and its compute time.
5. The plugin writes `C` into guest RAM, then `ERRCODE` and `STATUS`.
6. The firmware reads `STATUS` (already `DONE` or `ERROR`) and checks `C`.

## Guest mailbox (`matmul_accel.h`)

| Offset | Name | Written by | Meaning |
| --- | --- | --- | --- |
| `+0x00` | `N` | guest | matrix size, 1..`MATMUL_ACCEL_MAX_N` (32) |
| `+0x04` | `A` | guest | address of row-major `int32` A |
| `+0x08` | `B` | guest | address of B |
| `+0x0C` | `C` | guest | address for the result |
| `+0x10` | `STATUS` | both | `0` idle, `1` done, `2` error |
| `+0x14` | `KICK` | guest | any store starts a request |
| `+0x18` | `ERRCODE` | plugin | `1` bad N, `2` guest memory, `3` daemon unreachable or timed out, `4` daemon rejected |

The mailbox is ordinary guest RAM below the stack, so firmware must end below
`0x0011C000`; the test firmware checks this at startup. Without the plugin,
`STATUS` never changes and the firmware reports `no response` after a bounded
poll instead of hanging.

## Socket protocol (`matmul_daemon_protocol.h`)

Unix stream socket, default `/tmp/ibex-matmul-accel.sock`, overridden by
`IBEX_MATMUL_SOCKET` or the plugin's `socket=` argument. Each message is a
16-byte header, all fields little-endian:

```text
magic u32 = 0x444D4D49 ("IMMD")   version u16 = 1   type u16   seq u32   len u32
```

| Type | Direction | Payload |
| --- | --- | --- |
| `HELLO` (1) | plugin → daemon | client pid, reserved |
| `HELLO_ACK` (2) | daemon → plugin | daemon pid, max N (64), GEMMs served |
| `GEMM` (3) | plugin → daemon | `n`, `flags` (0), `A[n*n]`, `B[n*n]` |
| `GEMM_DONE` (4) | daemon → plugin | `n`, reserved, MACs (u64), compute ns (u64), `C[n*n]` |
| `ERROR` (5) | daemon → plugin | error code, detail |

Replies echo the request's `seq`. A bad `n` or unknown type returns `ERROR` and
leaves the connection open. Bad magic, bad version, an oversized payload, or a
length that does not match `n` returns `ERROR` and closes the connection,
because the byte stream can no longer be trusted.

## Daemon behavior

- `--daemon` double-forks and detaches. The launching command returns only
  after the socket is listening, so callers can connect immediately; startup
  errors still print to the terminal.
- The pidfile and log default to `SOCKET.pid` and `SOCKET.log`.
- The socket is created owner-only (mode `0600`).
- A second daemon on a live socket is refused. A stale socket left by a crash is
  replaced. A non-socket file at the socket path is never removed.
- Each client gets its own thread, so several QEMU instances can share one daemon.
- SIGTERM, SIGINT, or SIGHUP removes the socket and pidfile, but only if they
  still belong to this process.

## Plugin behavior and failure handling

- Plugin arguments: `socket=PATH` and `timeout_ms=N` (default 5000), the deadline
  for sending a request and receiving its reply.
- At load time the plugin connects and exchanges `HELLO`. If the daemon is not
  reachable, QEMU exits before the guest starts.
- If a request fails because of the connection, the plugin reconnects once and
  resends. This is safe because a `GEMM` request has no side effects in the
  daemon. It covers a daemon restarted while QEMU is running.
- If the retry also fails, or the daemon stops responding, the guest sees
  `STATUS = ERROR` with `ERRCODE = 3` after at most two timeouts. QEMU does not
  hang. The socket is non-blocking because requests can exceed macOS's 8 KiB
  socket buffer, and macOS ignores `MSG_DONTWAIT` on Unix stream sockets.
- On exit the plugin prints counts of kicks, completed requests, and failures.

## Tests

```bash
python3 ibex-vp-cosim/scripts/test_matmul_daemon.py
```

The 19 tests cover:

- Daemon lifecycle: detach, pidfile, socket permissions, SIGTERM cleanup,
  refusing a second daemon, stale-socket recovery, and never deleting a
  non-socket file.
- Protocol: GEMM correctness for N = 1..64 with full-range `int32` values and
  wrap-around, fragmented requests, every error code, which errors keep or close
  the connection, and six concurrent clients.
- QEMU end to end: firmware PASS through the daemon, QEMU refusing to start
  without a daemon, firmware failing cleanly without the plugin, reconnecting
  after a daemon restart while QEMU is paused, a daemon that never answers
  (guest error, no hang), and the launcher starting, reusing, and stopping
  daemons.

## Limits

- This is a functional model. The daemon's compute time is reported, but guest
  time does not advance for it, and no accelerator timing is modeled.
- The guest addresses in the mailbox are passed to
  `qemu_plugin_read/write_memory_vaddr`. That is correct for this M-mode,
  no-MMU firmware. Firmware that turns on address translation would need the
  plugin to translate addresses first.
- The plugin registers a store callback on every guest instruction, which slows
  TCG compared with running without the plugin.
