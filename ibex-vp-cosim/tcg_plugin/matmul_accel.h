/* Shared doorbell layout for the TCG matmul plugin and Ibex firmware. */
#ifndef MATMUL_ACCEL_H
#define MATMUL_ACCEL_H

/* In Ibex RAM (0x00100000..0x0011E000), above typical firmware BSS. */
#define MATMUL_ACCEL_BASE 0x0011C000u
#define MATMUL_ACCEL_N (MATMUL_ACCEL_BASE + 0u)
#define MATMUL_ACCEL_A (MATMUL_ACCEL_BASE + 4u)
#define MATMUL_ACCEL_B (MATMUL_ACCEL_BASE + 8u)
#define MATMUL_ACCEL_C (MATMUL_ACCEL_BASE + 12u)
#define MATMUL_ACCEL_STATUS (MATMUL_ACCEL_BASE + 16u)
#define MATMUL_ACCEL_KICK (MATMUL_ACCEL_BASE + 20u)

#define MATMUL_ACCEL_MAX_N 16
#define MATMUL_ACCEL_DONE 1u

#endif
