// SPDX-License-Identifier: Apache-2.0
//
// Socket protocol between the QEMU TCG matmul plugin (client) and the matmul
// accelerator daemon (server). One Unix stream socket per QEMU process.
//
// Every message is a 16-byte header followed by `len` payload bytes. All
// fields are little-endian. Requests are synchronous: the client sends one
// request and waits for the response carrying the same `seq`.
//
//   HELLO      client -> daemon   MmdHello
//   HELLO_ACK  daemon -> client   MmdHelloAck
//   GEMM       client -> daemon   MmdGemmReq, then A[n*n], then B[n*n]
//   GEMM_DONE  daemon -> client   MmdGemmDone, then C[n*n]
//   ERROR      daemon -> client   MmdError
//
// Matrices are row-major int32. C = A * B uses 32-bit wrap-around arithmetic,
// matching the result of the same loop compiled for RV32.

#ifndef MATMUL_DAEMON_PROTOCOL_H
#define MATMUL_DAEMON_PROTOCOL_H

#include <stdint.h>

#if defined(QEMU_PACKED)
#define MMD_PACKED QEMU_PACKED
#else
#define MMD_PACKED __attribute__((packed))
#endif

#define MMD_MAGIC 0x444D4D49u /* bytes "IMMD" on the wire */
#define MMD_VERSION 1u
#define MMD_DEFAULT_SOCKET "/tmp/ibex-matmul-accel.sock"
#define MMD_SOCKET_ENV "IBEX_MATMUL_SOCKET"

#define MMD_MAX_N 64u
#define MMD_MAX_PAYLOAD \
    (sizeof(MmdGemmReq) + 2u * MMD_MAX_N * MMD_MAX_N * sizeof(int32_t))

enum {
    MMD_MSG_HELLO = 1,
    MMD_MSG_HELLO_ACK = 2,
    MMD_MSG_GEMM = 3,
    MMD_MSG_GEMM_DONE = 4,
    MMD_MSG_ERROR = 5,
};

/* MmdError.code. The daemon closes the connection after framing errors. */
enum {
    MMD_ERR_BAD_MAGIC = 1,   /* closes connection */
    MMD_ERR_BAD_VERSION = 2, /* closes connection */
    MMD_ERR_BAD_LENGTH = 3,  /* closes connection */
    MMD_ERR_BAD_TYPE = 4,
    MMD_ERR_BAD_N = 5,
};

typedef struct MMD_PACKED MmdHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t type;
    uint32_t seq;
    uint32_t len; /* payload bytes after this header */
} MmdHeader;

typedef struct MMD_PACKED MmdHello {
    uint32_t client_pid;
    uint32_t reserved;
} MmdHello;

typedef struct MMD_PACKED MmdHelloAck {
    uint32_t daemon_pid;
    uint32_t max_n;
    uint64_t requests_served; /* GEMMs completed since the daemon started */
} MmdHelloAck;

typedef struct MMD_PACKED MmdGemmReq {
    uint32_t n;
    uint32_t flags; /* must be zero */
} MmdGemmReq;

typedef struct MMD_PACKED MmdGemmDone {
    uint32_t n;
    uint32_t reserved;
    uint64_t macs;       /* multiply-accumulates performed: n^3 */
    uint64_t compute_ns; /* host time spent in the GEMM kernel */
} MmdGemmDone;

typedef struct MMD_PACKED MmdError {
    uint32_t code;
    uint32_t detail; /* e.g. the rejected n */
} MmdError;

_Static_assert(sizeof(MmdHeader) == 16, "MmdHeader layout");
_Static_assert(sizeof(MmdHello) == 8, "MmdHello layout");
_Static_assert(sizeof(MmdHelloAck) == 16, "MmdHelloAck layout");
_Static_assert(sizeof(MmdGemmReq) == 8, "MmdGemmReq layout");
_Static_assert(sizeof(MmdGemmDone) == 24, "MmdGemmDone layout");
_Static_assert(sizeof(MmdError) == 8, "MmdError layout");

static inline uint16_t mmd_le16(uint16_t v) {
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return __builtin_bswap16(v);
#else
    return v;
#endif
}

static inline uint32_t mmd_le32(uint32_t v) {
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return __builtin_bswap32(v);
#else
    return v;
#endif
}

static inline uint64_t mmd_le64(uint64_t v) {
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return __builtin_bswap64(v);
#else
    return v;
#endif
}

#endif
