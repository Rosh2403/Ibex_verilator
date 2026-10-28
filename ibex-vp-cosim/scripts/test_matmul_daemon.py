#!/usr/bin/env python3
"""Tests for the matmul accelerator daemon and the QEMU TCG plugin client.

Run from the repository root after `source ./env.sh`:
    python3 ibex-vp-cosim/scripts/test_matmul_daemon.py
"""
import os
from pathlib import Path
import random
import re
import shutil
import socket
import stat
import struct
import subprocess
import tempfile
import threading
import time
import unittest


ROOT = Path(__file__).resolve().parents[2]
COSIM = ROOT / "ibex-vp-cosim"
BUILD = COSIM / "build"
DAEMON = BUILD / "matmul_daemon"
PLUGIN = BUILD / "matmul_plugin.so"
PLUGIN_DIRECT = BUILD / "matmul_plugin_direct.so"
FIRMWARE = BUILD / "matmul_accel.elf"
QEMU = ROOT / "qemu/build/qemu-system-riscv32"

MAGIC = 0x444D4D49
VERSION = 1
HELLO, HELLO_ACK, GEMM, GEMM_DONE, ERROR = 1, 2, 3, 4, 5
ERR_BAD_MAGIC, ERR_BAD_VERSION, ERR_BAD_LENGTH, ERR_BAD_TYPE, ERR_BAD_N = 1, 2, 3, 4, 5
MAX_N = 64


def wait_for(predicate, timeout=10.0, what="condition"):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.02)
    raise AssertionError(f"timed out waiting for {what}")


def recv_exact(conn, length):
    data = bytearray()
    while len(data) < length:
        chunk = conn.recv(length - len(data))
        if not chunk:
            raise EOFError("daemon closed the connection")
        data.extend(chunk)
    return bytes(data)


def header(msg_type, seq, length, magic=MAGIC, version=VERSION):
    return struct.pack("<IHHII", magic, version, msg_type, seq, length)


def read_msg(conn):
    magic, version, msg_type, seq, length = struct.unpack("<IHHII", recv_exact(conn, 16))
    assert magic == MAGIC and version == VERSION, (hex(magic), version)
    return msg_type, seq, recv_exact(conn, length)


def gemm_payload(n, a, b):
    return struct.pack(f"<II{n * n}i{n * n}i", n, 0, *a, *b)


def reference_gemm(n, a, b):
    out = []
    for i in range(n):
        for j in range(n):
            acc = sum(a[i * n + k] * b[k * n + j] for k in range(n)) & 0xFFFFFFFF
            out.append(acc - (1 << 32) if acc & 0x80000000 else acc)
    return out


def connect(path):
    conn = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    conn.settimeout(5)
    conn.connect(str(path))
    return conn


def gemm(conn, n, a, b, seq=7):
    conn.sendall(header(GEMM, seq, 8 + 8 * n * n) + gemm_payload(n, a, b))
    msg_type, rseq, payload = read_msg(conn)
    assert rseq == seq
    return msg_type, payload


def decode_done(n, payload):
    rn, _, macs, _ns = struct.unpack("<IIQQ", payload[:24])
    assert rn == n and macs == n ** 3
    return list(struct.unpack(f"<{n * n}i", payload[24:]))


def random_matrix(rng, n, lo=-(1 << 31), hi=(1 << 31) - 1):
    return [rng.randint(lo, hi) for _ in range(n * n)]


class DaemonFixture:
    """A detached daemon on a private socket."""

    def __init__(self, directory):
        self.socket = Path(directory) / "mm.sock"
        self.pidfile = Path(directory) / "mm.pid"
        self.log = Path(directory) / "mm.log"

    def start(self, check=True):
        proc = subprocess.run(
            [DAEMON, "--socket", self.socket, "--daemon", "--pidfile", self.pidfile,
             "--log", self.log],
            capture_output=True, text=True, timeout=10)
        if check and proc.returncode != 0:
            raise AssertionError(f"daemon failed to start: {proc.stderr}")
        return proc

    @property
    def pid(self):
        return int(self.pidfile.read_text())

    def stop(self):
        try:
            pid = self.pid
        except (FileNotFoundError, ValueError):
            return
        try:
            os.kill(pid, 15)
            wait_for(lambda: not pid_alive(pid), what="daemon exit")
        except ProcessLookupError:
            pass


def pid_alive(pid):
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    # A zombie child still answers kill(0); the daemon is never our child.
    return True


class MatmulDaemonTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        subprocess.run(["bash", COSIM / "build_matmul_plugin.sh"], check=True,
                       stdout=subprocess.DEVNULL)
        subprocess.run(["make", "-s", "-C", COSIM, "matmul-bench"], check=True)

    def setUp(self):
        # Short /tmp path: Unix socket paths are limited to ~104 bytes on macOS.
        self.dir = tempfile.mkdtemp(prefix="mmd", dir="/tmp")
        self.daemon = DaemonFixture(self.dir)

    def tearDown(self):
        self.daemon.stop()
        shutil.rmtree(self.dir, ignore_errors=True)

    # ---------------------------------------------------------- lifecycle

    def test_detach_writes_pidfile_and_private_socket(self):
        start = time.monotonic()
        self.daemon.start()
        self.assertLess(time.monotonic() - start, 5)
        self.assertTrue(pid_alive(self.daemon.pid))
        mode = self.daemon.socket.stat().st_mode
        self.assertTrue(stat.S_ISSOCK(mode))
        self.assertEqual(mode & 0o077, 0, "socket must be owner-only")
        ping = subprocess.run([DAEMON, "--socket", self.daemon.socket, "--ping"],
                              capture_output=True, text=True, timeout=5)
        self.assertEqual(ping.returncode, 0, ping.stderr)
        self.assertIn(f"pid {self.daemon.pid}", ping.stdout)
        self.assertIn("0 GEMMs served", ping.stdout)

    def test_sigterm_removes_socket_and_pidfile(self):
        self.daemon.start()
        pid = self.daemon.pid
        os.kill(pid, 15)
        wait_for(lambda: not pid_alive(pid), what="daemon exit")
        self.assertFalse(self.daemon.socket.exists())
        self.assertFalse(self.daemon.pidfile.exists())
        self.assertIn("stopping; served 0 GEMMs", self.daemon.log.read_text())

    def test_second_daemon_is_refused(self):
        self.daemon.start()
        second = self.daemon.start(check=False)
        self.assertNotEqual(second.returncode, 0)
        self.assertIn("already listening", second.stderr)
        with connect(self.daemon.socket) as conn:
            msg_type, _ = gemm(conn, 2, [1, 2, 3, 4], [5, 6, 7, 8])
            self.assertEqual(msg_type, GEMM_DONE)

    def test_stale_socket_after_crash_is_replaced(self):
        self.daemon.start()
        os.kill(self.daemon.pid, 9)
        wait_for(lambda: not pid_alive(self.daemon.pid), what="daemon kill")
        self.assertTrue(self.daemon.socket.exists())
        self.daemon.start()
        with connect(self.daemon.socket) as conn:
            self.assertEqual(gemm(conn, 1, [3], [4])[0], GEMM_DONE)

    def test_refuses_to_replace_non_socket_file(self):
        self.daemon.socket.write_text("not a socket")
        proc = self.daemon.start(check=False)
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("is not a socket", proc.stderr)
        self.assertEqual(self.daemon.socket.read_text(), "not a socket")

    # ----------------------------------------------------------- protocol

    def test_hello_reports_served_count(self):
        self.daemon.start()
        with connect(self.daemon.socket) as conn:
            gemm(conn, 2, [1, 0, 0, 1], [9, 8, 7, 6])
            conn.sendall(header(HELLO, 3, 8) + struct.pack("<II", os.getpid(), 0))
            msg_type, seq, payload = read_msg(conn)
        self.assertEqual((msg_type, seq), (HELLO_ACK, 3))
        pid, max_n, served = struct.unpack("<IIQ", payload)
        self.assertEqual((pid, max_n, served), (self.daemon.pid, MAX_N, 1))

    def test_gemm_matches_reference_with_wraparound(self):
        self.daemon.start()
        rng = random.Random(1234)
        with connect(self.daemon.socket) as conn:
            for n in (1, 2, 3, 7, 16, 33, MAX_N):
                a, b = random_matrix(rng, n), random_matrix(rng, n)
                msg_type, payload = gemm(conn, n, a, b, seq=n)
                self.assertEqual(msg_type, GEMM_DONE)
                self.assertEqual(decode_done(n, payload), reference_gemm(n, a, b), n)

    def test_fragmented_request(self):
        self.daemon.start()
        n, a, b = 3, list(range(9)), list(range(9, 18))
        message = header(GEMM, 11, 8 + 8 * n * n) + gemm_payload(n, a, b)
        with connect(self.daemon.socket) as conn:
            for i in range(0, len(message), 5):
                conn.sendall(message[i:i + 5])
                time.sleep(0.002)
            msg_type, seq, payload = read_msg(conn)
        self.assertEqual((msg_type, seq), (GEMM_DONE, 11))
        self.assertEqual(decode_done(n, payload), reference_gemm(n, a, b))

    def test_bad_n_is_rejected_and_connection_survives(self):
        self.daemon.start()
        with connect(self.daemon.socket) as conn:
            for bad_n in (0, MAX_N + 1):
                conn.sendall(header(GEMM, 5, 8) + struct.pack("<II", bad_n, 0))
                msg_type, seq, payload = read_msg(conn)
                self.assertEqual((msg_type, seq), (ERROR, 5))
                self.assertEqual(struct.unpack("<II", payload), (ERR_BAD_N, bad_n))
            self.assertEqual(gemm(conn, 1, [6], [7])[0], GEMM_DONE)

    def test_unknown_type_is_rejected_and_connection_survives(self):
        self.daemon.start()
        with connect(self.daemon.socket) as conn:
            conn.sendall(header(99, 4, 3) + b"abc")
            msg_type, _, payload = read_msg(conn)
            self.assertEqual(msg_type, ERROR)
            self.assertEqual(struct.unpack("<II", payload), (ERR_BAD_TYPE, 99))
            self.assertEqual(gemm(conn, 1, [6], [7])[0], GEMM_DONE)

    def test_framing_errors_close_connection(self):
        self.daemon.start()
        cases = [
            (header(GEMM, 1, 0, magic=0x12345678), ERR_BAD_MAGIC),
            (header(GEMM, 1, 0, version=9), ERR_BAD_VERSION),
            (header(GEMM, 1, 1 << 24), ERR_BAD_LENGTH),
            (header(GEMM, 1, 12) + struct.pack("<III", 2, 0, 0), ERR_BAD_LENGTH),
        ]
        for message, code in cases:
            with self.subTest(code=code), connect(self.daemon.socket) as conn:
                conn.sendall(message)
                msg_type, _, payload = read_msg(conn)
                self.assertEqual(msg_type, ERROR)
                self.assertEqual(struct.unpack("<II", payload)[0], code)
                self.assertEqual(conn.recv(1), b"", "connection should be closed")
        with connect(self.daemon.socket) as conn:
            self.assertEqual(gemm(conn, 1, [2], [3])[0], GEMM_DONE)

    def test_concurrent_clients(self):
        self.daemon.start()
        errors = []

        def worker(seed):
            rng = random.Random(seed)
            try:
                with connect(self.daemon.socket) as conn:
                    for i in range(15):
                        n = rng.randint(1, 12)
                        a, b = random_matrix(rng, n, -50, 50), random_matrix(rng, n, -50, 50)
                        msg_type, payload = gemm(conn, n, a, b, seq=i + 1)
                        if msg_type != GEMM_DONE or decode_done(n, payload) != reference_gemm(n, a, b):
                            errors.append(f"client {seed} request {i}")
            except Exception as exc:  # noqa: BLE001 - report from thread
                errors.append(f"client {seed}: {exc!r}")

        threads = [threading.Thread(target=worker, args=(seed,)) for seed in range(6)]
        for t in threads:
            t.start()
        for t in threads:
            t.join(30)
        self.assertEqual(errors, [])
        self.assertIn("[total 90]", self.daemon.log.read_text())

    # ------------------------------------------------------- QEMU end-to-end

    def run_qemu(self, *extra, plugin=True, timeout=30):
        cmd = [QEMU, "-M", "ibexdemo", "-nographic", "-kernel", FIRMWARE]
        if plugin:
            cmd += ["-plugin", f"{PLUGIN},socket={self.daemon.socket}"]
        return subprocess.run(cmd + list(extra), stdin=subprocess.DEVNULL,
                              capture_output=True, text=True, timeout=timeout)

    def test_qemu_firmware_passes_through_daemon(self):
        self.daemon.start()
        proc = self.run_qemu()
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn("PASS", proc.stdout)
        self.assertEqual(proc.stdout.count("ok  checksum"), 4)
        self.assertIn("rejected", proc.stdout)
        self.assertIn(f"connected to daemon pid {self.daemon.pid}", proc.stderr)
        self.assertIn("exit: 5 kicks, 4 done, 1 failed", proc.stderr)
        log = self.daemon.log.read_text()
        for n in (4, 8, 16, 32):
            self.assertIn(f"GEMM {n}x{n} ", log)
        self.assertIn("disconnected", log)

    def test_qemu_refuses_to_start_without_daemon(self):
        start = time.monotonic()
        proc = self.run_qemu()
        self.assertNotEqual(proc.returncode, 0)
        self.assertLess(time.monotonic() - start, 10)
        self.assertIn("cannot connect to matmul daemon", proc.stderr)
        self.assertNotIn("guest:", proc.stdout)

    def test_firmware_without_plugin_fails_instead_of_hanging(self):
        proc = self.run_qemu(plugin=False)
        self.assertEqual(proc.returncode, 1, proc.stdout)
        self.assertIn("no response (is the plugin loaded?)", proc.stdout)
        self.assertIn("FAIL", proc.stdout)

    def test_plugin_reconnects_after_daemon_restart(self):
        self.daemon.start()
        first_pid = self.daemon.pid
        monitor = Path(self.dir) / "mon.sock"
        out, err = Path(self.dir) / "qemu.out", Path(self.dir) / "qemu.err"
        with out.open("w") as stdout, err.open("w") as stderr:
            qemu = subprocess.Popen(
                [QEMU, "-M", "ibexdemo", "-display", "none", "-serial", "stdio",
                 "-monitor", f"unix:{monitor},server=on,wait=off", "-S",
                 "-kernel", FIRMWARE, "-plugin", f"{PLUGIN},socket={self.daemon.socket}"],
                stdin=subprocess.DEVNULL, stdout=stdout, stderr=stderr)
        try:
            wait_for(lambda: monitor.exists() and "loaded;" in err.read_text(),
                     what="paused QEMU with plugin loaded")
            self.daemon.stop()
            self.daemon.start()
            self.assertNotEqual(self.daemon.pid, first_pid)
            with connect(monitor) as mon:
                mon.sendall(b"cont\n")
                self.assertEqual(qemu.wait(timeout=30), 0, out.read_text() + err.read_text())
        finally:
            if qemu.poll() is None:
                qemu.kill()
                qemu.wait()
        self.assertIn("PASS", out.read_text())
        log = err.read_text()
        self.assertIn("send to daemon failed", log)  # old daemon gone...
        self.assertIn(f"connected to daemon pid {self.daemon.pid}", log)  # ...retried
        self.assertIn("GEMM 32x32", self.daemon.log.read_text())

    def test_hung_daemon_times_out_into_guest_error(self):
        """A daemon that accepts but never answers GEMM must not hang QEMU."""
        listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        listener.bind(str(self.daemon.socket))
        listener.listen(4)
        held = []

        def serve():
            while True:
                try:
                    conn, _ = listener.accept()
                except OSError:
                    return
                held.append(conn)
                _, seq, _ = (lambda h: (h, struct.unpack("<IHHII", h)[3], recv_exact(conn, 8)))(
                    recv_exact(conn, 16))
                conn.sendall(header(HELLO_ACK, seq, 16) + struct.pack("<IIQ", 1, MAX_N, 0))

        threading.Thread(target=serve, daemon=True).start()
        try:
            cmd = [QEMU, "-M", "ibexdemo", "-nographic", "-kernel", FIRMWARE, "-plugin",
                   f"{PLUGIN},socket={self.daemon.socket},timeout_ms=200"]
            proc = subprocess.run(cmd, stdin=subprocess.DEVNULL, capture_output=True,
                                  text=True, timeout=30)
        finally:
            listener.close()
            for conn in held:
                conn.close()
        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
        self.assertIn("accelerator error 3", proc.stdout)  # MATMUL_ACCEL_ERR_DAEMON_IO
        self.assertIn("no reply from daemon within 200 ms", proc.stderr)
        self.assertIn("FAIL", proc.stdout)

    def test_summary_log_reports_per_size_timing(self):
        self.daemon.start()
        proc = subprocess.run(
            [QEMU, "-M", "ibexdemo", "-nographic", "-kernel", FIRMWARE, "-plugin",
             f"{PLUGIN},socket={self.daemon.socket},log=summary"],
            stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=30)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertNotIn("kick #1:", proc.stderr)  # no per-request lines
        for n in (4, 8, 16, 32):
            self.assertIn(f"timing [daemon] n={n}: 1 requests", proc.stderr)

    def test_direct_baseline_plugin_matches_daemon_results(self):
        """The benchmark baseline must compute the same results without a daemon."""
        self.daemon.start()
        daemon_run = self.run_qemu()
        direct = subprocess.run(
            [QEMU, "-M", "ibexdemo", "-nographic", "-kernel", FIRMWARE,
             "-plugin", str(PLUGIN_DIRECT)],
            stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=30)
        self.assertEqual(direct.returncode, 0, direct.stdout + direct.stderr)
        self.assertIn("by plugin (direct)", direct.stderr)
        checksums = lambda out: re.findall(r"checksum ([0-9A-F]{8})", out)
        self.assertEqual(checksums(direct.stdout), checksums(daemon_run.stdout))
        self.assertEqual(len(checksums(direct.stdout)), 4)

    def test_gdb_debugs_guest_with_daemon_plugin(self):
        """QEMU's GDB server works with the plugin loaded and the daemon attached."""
        gdb = next((shutil.which(g) for g in
                    ("riscv32-unknown-elf-gdb", "riscv-none-elf-gdb", "gdb-multiarch")
                    if shutil.which(g)), None)
        if not gdb:
            self.skipTest("no RISC-V GDB found")
        self.daemon.start()
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        qemu = subprocess.Popen(
            [QEMU, "-M", "ibexdemo", "-display", "none", "-serial", "null", "-kernel",
             FIRMWARE, "-plugin", f"{PLUGIN},socket={self.daemon.socket}",
             "-gdb", f"tcp::{port}", "-S"],
            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
            text=True)
        try:
            script = "\n".join([
                "set pagination off", "set confirm off", f"target remote :{port}",
                "break kick", "continue",
                "print/x *(unsigned int *)0x0011C010",   # STATUS before the kick
                "finish",                                # run the kick under GDB
                "print/x *(unsigned int *)0x0011C010",   # STATUS after the kick
                "delete", "break finish", "continue", "print pass", "continue", ""])
            cmds = Path(self.dir) / "gdb.cmds"
            cmds.write_text(script)
            out = subprocess.run([gdb, "-q", "-batch", "-x", cmds, FIRMWARE],
                                 capture_output=True, text=True, timeout=60).stdout
            self.assertEqual(qemu.wait(timeout=30), 0, out)
        finally:
            if qemu.poll() is None:
                qemu.kill()
                qemu.wait()
        self.assertIn("Breakpoint 1, kick (n=n@entry=4)", out)
        self.assertRegex(out, r"\$\d+ = 0x0\n")              # IDLE before
        self.assertRegex(out, r"Value returned is \$\d+ = 1")  # kick() saw DONE
        self.assertRegex(out, r"\$\d+ = 0x1\n")              # DONE after
        self.assertIn("finish (pass=pass@entry=1)", out)
        self.assertIn("exited normally", out)
        self.assertIn("GEMM 32x32", self.daemon.log.read_text())

    def test_launcher_starts_and_stops_its_daemon(self):
        env = dict(os.environ, IBEX_MATMUL_SOCKET=str(self.daemon.socket))
        proc = subprocess.run(["bash", COSIM / "run_matmul_plugin.sh"], env=env,
                              stdin=subprocess.DEVNULL, capture_output=True, text=True,
                              timeout=120)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn("Result: PASS", proc.stdout)
        self.assertIn("GEMM 32x32", proc.stdout)  # daemon log excerpt
        self.assertFalse(self.daemon.socket.exists(), "launcher should stop its daemon")
        self.assertFalse(Path(f"{self.daemon.socket}.pid").exists())

    def test_launcher_reuses_running_daemon(self):
        env = dict(os.environ, IBEX_MATMUL_SOCKET=str(self.daemon.socket))
        ctl = COSIM / "matmul_daemon.sh"
        subprocess.run(["bash", ctl, "start"], env=env, check=True, capture_output=True)
        try:
            for _ in range(2):
                proc = subprocess.run(["bash", COSIM / "run_matmul_plugin.sh"], env=env,
                                      stdin=subprocess.DEVNULL, capture_output=True,
                                      text=True, timeout=120)
                self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
                self.assertIn("Using running matmul daemon", proc.stdout)
            status = subprocess.run(["bash", ctl, "status"], env=env, capture_output=True,
                                    text=True, check=True)
            self.assertIn("8 GEMMs served", status.stdout)
        finally:
            subprocess.run(["bash", ctl, "stop"], env=env, capture_output=True)
        self.assertFalse(self.daemon.socket.exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
