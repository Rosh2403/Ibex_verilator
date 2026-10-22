// SPDX-License-Identifier: Apache-2.0
// Full SoC cosim: QEMU steps cycle-accurate Verilator, UART over socket.

#ifndef COSIM_SOC_PROTOCOL_H
#define COSIM_SOC_PROTOCOL_H

#include <stdint.h>

#if defined(QEMU_PACKED)
#define COSIM_SOC_PACKED QEMU_PACKED
#else
#define COSIM_SOC_PACKED __attribute__((packed))
#endif

#define COSIM_SOC_REQ_MAGIC 0x49425853u /* 'IBXS' */
#define COSIM_SOC_RESP_MAGIC 0x49425854u /* 'IBXT' */

#define COSIM_SOC_DEFAULT_SOCKET "/tmp/ibexdemo-verilator.sock"

#define COSIM_SOC_CMD_STEP 1u
#define COSIM_SOC_CMD_SHUTDOWN 2u
#define COSIM_SOC_CMD_UART_RX 3u
#define COSIM_SOC_CMD_PING 4u
#define COSIM_SOC_VERSION 1u
#define COSIM_SOC_MAX_STEP 1000000u

#define COSIM_SOC_MAX_UART_CHUNK 512u

typedef struct COSIM_SOC_PACKED CosimSocReq {
    uint32_t magic;
    uint32_t cmd;
    uint64_t param;
} CosimSocReq;

typedef struct COSIM_SOC_PACKED CosimSocResp {
    uint32_t magic;
    uint32_t uart_len;
    uint8_t flags;
    uint8_t reserved[3];
} CosimSocResp;

/* flags */
#define COSIM_SOC_FLAG_STOPPED 0x01u
#define COSIM_SOC_FLAG_ERROR 0x02u

static inline uint32_t cosim_soc_le32(uint32_t value) {
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return __builtin_bswap32(value);
#else
    return value;
#endif
}

static inline uint64_t cosim_soc_le64(uint64_t value) {
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return __builtin_bswap64(value);
#else
    return value;
#endif
}

#endif
