#!/usr/bin/env python3
import contextlib
import json
from pathlib import Path
import re
import socket
import struct
import subprocess
import tempfile
import threading
import time
import unittest


ROOT = Path(__file__).resolve().parents[2]
SIM = ROOT / "ibex-demo-system/build/lowrisc_ibex_demo_system_0/sim-verilator/Vtop_verilator"
QEMU = ROOT / "qemu/build/qemu-system-riscv32"
ELF = ROOT / "ibex-demo-system/sw/c/build/demo/hello_world/demo"
REQUEST_MAGIC = 0x49425853
RESPONSE_MAGIC = 0x49425854


def wait_for(predicate, timeout=20):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.02)
    raise AssertionError("Timed out waiting for simulation condition")


def receive_exact(connection, length):
    result = bytearray()
    while len(result) < length:
        chunk = connection.recv(length - len(result))
        if not chunk:
            raise AssertionError("Unexpected socket EOF")
        result.extend(chunk)
    return bytes(result)


def request(connection, command, parameter=0, fragmented=False, magic=REQUEST_MAGIC):
    message = struct.pack("<IIQ", magic, command, parameter)
    if fragmented:
        connection.sendall(message[:3])
        time.sleep(0.01)
        connection.sendall(message[3:])
    else:
        connection.sendall(message)
    response_magic, length, flags = struct.unpack("<IIB3x", receive_exact(connection, 12))
    if response_magic != RESPONSE_MAGIC or length > 512:
        raise AssertionError("Invalid full-SoC response")
    return flags, receive_exact(connection, length)


def stop_process(process):
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=3)


@contextlib.contextmanager
def server_session(*extra_arguments):
    with tempfile.TemporaryDirectory(prefix="ibexsoc-", dir="/tmp") as directory:
        work = Path(directory)
        endpoint = work / "soc.sock"
        with (work / "server.log").open("w") as log:
            process = subprocess.Popen(
                [str(SIM), f"--meminit=ram,{ELF}", f"--qemu-cosim-socket={endpoint}", *extra_arguments],
                cwd=work, stdout=log, stderr=subprocess.STDOUT,
            )
            try:
                wait_for(lambda: endpoint.exists() or process.poll() is not None)
                if process.poll() is not None:
                    raise AssertionError((work / "server.log").read_text())
                yield work, endpoint, process
            finally:
                stop_process(process)


class Qmp:
    def __init__(self, path):
        wait_for(path.exists)
        self.connection = socket.socket(socket.AF_UNIX)
        self.connection.settimeout(10)
        self.connection.connect(str(path))
        self.stream = self.connection.makefile("rwb", buffering=0)
        json.loads(self.stream.readline())
        self.command("qmp_capabilities")

    def command(self, name):
        self.stream.write(json.dumps({"execute": name}).encode() + b"\n")
        while True:
            response = json.loads(self.stream.readline())
            if "return" in response:
                return response["return"]
            if "error" in response:
                raise AssertionError(response)

    def close(self):
        self.stream.close()
        self.connection.close()


class FullSocTests(unittest.TestCase):
    def test_fragmented_requests_and_shutdown(self):
        with server_session() as (work, endpoint, process):
            with socket.socket(socket.AF_UNIX) as connection:
                connection.settimeout(5)
                connection.connect(str(endpoint))
                self.assertEqual(request(connection, 4, 1, fragmented=True), (0, b""))
                self.assertEqual(request(connection, 1, 32), (0, b""))
                self.assertEqual(request(connection, 2), (1, b""))
            self.assertEqual(process.wait(timeout=3), 0)
            self.assertFalse(endpoint.exists())

    def test_cycle_limit_reports_stopped(self):
        with server_session("--term-after-cycles=32") as (work, endpoint, process):
            with socket.socket(socket.AF_UNIX) as connection:
                connection.settimeout(5)
                connection.connect(str(endpoint))
                self.assertEqual(request(connection, 1, 100)[0], 1)
            self.assertEqual(process.wait(timeout=3), 0)
            self.assertIn("Executed cycles:  32", (work / "server.log").read_text())

    def test_invalid_step_is_rejected(self):
        with server_session() as (work, endpoint, process):
            with socket.socket(socket.AF_UNIX) as connection:
                connection.settimeout(5)
                connection.connect(str(endpoint))
                self.assertEqual(request(connection, 1, 0)[0], 3)
            self.assertEqual(process.wait(timeout=3), 1)

    def test_disconnect_stops_server(self):
        with server_session() as (work, endpoint, process):
            with socket.socket(socket.AF_UNIX) as connection:
                connection.settimeout(5)
                connection.connect(str(endpoint))
                request(connection, 4, 1)
            self.assertEqual(process.wait(timeout=3), 0)
            self.assertFalse(endpoint.exists())

    def test_qemu_has_no_cpu_and_uart_echo_uses_rtl(self):
        with server_session() as (work, endpoint, server):
            console = work / "console.log"
            qmp_path = work / "qmp.sock"
            with console.open("w") as output:
                client = subprocess.Popen(
                    [str(QEMU), "-M", f"ibexdemo-soc-cosim,socket={endpoint}",
                     "-display", "none", "-serial", "stdio", "-monitor", "none",
                     "-S", "-qmp", f"unix:{qmp_path},server=on,wait=off"],
                    stdin=subprocess.PIPE, stdout=output, stderr=subprocess.STDOUT,
                )
                qmp = None
                try:
                    qmp = Qmp(qmp_path)
                    self.assertEqual(qmp.command("query-cpus-fast"), [])
                    time.sleep(0.1)
                    self.assertNotIn(b"Hello World!", console.read_bytes())
                    qmp.command("cont")
                    wait_for(lambda: b"Hello World! 00000001" in console.read_bytes())
                    client.stdin.write(b"Z")
                    client.stdin.flush()
                    wait_for(lambda: b"Z\r\n" in console.read_bytes())
                    self.assertIn(b"Hello World!", (work / "uart0.log").read_bytes())
                    qmp.command("quit")
                    self.assertEqual(client.wait(timeout=5), 0)
                    self.assertEqual(server.wait(timeout=5), 0,
                                     (work / "server.log").read_text())
                finally:
                    if qmp:
                        qmp.close()
                    stop_process(client)
                    client.stdin.close()

    def test_qemu_rejects_oversized_response(self):
        with tempfile.TemporaryDirectory(prefix="ibexsoc-", dir="/tmp") as directory:
            endpoint = Path(directory) / "fake.sock"
            with socket.socket(socket.AF_UNIX) as listener:
                listener.bind(str(endpoint))
                listener.listen(1)
                listener.settimeout(5)
                def fake_server():
                    with listener.accept()[0] as connection:
                        receive_exact(connection, 16)
                        connection.sendall(struct.pack("<IIB3x", RESPONSE_MAGIC, 513, 0))
                        time.sleep(0.1)
                worker = threading.Thread(target=fake_server)
                worker.start()
                try:
                    result = subprocess.run(
                        [str(QEMU), "-M", f"ibexdemo-soc-cosim,socket={endpoint}",
                         "-display", "none", "-serial", "null", "-monitor", "none"],
                        capture_output=True, timeout=10,
                    )
                    self.assertEqual(result.returncode, 1)
                    self.assertIn(b"invalid response", result.stderr)
                finally:
                    worker.join(timeout=6)

    def test_launcher_exit_cleans_socket(self):
        with tempfile.TemporaryDirectory(prefix="ibexsoc-", dir="/tmp") as directory:
            work = Path(directory)
            console = work / "launcher.log"
            with console.open("w") as output:
                launcher = subprocess.Popen(
                    ["bash", str(ROOT / "ibex-vp-cosim/run_full_soc.sh")],
                    cwd=ROOT, stdin=subprocess.PIPE, stdout=output, stderr=subprocess.STDOUT,
                )
                try:
                    wait_for(lambda: b"Hello World!" in console.read_bytes())
                    launcher.stdin.write(b"\x01x")
                    launcher.stdin.flush()
                    self.assertEqual(launcher.wait(timeout=8), 0)
                    log_directory = re.search(r"Logs: (.+)", console.read_text()).group(1)
                    server_log = (Path(log_directory) / "verilator.log").read_text()
                    endpoint = re.search(r"listening on (.+)", server_log).group(1)
                    self.assertFalse(Path(endpoint).exists())
                    self.assertIn("Simulation statistics", server_log)
                finally:
                    stop_process(launcher)
                    launcher.stdin.close()


if __name__ == "__main__":
    unittest.main(verbosity=2)
