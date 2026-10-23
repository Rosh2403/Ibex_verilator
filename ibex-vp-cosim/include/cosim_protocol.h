// SPDX-License-Identifier: Apache-2.0
// MMIO cosim bridge protocol (QEMU <-> Verilator), host-endian.

#ifndef COSIM_PROTOCOL_H
#define COSIM_PROTOCOL_H

#include <stdint.h>

#define COSIM_REQ_MAGIC 0x49425843u /* 'IBXC' */
#define COSIM_RESP_MAGIC 0x49425852u /* 'IBXR' */

#define COSIM_GPIO_BASE 0x80000000u
#define COSIM_GPIO_SIZE 0x00001000u
#define COSIM_UART_BASE 0x80001000u
#define COSIM_UART_SIZE 0x00001000u
#define COSIM_TIMER_BASE 0x80002000u
#define COSIM_TIMER_SIZE 0x00001000u
#define COSIM_PERIPH_BASE 0x80000000u
#define COSIM_PERIPH_SIZE 0x00003000u

#define COSIM_PWM_BASE 0x80003000u
#define COSIM_PWM_SIZE 0x00001000u

#define COSIM_CMD_MMIO 0u
#define COSIM_CMD_IDLE 1u
#define COSIM_CMD_UART_RX 2u
#define COSIM_IRQ_TIMER 0x1u
#define COSIM_IRQ_UART 0x2u
#define COSIM_PERIPH_MAX_UART 512u
#define COSIM_PERIPH_MAX_IDLE 5000000u
#define COSIM_UART_CLOCK_HZ 50000000u
#define COSIM_UART_BAUD 115200u

struct CosimMmioReq {
    uint32_t magic;
    uint32_t addr;
    uint8_t we;
    uint8_t be;
    uint8_t cmd;
    uint8_t reserved;
    uint32_t wdata;
};

struct CosimMmioResp {
    uint32_t magic;
    uint32_t rdata;
};

struct CosimPeriphResp {
    uint32_t magic;
    uint32_t rdata;
    uint32_t irq;
    uint32_t uart_len;
};

#endif
