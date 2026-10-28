// SPDX-License-Identifier: Apache-2.0
//
// Benchmark firmware for scripts/bench_npu.py. Two builds:
//   -DBENCH_NPU  BENCH_REPS NPU requests for each size, then a correctness check
//   (default)    guest CPU + UART workload with no NPU requests, to measure
//                the cost of having the plugin loaded
// Exits QEMU through sim_ctrl: code 0 on success, 1 on failure.

#include <stdint.h>

#include "demo_system.h"
#include "matmul_accel.h"

#define MAX_N MATMUL_ACCEL_MAX_N
#ifndef BENCH_REPS
#define BENCH_REPS 2000u
#endif
#ifndef IO_REPS
#define IO_REPS 6000u
#endif

static int32_t a[MAX_N * MAX_N];
static int32_t b[MAX_N * MAX_N];
static int32_t c[MAX_N * MAX_N];

static void reg_write(uint32_t addr, uint32_t value) {
  *(volatile uint32_t *)addr = value;
}

__attribute__((unused)) static uint32_t reg_read(uint32_t addr) { return *(volatile uint32_t *)addr; }

static void fill(uint32_t n) {
  uint32_t s = 0x1234567u, i;

  for (i = 0; i < n * n; i++) {
    s = s * 1103515245u + 12345u;
    a[i] = (int32_t)((s >> 16) & 0xFFu) - 128;
    s = s * 1103515245u + 12345u;
    b[i] = (int32_t)((s >> 16) & 0xFFu) - 128;
  }
}

__attribute__((unused)) static void soft_gemm(uint32_t n) {
  uint32_t i, j, k;

  for (i = 0; i < n; i++) {
    for (j = 0; j < n; j++) {
      int32_t acc = 0;
      for (k = 0; k < n; k++) {
        acc += a[i * n + k] * b[k * n + j];
      }
      c[i * n + j] = acc;
    }
  }
}

static void finish(int pass) {
  volatile uint32_t i;

  puts(pass ? "PASS\n" : "FAIL\n");
  for (i = 0; i < 20000u; i++) {
  }
  reg_write(SIM_CTRL_BASE + SIM_CTRL_CTRL, pass ? 1u : (1u << 1) | 1u);
  for (;;) {
  }
}

#ifdef BENCH_NPU
static int run_size(uint32_t n) {
  uint32_t r, i, j, k;

  fill(n);
  reg_write(MATMUL_ACCEL_N, n);
  reg_write(MATMUL_ACCEL_A, (uint32_t)(uintptr_t)a);
  reg_write(MATMUL_ACCEL_B, (uint32_t)(uintptr_t)b);
  reg_write(MATMUL_ACCEL_C, (uint32_t)(uintptr_t)c);
  for (r = 0; r < BENCH_REPS; r++) {
    reg_write(MATMUL_ACCEL_STATUS, MATMUL_ACCEL_IDLE);
    reg_write(MATMUL_ACCEL_KICK, 1u);
    if (reg_read(MATMUL_ACCEL_STATUS) != MATMUL_ACCEL_DONE) {
      return 0;
    }
  }
  for (i = 0; i < n; i++) {
    for (j = 0; j < n; j++) {
      int32_t acc = 0;
      for (k = 0; k < n; k++) {
        acc += a[i * n + k] * b[k * n + j];
      }
      if (c[i * n + j] != acc) {
        return 0;
      }
    }
  }
  return 1;
}

int main(void) {
  static const uint32_t sizes[] = {4, 8, 16, MAX_N};
  int ok = 1;
  uint32_t i;

  puts("bench: NPU requests\n");
  for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
    ok &= run_size(sizes[i]);
  }
  finish(ok);
  return 0;
}
#else
int main(void) {
  uint32_t r, i, sum = 0;

  puts("bench: guest CPU + UART workload, no NPU requests\n");
  fill(MAX_N);
  for (r = 0; r < IO_REPS; r++) {
    soft_gemm(MAX_N);
    for (i = 0; i < MAX_N * MAX_N; i++) {
      sum += (uint32_t)c[i];
    }
    puts("iteration ");
    puthex(r);
    puts(" checksum ");
    puthex(sum);
    puts("\n");
  }
  finish(1);
  return 0;
}
#endif
