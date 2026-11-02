// SPDX-License-Identifier: Apache-2.0
//
// The NPU's GEMM kernel, shared by the matmul daemon and the benchmark-only
// direct plugin build so both run exactly the same math.

#ifndef MATMUL_GEMM_H
#define MATMUL_GEMM_H

#include <stdint.h>
#include <string.h>

/* C = A * B with RV32-style wrap-around. i-k-j order keeps B row-contiguous. */
static inline void mmd_gemm(uint32_t n, const uint32_t *a, const uint32_t *b,
                            uint32_t *c) {
    memset(c, 0, (size_t)n * n * sizeof(*c));
    for (uint32_t i = 0; i < n; i++) {
        for (uint32_t k = 0; k < n; k++) {
            uint32_t aik = a[i * n + k];
            for (uint32_t j = 0; j < n; j++) {
                c[i * n + j] += aik * b[k * n + j];
            }
        }
    }
}

#endif
