# Installed local development environment

Prepared on October 21, 2026 on macOS / Apple Silicon. Everything is installed
inside this workspace; no Homebrew installation or shell-profile edits were needed.

From the repository root, use `source ./env.sh` in Bash or Zsh. It adds
`local/env/bin` and `toolchain/bin` to PATH and sets `ROOT`, `QEMU`, `ELF`, and
`PKG_CONFIG_PATH`. The prebuilt native tools occupy about 3.2 GiB across `local/`
and `toolchain/`; source trees, build artifacts, and downloads use additional space.

## Sources and tools

- Ibex Demo System: `d37beb753e19482eb054aefac73a27289bf72e47` in `ibex-demo-system/`.
- lowRISC QEMU: branch `ot-10.2.0`, base `d7655281a649abb339f5267f0b9e05329b3cee6d`,
  with the repository's GPIO bridge patch applied in `qemu/`.
- Verilator 5.052, Python 3.11, CMake, Ninja, Meson, pkg-config, Rust, GLib,
  Pixman, zlib, LZ4, and compiler/build utilities in `local/env/` via micromamba.
- The upstream Python requirements, including lowRISC FuseSoC and Edalize,
  installed in the same Python environment.
- xPack RISC-V GCC 15.2.0-1 for Darwin ARM64 in `toolchain/`. Compatibility
  symlinks expose its `riscv-none-elf-*` tools as `riscv32-unknown-elf-*`.
- libelf 0.8.13 built from source into `local/env/` for full-SoC ELF loading.

Downloads are under `deps/`. The xPack archive's SHA-256 was checked against its
published checksum; the libelf archive matched the Homebrew formula's checksum.
`deps/conda-explicit.txt` records the native package URLs and
`deps/python-freeze.txt` records Python packages. These and the dependency trees
are ignored by Git, so another clone needs its own installation.

## Build commands and compatibility adjustments

Source `env.sh` first. The firmware needs explicit CSR and instruction-fence
extensions with this modern compiler; CMake 4 also needs the policy override:

```bash
cmake -S "$ROOT/ibex-demo-system/sw/c" -B "$ROOT/ibex-demo-system/sw/c/build" \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  '-DCMAKE_C_FLAGS=-march=rv32imc_zicsr_zifencei -mabi=ilp32 -mcmodel=medany -Wall -fvisibility=hidden -ffreestanding' \
  '-DCMAKE_ASM_FLAGS=-march=rv32imc_zicsr_zifencei -mabi=ilp32'
cmake --build "$ROOT/ibex-demo-system/sw/c/build" --target demo
make -C "$ROOT/ibex-vp-cosim"
```

QEMU was configured with Apple's Clang. This QEMU branch requires a Rust compiler
at Meson configuration time even with Rust devices disabled:

```bash
cd "$ROOT/qemu/build"
../configure --target-list=riscv32-softmmu --without-default-features \
  --enable-tcg --enable-tools --disable-rust \
  --cc=/usr/bin/clang --cxx=/usr/bin/clang++
ninja -j 8 qemu-system-riscv32
```

The full-SoC builder applies `-Wno-UNOPTFLAT` through the upstream core patch and
adds these native library paths to the generated `config.mk`:

```make
VERILATOR_OPTIONS += -CFLAGS "-I$(IBEX_LOCAL_ENV)/include -I$(IBEX_LOCAL_ENV)/include/libelf"
VERILATOR_OPTIONS += -LDFLAGS "-L$(IBEX_LOCAL_ENV)/lib -Wl,-rpath,$(IBEX_LOCAL_ENV)/lib -llz4"
```

Rebuild the prepared full-SoC build with:

```bash
export IBEX_LOCAL_ENV="$ROOT/local/env"
make -C "$ROOT/ibex-demo-system/build/lowrisc_ibex_demo_system_0/sim-verilator" -j 8
```

If you rerun FuseSoC setup, it overwrites `config.mk`. Reapply the extra lines,
then use `make -B` once to regenerate the Verilator makefile with those options.
Use `ibex-vp-cosim/build_full_soc.sh` to regenerate the complete setup instead of
manually maintaining generated files. Upstream integration edits are preserved
as patches in this repository, and bridge files are maintained in
`ibex-vp-cosim/full_soc/`.

## Demo commands and verification

The two-terminal QEMU + GPIO RTL demo is documented in `README.md`.
The socket smoke test returned `GPIO OUT readback: 0x0000beef`.
Both `ibexdemo` and `ibexdemo-cosim` printed `Hello World! 00000001` during
verification. The observed input values were `00000000` for QEMU alone and
`00000010` for GPIO co-simulation, so do not require identical GPIO input output
as a pass criterion. The supplied bridge's GPIO readback behavior needs further
investigation before treating guest input values as validated pin samples.

To run the standalone full-SoC RTL model:

```bash
cd "$ROOT/ibex-demo-system/build/lowrisc_ibex_demo_system_0/sim-verilator"
./Vtop_verilator --meminit=ram,"$ELF"
```

Read `uart0.log` in that directory from a second terminal. Stop with Ctrl+C.
The native full-SoC run also passed: `uart0.log` contained
`Hello World! 00000001   Input Value: 00000000`.
Build and execution logs are saved under `deps/`.

## Full-SoC socket integration

The full-SoC integration is now implemented and built. Run:

```bash
source ./env.sh
./ibex-vp-cosim/run_cosim.sh
```

The new QEMU machine `ibexdemo-soc-cosim` instantiates no CPU. Verilator executes
all SoC RTL, and the console receives decoded RTL UART bytes. Firmware output and
keyboard echo through the RTL UART both pass. Seven integration tests also verify
pause behavior, cycle limits, malformed traffic, disconnects, and launcher cleanup:

```bash
python3 ibex-vp-cosim/scripts/test_full_soc.py
```

The external sources are changed by the tracked patches and copied bridge files;
`ibex-vp-cosim/build_full_soc.sh` can reapply and rebuild them. The integration test
log is `deps/full-soc-tests.log` and the build log is
`deps/full-soc-integration-build.log`.
