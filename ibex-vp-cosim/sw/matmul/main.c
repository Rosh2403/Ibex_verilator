// SPDX-License-Identifier: Apache-2.0
// Integer matrix multiply sized with -DN=. Prints checksum and mcycle.

#include "demo_system.h"

#ifndef N
#define N 8
#endif

// Three int32 matrices need 3 * N * N * 4 bytes. 124 KiB RAM holds that
// only through 64x64. Larger N evaluates A and B from the same formulas.
#if N <= 64
#define MATMUL_STORED 1
#else
#define MATMUL_STORED 0
#endif

#if MATMUL_STORED
static int32_t a[N][N];
static int32_t b[N][N];
static int32_t c[N][N];
#endif

static uint64_t read_mcycle(void) {
  uint32_t hi, lo, hi2;
  do {
    __asm__ volatile("csrr %0, mcycleh" : "=r"(hi));
    __asm__ volatile("csrr %0, mcycle" : "=r"(lo));
    __asm__ volatile("csrr %0, mcycleh" : "=r"(hi2));
  } while (hi != hi2);
  return ((uint64_t)hi << 32) | lo;
}

static void putu(uint32_t v) {
  char buf[10];
  int n = 0;
  if (v == 0) {
    putchar('0');
    return;
  }
  while (v) {
    buf[n++] = (char)('0' + (v % 10));
    v /= 10;
  }
  while (n--) {
    putchar(buf[n]);
  }
}

static void puthex64(uint64_t v) {
  puthex((uint32_t)(v >> 32));
  puthex((uint32_t)v);
}

static void drain_uart(void) {
  volatile uint32_t i;
  for (i = 0; i < 80000u; i++) {
  }
}

int main(void) {
  int i, j, k;
  uint64_t sum = 0;
  uint64_t start, cycles;
  // Keep N out of the optimizer so the triple loop is not folded to a constant.
  volatile int n = N;

#if MATMUL_STORED
  for (i = 0; i < n; i++) {
    for (j = 0; j < n; j++) {
      a[i][j] = i + j + 1;
      b[i][j] = i * j + 1;
      c[i][j] = 0;
    }
  }
#endif

  start = read_mcycle();
  for (i = 0; i < n; i++) {
    for (j = 0; j < n; j++) {
      int64_t acc = 0;
      for (k = 0; k < n; k++) {
#if MATMUL_STORED
        acc += (int64_t)a[i][k] * (int64_t)b[k][j];
#else
        acc += (int64_t)(i + k + 1) * (int64_t)(k * j + 1);
#endif
      }
#if MATMUL_STORED
      c[i][j] = (int32_t)acc;
#endif
      sum += (uint64_t)acc;
    }
  }
  cycles = read_mcycle() - start;

  puts("matmul N=");
  putu(N);
  puts("\nmode ");
#if MATMUL_STORED
  puts("stored");
#else
  puts("formula");
#endif
  puts("\nsum ");
  puthex64(sum);
  puts("\nmcycle ");
  puthex64(cycles);
  puts("\n");
  drain_uart();
  return 0;
}
