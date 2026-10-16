#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Smoke-test the GPIO cosim socket without QEMU."""

import socket
import struct
import sys

COSIM_REQ_MAGIC = 0x49425843
COSIM_RESP_MAGIC = 0x49425852
GPIO_BASE = 0x80000000


def mmio(sock, addr, we, be, wdata=0):
    req = struct.pack("<IIBB2xI", COSIM_REQ_MAGIC, addr, we, be, wdata)
    sock.sendall(req)
    magic, rdata = struct.unpack("<II", sock.recv(8))
    assert magic == COSIM_RESP_MAGIC
    return rdata


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "/tmp/ibexdemo-cosim.sock"
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(path)
    mmio(s, GPIO_BASE, 1, 0xF, 0xDEADBEEF)
    rdata = mmio(s, GPIO_BASE, 0, 0xF)
    print(f"GPIO OUT readback: 0x{rdata:08x}")
    s.close()


if __name__ == "__main__":
    main()
