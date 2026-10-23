// SPDX-License-Identifier: Apache-2.0
// Bare-metal 4x4 GEMM offload: store to TCG-plugin doorbell, then check C.

#include "demo_system.h"
#include "matmul_accel.h"

#define N 4

static int32_t a[N][N];
static int32_t b[N][N];
static int32_t c[N][N];

static void mmio_write(uint32_t addr, uint32_t value) {
  *(volatile uint32_t *)addr = value;
}

static uint32_t mmio_read(uint32_t addr) {
  return *(volatile uint32_t *)addr;
}

static void drain_uart(void) {
  volatile uint32_t i;
  for (i = 0; i < 40000u; i++) {
  }
}

int main(void) {
  int i, j, k;
  int32_t sum = 0;
  int32_t expect = 0;

  for (i = 0; i < N; i++) {
    for (j = 0; j < N; j++) {
      a[i][j] = i + j + 1;
      b[i][j] = i * j + 1;
      c[i][j] = 0xA5A5A5A5;
    }
  }

  mmio_write(MATMUL_ACCEL_N, N);
  mmio_write(MATMUL_ACCEL_A, (uint32_t)(uintptr_t)&a[0][0]);
  mmio_write(MATMUL_ACCEL_B, (uint32_t)(uintptr_t)&b[0][0]);
  mmio_write(MATMUL_ACCEL_C, (uint32_t)(uintptr_t)&c[0][0]);
  mmio_write(MATMUL_ACCEL_STATUS, 0);

  puts("guest: kick host matmul\n");
  mmio_write(MATMUL_ACCEL_KICK, 1);

  while (mmio_read(MATMUL_ACCEL_STATUS) != MATMUL_ACCEL_DONE) {
  }

  for (i = 0; i < N; i++) {
    for (j = 0; j < N; j++) {
      int32_t acc = 0;
      for (k = 0; k < N; k++) {
        acc += a[i][k] * b[k][j];
      }
      expect += acc;
      sum += c[i][j];
    }
  }

  puts("guest: C sum ");
  puthex((uint32_t)sum);
  puts(" expect ");
  puthex((uint32_t)expect);
  puts(sum == expect ? "\nPASS\n" : "\nFAIL\n");
  drain_uart();
  return sum == expect ? 0 : 1;
}
