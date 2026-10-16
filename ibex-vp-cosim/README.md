# Ibex Demo System: QEMU + Verilator cosim

Hybrid virtual platform: **QEMU** runs the Ibex CPU and most peripherals on the
`-M ibexdemo-cosim` machine; **Verilator** simulates the **GPIO RTL** from
`ibex-demo-system` over a Unix socket MMIO bridge.

For full build steps and how to tell each simulation mode is working, see the
[workspace README](../README.md).

## Prerequisites

Build from the parent tree (`Ibex_verilator/`):

- Verilator (local install under `local/` or on `PATH`)
- `ibex-demo-system` bare-metal ELF (`sw/c/build/demo/hello_world/demo`)
- lowRISC QEMU with cosim patches (`qemu/build/qemu-system-riscv32`)

## Build GPIO cosim server

```bash
make -C ibex-vp-cosim
```

## Run

```bash
./ibex-vp-cosim/run_cosim.sh
```

Optional: pass a different ELF or extra QEMU flags:

```bash
./ibex-vp-cosim/run_cosim.sh /path/to/demo -icount shift=6
```

## Socket protocol

See `include/cosim_protocol.h`. Smoke test without QEMU:

```bash
./build/Vcosim_gpio_server --socket /tmp/ibexdemo-cosim.sock &
python3 scripts/test_gpio_client.py
```

## Extensions (PWM / timing)

- **PWM at `0x80003000`**: add `pwm_wrapper` to `cosim_gpio_top` and extend the
  bridge; QEMU PWM remains `unimp` until then.
- **Determinism**: use QEMU `-icount shift=N` to slow the vCPU while debugging
  cosim transactions.
