# External source integration patches

The supported build entry point, from the repository root, is:

```bash
source ./env.sh
./ibex-vp-cosim/build_full_soc.sh
```

It applies the following patches with `git apply` when they are not already
present, then copies the maintained bridge sources and builds both simulators:

- `0001-Add-ibexdemo-cosim-machine-and-Verilator-MMIO-bridge.patch`: original GPIO
  bridge and 128 KiB RAM adjustment for lowRISC QEMU branch `ot-10.2.0`.
- `0002-full-soc-qemu-build.patch`: adds the separate full-SoC QEMU machine source
  to the build. Its implementation is `ibex-vp-cosim/full_soc/ibexdemo_soc_cosim.c`.
- `0003-full-soc-verilator-hooks.patch`: registers the simulation-controller
  extension, adds UART DPI callbacks and headers, and configures Verilator.
- `0004-tcg-periph-cosim-qemu.patch`: QEMU `ibexdemo-cosim` TCG CPU with
  Verilator GPIO, UART, and timer. Bridge sources are in
  `ibex-vp-cosim/periph/` and are copied by `build_periph_cosim.sh`.

Tested QEMU base: `d7655281a649abb339f5267f0b9e05329b3cee6d`.
Tested Ibex Demo System base: `d37beb753e19482eb054aefac73a27289bf72e47`.

Patch 0001 is also a mailbox patch and can be applied on its own using `git am`.
Patches 0002 and 0003 are working-tree diffs; do not use `git am patches/*.patch`.
The full-SoC source files live in this repository and are copied by the builder,
so applying the patch files alone is insufficient to build the integration.
