// SPDX-License-Identifier: Apache-2.0
// MMIO cosim bridge protocol (QEMU <-> Verilator), host-endian.

#ifndef COSIM_PROTOCOL_H
#define COSIM_PROTOCOL_H

#include <stdint.h>

#define COSIM_REQ_MAGIC 0x49425843u /* 'IBXC' */
#define COSIM_RESP_MAGIC 0x49425852u /* 'IBXR' */

#define COSIM_GPIO_BASE 0x80000000u
#define COSIM_GPIO_SIZE 0x00001000u

#define COSIM_PWM_BASE 0x80003000u
#define COSIM_PWM_SIZE 0x00001000u

struct CosimMmioReq {
    uint32_t magic;
    uint32_t addr;
    uint8_t we;
    uint8_t be;
    uint8_t reserved[2];
    uint32_t wdata;
};

struct CosimMmioResp {
    uint32_t magic;
    uint32_t rdata;
};

#endif
