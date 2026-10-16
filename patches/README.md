# QEMU patches

Apply on [lowRISC QEMU](https://github.com/lowRISC/qemu) branch `ot-10.2.0`:

```bash
git clone --branch ot-10.2.0 https://github.com/lowRISC/qemu.git
cd qemu
git am /path/to/Ibex_verilator/patches/*.patch
```

Or use the `qemu/` tree in this workspace on branch `ibexdemo-verilator-cosim` if you
cloned it locally during development.
