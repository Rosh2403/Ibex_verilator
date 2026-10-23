// SPDX-License-Identifier: Apache-2.0
// 8x8 integer matrix multiply; prints mcycle around the kernel.

#include "demo_system.h"

#define N 8

static int32_t a[N][N];
static int32_t b[N][N];
static int32_t c[N][N];

static uint32_t read_mcycle(void) {
  uint32_t value;
  __asm__ volatile("csrr %0, mcycle" : "=r"(value));
  return value;
}

static void drain_uart(void) {
  volatile uint32_t i;
  for (i = 0; i < 80000u; i++) {
  }
}

int main(void) {
  int i, j, k;
  int32_t sum = 0;
  uint32_t start, cycles;

  for (i = 0; i < N; i++) {
    for (j = 0; j < N; j++) {
      a[i][j] = i + j + 1;
      b[i][j] = i * j + 1;
      c[i][j] = 0;
    }
  }

  start = read_mcycle();
  for (i = 0; i < N; i++) {
    for (j = 0; j < N; j++) {
      int32_t acc = 0;
      for (k = 0; k < N; k++) {
        acc += a[i][k] * b[k][j];
      }
      c[i][j] = acc;
      sum += acc;
    }
  }
  cycles = read_mcycle() - start;

  puts("matmul 8x8\nsum ");
  puthex((uint32_t)sum);
  puts("\nmcycle ");
  puthex(cycles);
  puts("\n");
  drain_uart();
  return 0;
}
