#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Smoke-test GPIO + timer through the peripheral cosim socket."""

import socket
import struct
import sys

COSIM_REQ_MAGIC = 0x49425843
COSIM_RESP_MAGIC = 0x49425852
COSIM_CMD_MMIO = 0
COSIM_CMD_IDLE = 1
GPIO_BASE = 0x80000000
TIMER_BASE = 0x80002000


def xfer(sock, cmd, addr=0, we=0, be=0, wdata=0):
    sock.sendall(
        struct.pack("<IIBBBBI", COSIM_REQ_MAGIC, addr, we, be, cmd, 0, wdata)
    )
    hdr = sock.recv(16)
    magic, rdata, irq, uart_len = struct.unpack("<IIII", hdr)
    assert magic == COSIM_RESP_MAGIC
    if uart_len:
        sock.recv(uart_len)
    return rdata, irq


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "/tmp/ibexdemo-cosim.sock"
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(path)
    xfer(s, COSIM_CMD_MMIO, GPIO_BASE, 1, 0xF, 0xDEADBEEF)
    rdata, _ = xfer(s, COSIM_CMD_MMIO, GPIO_BASE, 0, 0xF)
    print(f"GPIO OUT readback: 0x{rdata:08x}")
    before, _ = xfer(s, COSIM_CMD_MMIO, TIMER_BASE, 0, 0xF)
    xfer(s, COSIM_CMD_IDLE, wdata=1000)
    after, irq = xfer(s, COSIM_CMD_MMIO, TIMER_BASE, 0, 0xF)
    print(f"timer mtime {before} -> {after} irq=0x{irq:x}")
    assert rdata == 0x0000BEEF
    assert after > before
    s.close()


if __name__ == "__main__":
    main()
