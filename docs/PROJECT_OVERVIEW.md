# Daemon-based NPU on the Ibex virtual platform

The virtual platform is QEMU’s `ibexdemo` machine. It runs Ibex firmware with TCG. The NPU is `matmul_daemon`, a separate host process. `matmul_plugin.so`, loaded with `-plugin`, is the only link between them. QEMU itself is not modified.

Firmware writes the matrix size and the addresses of A, B, and C into a mailbox at `0x0011C000`, then stores to the doorbell at `0x0011C014`. The plugin runs on that store, before the next guest instruction. It reads A and B from guest memory, sends one GEMM request over a Unix socket (`/tmp/ibex-matmul-accel.sock`), and writes C, an error code, and status back into guest memory. The daemon computes `C = A × B` in 32-bit wraparound arithmetic and replies with the result, the MAC count, and its own compute time. One daemon thread serves each QEMU connection. A size outside 1–32 is rejected in the plugin and never sent.

The same plugin can be built to do the multiply inside QEMU (`--direct`). Mailbox handling and the GEMM kernel are the same. Only the place the math runs changes, so the two timings can be compared. These times are host wall-clock microseconds for one request. They are not Ibex cycles.

## Result on this machine

Both launches print `PASS`. Each size matches the firmware’s own checksum, and N = 33 is rejected:

| Kick | Checksum | Outcome |
| --- | --- | --- |
| 4×4 | `FFFE8FC9` | ok |
| 8×8 | `FFFFE360` | ok |
| 16×16 | `FFFE87A3` | ok |
| 32×32 | `001F11EC` | ok |
| N = 33 | — | rejected (allowed 1..32) |

The daemon log agrees with the plugin’s compute phase. On one run the 32×32 multiply was 32,768 MACs in 53,597 ns, which is the plugin’s 53.60 µs compute figure.

## Plugin inside QEMU vs daemon

Direct plugin, one complete run:

| Size | Total | Read A,B | Compute | Write C |
| --- | --- | --- | --- | --- |
| 4×4 | 10.7 µs | 8.08 | 0.43 | 2.18 |
| 8×8 | 5.4 µs | 2.89 | 0.68 | 1.79 |
| 16×16 | 9.0 µs | 5.06 | 3.00 | 0.90 |
| 32×32 | 27.5 µs | 4.81 | 21.63 | 1.02 |

Daemon, three separate runs. The middle column is the fastest of the three; the range is all three totals.

| Size | Fastest total | Read A,B | Socket | Compute | Write C | Total across 3 runs |
| --- | --- | --- | --- | --- | --- | --- |
| 4×4 | 99.0 µs | 8.44 | 86.55 | 0.40 | 3.58 | 99 – 294 µs |
| 8×8 | 68.8 µs | 2.30 | 62.79 | 0.72 | 2.97 | 69 – 217 µs |
| 16×16 | 56.2 µs | 9.36 | 41.19 | 3.46 | 2.24 | 56 – 355 µs |
| 32×32 | 98.3 µs | 12.00 | 57.72 | 25.85 | 2.74 | 98 – 303 µs |

On the fastest daemon run the socket is 87% of a 4×4 request and 59% of a 32×32 request. Compute itself stays close to the in-plugin numbers (32×32: 25.85 µs in the daemon, 21.63 µs in the plugin). The extra time is the round trip: the guest is stopped on the doorbell until the reply arrives, so nothing overlaps that wait. Single kicks on this laptop also move around a lot; the 4×4 socket phase alone ran from 87 µs to 266 µs. A second direct run, captured through 16×16, sat in the same band as the first (7.3, 3.4, and 8.6 µs).

The daemon path is the one this platform is built to use. The in-plugin path is faster per request because it skips the socket. The split is what makes the NPU a separate process the virtual platform can attach to, restart, and share.
