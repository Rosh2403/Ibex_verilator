# Cosim extensions: PWM and IRQ

## PWM (MMIO `0x80003000`)

1. Instantiate `pwm_wrapper` beside `gpio` in `rtl/cosim_gpio_top.sv`.
2. Demux `host_*` by address in C++ or add separate socket regions.
3. Replace QEMU `ibexdemo` PWM `TYPE_UNIMPLEMENTED_DEVICE` with a second
   `ibexdemo-cosim-bridge` instance (`region-base=0x80003000`).

## IRQ (UART in RTL later)

Extend the protocol with an optional `CosimIrqMsg` after MMIO responses, or add
a second socket for wire updates (Remote-Port style). Pulse `qemu_irq` from QEMU
when the Verilator side reports `uart_irq`.

## icount

For lock-step style debugging:

```bash
qemu-system-riscv32 -M ibexdemo-cosim -icount shift=7,align=off,sleep=on ...
```

This does not cycle-accurately couple QEMU and Verilator; it only slows the CPU.
