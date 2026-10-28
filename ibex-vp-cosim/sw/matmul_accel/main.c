// SPDX-License-Identifier: Apache-2.0
//
// Ibex firmware for the daemon-backed matmul accelerator. For each size it
// fills A and B, kicks the doorbell, and checks every element of C against a
// software GEMM computed on the guest. It also checks that an out-of-range N
// is rejected. Exits QEMU through sim_ctrl: code 0 on PASS, 1 on FAIL.

#include <stdint.h>

#include "demo_system.h"
#include "matmul_accel.h"

#define MAX_N MATMUL_ACCEL_MAX_N
#define POLL_LIMIT 100000u

static int32_t a[MAX_N * MAX_N];
static int32_t b[MAX_N * MAX_N];
static int32_t c[MAX_N * MAX_N];

extern char _bss_end[];

static void reg_write(uint32_t addr, uint32_t value) {
  *(volatile uint32_t *)addr = value;
}

static uint32_t reg_read(uint32_t addr) { return *(volatile uint32_t *)addr; }

static uint32_t lcg_state = 0x1234567u;

static int32_t next_value(void) {
  lcg_state = lcg_state * 1103515245u + 12345u;
  return (int32_t)((lcg_state >> 16) & 0xFFu) - 128; /* -128..127 */
}

static void put_dec(uint32_t v) {
  char buf[11];
  int i = 10;

  buf[i] = '\0';
  do {
    buf[--i] = (char)('0' + v % 10u);
    v /= 10u;
  } while (v != 0u);
  puts(&buf[i]);
}

/* Kicks the accelerator and returns the final STATUS (IDLE = no response). */
/* noinline: keeps kick() and finish() as GDB breakpoint targets. */
__attribute__((noinline)) static uint32_t kick(uint32_t n) {
  uint32_t status = MATMUL_ACCEL_IDLE;
  uint32_t i;

  reg_write(MATMUL_ACCEL_N, n);
  reg_write(MATMUL_ACCEL_A, (uint32_t)(uintptr_t)a);
  reg_write(MATMUL_ACCEL_B, (uint32_t)(uintptr_t)b);
  reg_write(MATMUL_ACCEL_C, (uint32_t)(uintptr_t)c);
  reg_write(MATMUL_ACCEL_ERRCODE, MATMUL_ACCEL_ERR_NONE);
  reg_write(MATMUL_ACCEL_STATUS, MATMUL_ACCEL_IDLE);
  reg_write(MATMUL_ACCEL_KICK, 1u);

  /* The plugin answers synchronously; the bound only catches a missing plugin. */
  for (i = 0; i < POLL_LIMIT && status == MATMUL_ACCEL_IDLE; i++) {
    status = reg_read(MATMUL_ACCEL_STATUS);
  }
  return status;
}

/* Returns the number of mismatching elements. */
static uint32_t run_size(uint32_t n) {
  uint32_t status, errors = 0, i, j, k;
  uint32_t checksum = 0;

  for (i = 0; i < n * n; i++) {
    a[i] = next_value();
    b[i] = next_value();
    c[i] = (int32_t)0xA5A5A5A5u;
  }

  puts("guest: kick ");
  put_dec(n);
  puts("x");
  put_dec(n);
  puts(" ... ");
  status = kick(n);
  if (status != MATMUL_ACCEL_DONE) {
    puts(status == MATMUL_ACCEL_IDLE ? "no response (is the plugin loaded?)\n"
                                     : "accelerator error ");
    if (status != MATMUL_ACCEL_IDLE) {
      put_dec(reg_read(MATMUL_ACCEL_ERRCODE));
      puts("\n");
    }
    return 1;
  }

  for (i = 0; i < n; i++) {
    for (j = 0; j < n; j++) {
      int32_t acc = 0;
      for (k = 0; k < n; k++) {
        acc += a[i * n + k] * b[k * n + j];
      }
      if (c[i * n + j] != acc) {
        errors++;
      }
      checksum += (uint32_t)c[i * n + j];
    }
  }
  puts(errors == 0 ? "ok  checksum " : "MISMATCH  checksum ");
  puthex(checksum);
  if (errors != 0) {
    puts("  bad elements ");
    put_dec(errors);
  }
  puts("\n");
  return errors;
}

__attribute__((noinline)) static void finish(int pass) {
  volatile uint32_t i;

  puts(pass ? "PASS\n" : "FAIL\n");
  for (i = 0; i < 20000u; i++) {
  }
  /* sim_ctrl: bit 0 halts QEMU, bits [7:1] carry the exit code. */
  reg_write(SIM_CTRL_BASE + SIM_CTRL_CTRL, pass ? 1u : (1u << 1) | 1u);
  for (;;) {
  }
}

int main(void) {
  static const uint32_t sizes[] = {4, 8, 16, MAX_N};
  uint32_t failures = 0, i, status;

  puts("guest: matmul accelerator test (QEMU TCG plugin -> matmul daemon)\n");
  if ((uint32_t)(uintptr_t)_bss_end > MATMUL_ACCEL_BASE) {
    puts("guest: firmware overlaps the accelerator mailbox\n");
    finish(0);
  }

  for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
    failures += run_size(sizes[i]) != 0;
  }

  puts("guest: kick N=");
  put_dec(MAX_N + 1);
  puts(" (expect rejection) ... ");
  status = kick(MAX_N + 1);
  if (status == MATMUL_ACCEL_ERROR &&
      reg_read(MATMUL_ACCEL_ERRCODE) == MATMUL_ACCEL_ERR_BAD_N) {
    puts("rejected\n");
  } else {
    puts("NOT rejected\n");
    failures++;
  }

  finish(failures == 0);
  return 0;
}
