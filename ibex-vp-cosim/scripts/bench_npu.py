#!/usr/bin/env python3
"""Benchmark the daemon NPU against the direct (in-QEMU) plugin baseline.

Run from the repository root after `source ./env.sh`:
    python3 ibex-vp-cosim/scripts/bench_npu.py [--runs 5] [--npu-only]

Two measurements, each the median of --runs QEMU runs:

1. Guest workload with no NPU requests (CPU + UART output), with no plugin,
   the direct plugin, and the daemon plugin. Shows what loading the plugin
   costs ordinary guest execution and I/O. Skipped with --npu-only.
2. Time per NPU request (2000 requests per size) for the direct plugin and
   the daemon, split into the phases the plugin measures: reading A and B from
   guest memory, the socket round trip (daemon only), the GEMM, and writing C.

All times are host wall-clock time. Both plugin variants are built from the
same source with the same instrumentation and the same GEMM kernel; only where
the math runs differs.
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
TIMING = re.compile(r"timing \[[^\]]+\] n=(\d+): \d+ requests, avg ([0-9.]+) us = "
                    r"read A,B ([0-9.]+)(?: \+ socket ([0-9.]+))? \+ compute ([0-9.]+) \+ write C ([0-9.]+)")
PHASES = ("total", "read", "socket", "compute", "write")


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
    timings = {}
    for n, *vals in TIMING.findall(proc.stderr):
        timings[int(n)] = dict(zip(PHASES, (float(v) if v else 0.0 for v in vals)))
    return elapsed, timings


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--npu-only", action="store_true",
                        help="skip the guest I/O workload (takes a few seconds instead of a minute)")
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

        if not args.npu_only:
            print("Guest CPU + UART workload, no NPU requests (wall time per QEMU run):\n")
            print("| Configuration | Time (s) | vs no plugin |")
            print("| --- | --- | --- |")
            configs = [("No plugin", None), ("Direct plugin (math in QEMU)", direct_arg),
                       ("Daemon plugin (NPU daemon)", daemon_arg)]
            # Interleave the configurations so drift (thermal, background load)
            # affects all of them equally.
            times = {label: [] for label, _ in configs}
            for _ in range(args.runs):
                for label, arg in configs:
                    times[label].append(run_qemu(BENCH_IO, arg)[0])
            base = statistics.median(times["No plugin"])
            for label, _ in configs:
                t = statistics.median(times[label])
                print(f"| {label} | {t:.3f} | {t / base:.2f}x |")
            print()

        print("Time per NPU request in µs (host wall clock; average of 2000 requests per size):\n")
        runs = {"direct": [], "daemon": []}
        for _ in range(args.runs):  # interleaved, as above
            for label, arg in [("direct", direct_arg), ("daemon", daemon_arg)]:
                runs[label].append(run_qemu(BENCH_NPU, arg)[1])
        results = {label: {n: {ph: statistics.median(r[n][ph] for r in rs) for ph in PHASES}
                           for n in rs[0]}
                   for label, rs in runs.items()}
        print("| Matrix | Mode | Total | Read A,B | Socket | Compute | Write C |")
        print("| --- | --- | --- | --- | --- | --- | --- |")
        for n in sorted(results["direct"]):
            for label, name in [("direct", "Direct (in QEMU)"), ("daemon", "Daemon")]:
                r = results[label][n]
                sock = "–" if label == "direct" else f"{r['socket']:.2f}"
                print(f"| {n}×{n} | {name} | {r['total']:.2f} | {r['read']:.2f} | {sock} | "
                      f"{r['compute']:.2f} | {r['write']:.2f} |")
        extra = []
        for n in sorted(results["direct"]):
            diff = results["daemon"][n]["total"] - results["direct"][n]["total"]
            share = 100 * results["daemon"][n]["socket"] / diff if diff > 0 else 0
            extra.append(f"{n}×{n} +{diff:.1f} (socket {share:.0f}%)")
        print(f"\nDaemon minus direct, per request (µs): {', '.join(extra)}.")
        print("Read, compute, and write are the same work in both modes, so the socket round")
        print("trip is most of the difference.")
    finally:
        pid = (work / "pid").read_text().strip() if (work / "pid").exists() else None
        if pid:
            subprocess.run(["kill", pid])
            time.sleep(0.3)
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
