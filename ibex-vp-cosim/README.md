# Ibex cosim launcher (`run_cosim.sh`)

Cycle-accurate **full SoC** simulation: `Vtop_verilator` runs all RTL; QEMU
`-M ibexdemo-cosim` only steps the model and shows UART on `-nographic`.

**User-facing guide:** [workspace README](../README.md) (quick start, build, all three modes).

## Run

From the repo root (after `Vtop_verilator` and QEMU are built):

```bash
export ROOT="$HOME/Ibex_verilator"
export PATH="$ROOT/verilator/bin:$ROOT/toolchain/bin:$ROOT/local/bin:$HOME/.local/bin:$PATH"
export LD_LIBRARY_PATH="$ROOT/local/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

"$ROOT/ibex-vp-cosim/run_cosim.sh"
```

Optional ELF and QEMU arguments:

```bash
"$ROOT/ibex-vp-cosim/run_cosim.sh" "$ROOT/ibex-demo-system/sw/c/build/demo/hello_world/demo"
```

## Protocol

`include/cosim_soc_protocol.h` — socket `/tmp/ibexdemo-verilator.sock` by default
(`IBEXDEMO_COSIM_SOCKET` to override).

## Legacy GPIO-only server

`make -C ibex-vp-cosim` builds `Vcosim_gpio_server` for MMIO experiments
(`include/cosim_protocol.h`, `scripts/test_gpio_client.py`). Not used by `run_cosim.sh`.

Extensions: [docs/extend_pwm_irq.md](docs/extend_pwm_irq.md).
