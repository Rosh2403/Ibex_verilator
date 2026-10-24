#!/usr/bin/env bash
# Build and run NxN integer matmul on full-SoC Verilator for N = 8..256.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/env.sh"
VSIM="$ROOT/ibex-demo-system/build/lowrisc_ibex_demo_system_0/sim-verilator/Vtop_verilator"
OUT_CSV="${1:-$ROOT/ibex-vp-cosim/sw/matmul/matmul_sweep.csv}"
NS="${MATMUL_NS:-8 16 32 64 128 256}"

if [[ ! -x "$VSIM" ]]; then
  echo "Missing $VSIM. Run ibex-vp-cosim/build_full_soc.sh." >&2
  exit 1
fi

timeout_for() {
  case "$1" in
    8) echo 60 ;;
    16) echo 90 ;;
    32) echo 180 ;;
    64) echo 600 ;;
    128) echo 1800 ;;
    256) echo 5400 ;;
    *) echo 600 ;;
  esac
}

mkdir -p "$(dirname "$OUT_CSV")"
echo "n,macs,mode,sum_hex,sum_dec,expected_hex,checksum_ok,mcycle,cycles_per_mac,sim_cycles,wall_s,status" >"$OUT_CSV"

python3 - "$ROOT" "$OUT_CSV" $NS <<'PY'
import os, re, subprocess, sys, time

root, csv_path, *ns = sys.argv[1:]
vsim = os.path.join(
    root,
    "ibex-demo-system/build/lowrisc_ibex_demo_system_0/sim-verilator/Vtop_verilator",
)
build = os.path.join(root, "ibex-vp-cosim/build_matmul.sh")
timeouts = {8: 60, 16: 90, 32: 180, 64: 600, 128: 1800, 256: 5400}

def expected_sum(n: int) -> int:
    s = 0
    for i in range(n):
        for j in range(n):
            acc = 0
            for k in range(n):
                acc += (i + k + 1) * (k * j + 1)
            s += acc
    return s

def parse_uart(text: str):
    n = re.search(r"matmul N=(\d+)", text)
    mode = re.search(r"mode (\w+)", text)
    sm = re.search(r"sum ([0-9A-Fa-f]+)", text)
    cy = re.search(r"mcycle ([0-9A-Fa-f]+)", text)
    return (
        int(n.group(1)) if n else None,
        mode.group(1) if mode else "",
        int(sm.group(1), 16) if sm else None,
        int(cy.group(1), 16) if cy else None,
    )

def sim_cycles(log: str):
    m = re.search(r"(?:Executed cycles|Cycles):\s*([0-9]+)", log)
    return m.group(1) if m else ""

for n_s in ns:
    n = int(n_s)
    macs = n ** 3
    exp = expected_sum(n)
    print(f"=== N={n}  MACs={macs} ===", flush=True)
    subprocess.check_call(["bash", build, str(n)])
    elf = os.path.join(root, f"ibex-demo-system/sw/c/build/demo/matmul/n{n}/matmul")
    logdir = os.path.join("/tmp", f"ibex-matmul-n{n}")
    os.makedirs(logdir, exist_ok=True)
    sim_log = os.path.join(logdir, "sim.log")
    uart_log = os.path.join(logdir, "uart0.log")
    tmo = timeouts.get(n, 600)
    t0 = time.time()
    status = "ok"
    try:
        with open(sim_log, "w") as out:
            proc = subprocess.run(
                ["timeout", f"--signal=KILL", str(tmo), vsim, f"--meminit=ram,{elf}"],
                cwd=logdir,
                stdout=out,
                stderr=subprocess.STDOUT,
            )
        if proc.returncode not in (0,):
            status = "timeout" if proc.returncode in (124, 137) else f"exit{proc.returncode}"
    except Exception as exc:
        status = f"error:{exc}"
    wall = time.time() - t0
    uart = ""
    if os.path.exists(uart_log):
        uart = open(uart_log).read()
        print(uart, flush=True)
    slog = open(sim_log).read() if os.path.exists(sim_log) else ""
    _n, mode, sm, cy = parse_uart(uart)
    ok = sm is not None and sm == exp
    cpm = f"{cy / macs:.3f}" if cy else ""
    row = [
        str(n),
        str(macs),
        mode,
        f"{sm:x}" if sm is not None else "",
        str(sm) if sm is not None else "",
        f"{exp:x}",
        "yes" if ok else "no",
        str(cy) if cy is not None else "",
        cpm,
        sim_cycles(slog),
        f"{wall:.1f}",
        status if uart else status,
    ]
    if not uart:
        row[-1] = status if status != "ok" else "no_uart"
    with open(csv_path, "a") as f:
        f.write(",".join(row) + "\n")
    print("csv:", ",".join(row), flush=True)

print("Wrote", csv_path)
PY
echo "Wrote $OUT_CSV"
