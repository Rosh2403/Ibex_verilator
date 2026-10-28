#!/usr/bin/env python3
"""Benchmark the daemon NPU against the direct (in-QEMU) plugin baseline.

Run from the repository root after `source ./env.sh`:
    python3 ibex-vp-cosim/scripts/bench_npu.py [--runs 5]

Two measurements, each the median of --runs QEMU runs:

1. Guest workload with no NPU requests (CPU + UART output), with no plugin,
   the direct plugin, and the daemon plugin. Shows what loading the plugin
   costs ordinary guest execution and I/O.
2. Per-request NPU latency (2000 requests per size) for the direct plugin and
   the daemon, as measured by the plugin around each request.

Both plugin variants are built from the same source with the same
instrumentation and the same GEMM kernel; only where the math runs differs.
"""
import argparse
from pathlib import Path
import re
import shutil
import statistics
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / "ibex-vp-cosim/build"
QEMU = ROOT / "qemu/build/qemu-system-riscv32"
DAEMON = BUILD / "matmul_daemon"
PLUGIN = BUILD / "matmul_plugin.so"
PLUGIN_DIRECT = BUILD / "matmul_plugin_direct.so"
BENCH_IO = BUILD / "bench_io.elf"
BENCH_NPU = BUILD / "bench_npu.elf"
TIMING = re.compile(r"timing \[[^\]]+\] n=(\d+): \d+ requests, avg ([0-9.]+) us")


def run_qemu(elf, plugin_arg=None):
    cmd = [str(QEMU), "-M", "ibexdemo", "-nographic", "-kernel", str(elf)]
    if plugin_arg:
        cmd += ["-plugin", plugin_arg]
    start = time.perf_counter()
    proc = subprocess.run(cmd, stdin=subprocess.DEVNULL, capture_output=True, text=True,
                          timeout=300)
    elapsed = time.perf_counter() - start
    if proc.returncode != 0 or "PASS" not in proc.stdout:
        raise SystemExit(f"benchmark run failed ({' '.join(cmd)}):\n{proc.stdout}{proc.stderr}")
    return elapsed, {int(n): float(us) for n, us in TIMING.findall(proc.stderr)}


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--runs", type=int, default=5)
    args = parser.parse_args()

    subprocess.run(["make", "-s", "-C", str(ROOT / "ibex-vp-cosim"), "matmul-bench"],
                   check=True)
    work = Path(tempfile.mkdtemp(prefix="npub", dir="/tmp"))
    sock = work / "npu.sock"
    subprocess.run([DAEMON, "--socket", sock, "--daemon", "--pidfile", work / "pid",
                    "--log", work / "log"], check=True)
    daemon_arg = f"{PLUGIN},socket={sock},log=summary"
    direct_arg = f"{PLUGIN_DIRECT},log=summary"
    try:
        print(f"Median of {args.runs} runs each.\n")

        print("Guest CPU + UART workload, no NPU requests (wall time per QEMU run):\n")
        print("| Configuration | Time (s) | vs no plugin |")
        print("| --- | --- | --- |")
        base = None
        for label, arg in [("No plugin", None), ("Direct plugin (math in QEMU)", direct_arg),
                           ("Daemon plugin (NPU daemon)", daemon_arg)]:
            t = statistics.median(run_qemu(BENCH_IO, arg)[0] for _ in range(args.runs))
            base = base or t
            print(f"| {label} | {t:.3f} | {t / base:.2f}x |")

        print("\nNPU request latency (average per request, 2000 requests per size):\n")
        results = {}
        for label, arg in [("direct", direct_arg), ("daemon", daemon_arg)]:
            runs = [run_qemu(BENCH_NPU, arg)[1] for _ in range(args.runs)]
            results[label] = {n: statistics.median(r[n] for r in runs) for n in runs[0]}
        print("| Matrix | Direct plugin (us) | Daemon (us) | Daemon overhead (us) |")
        print("| --- | --- | --- | --- |")
        for n in sorted(results["direct"]):
            d, m = results["direct"][n], results["daemon"][n]
            print(f"| {n}x{n} | {d:.1f} | {m:.1f} | +{m - d:.1f} |")
    finally:
        pid = (work / "pid").read_text().strip() if (work / "pid").exists() else None
        if pid:
            subprocess.run(["kill", pid])
            time.sleep(0.3)
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
