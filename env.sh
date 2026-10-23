if [ -n "${BASH_SOURCE:-}" ]; then
  export ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
elif [ -n "${ZSH_VERSION:-}" ]; then
  export ROOT="$(cd "$(dirname "${(%):-%x}")" && pwd)"
else
  export ROOT="$PWD"
fi
export PATH="$ROOT/verilator/bin:$ROOT/local/env/bin:$ROOT/local/bin:$ROOT/toolchain/bin:$PATH"
export LD_LIBRARY_PATH="$ROOT/local/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export PKG_CONFIG_PATH="$ROOT/local/usr/lib/x86_64-linux-gnu/pkgconfig:$ROOT/local/env/lib/pkgconfig:$ROOT/local/env/share/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export ELF="$ROOT/ibex-demo-system/sw/c/build/demo/hello_world/demo"
export QEMU="$ROOT/qemu/build/qemu-system-riscv32"
