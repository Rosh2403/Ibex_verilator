/*
 * Guest-visible doorbell mailbox shared by the Ibex firmware and the TCG
 * matmul plugin. The plugin forwards each kick to the matmul accelerator
 * daemon (see include/matmul_daemon_protocol.h).
 *
 * Guest sequence: write N, A, B, C; write STATUS = IDLE; write KICK. The plugin
 * handles the KICK store synchronously, so STATUS is DONE or ERROR by the time
 * the next guest instruction runs.
 */
#ifndef MATMUL_ACCEL_H
#define MATMUL_ACCEL_H

/* In Ibex RAM (0x00100000..0x0011E000); firmware must end below this. */
#define MATMUL_ACCEL_BASE 0x0011C000u
#define MATMUL_ACCEL_N (MATMUL_ACCEL_BASE + 0u)
#define MATMUL_ACCEL_A (MATMUL_ACCEL_BASE + 4u)
#define MATMUL_ACCEL_B (MATMUL_ACCEL_BASE + 8u)
#define MATMUL_ACCEL_C (MATMUL_ACCEL_BASE + 12u)
#define MATMUL_ACCEL_STATUS (MATMUL_ACCEL_BASE + 16u)
#define MATMUL_ACCEL_KICK (MATMUL_ACCEL_BASE + 20u)
#define MATMUL_ACCEL_ERRCODE (MATMUL_ACCEL_BASE + 24u)

/* Largest N the guest may request (the daemon accepts up to MMD_MAX_N). */
#define MATMUL_ACCEL_MAX_N 32

/* STATUS values */
#define MATMUL_ACCEL_IDLE 0u
#define MATMUL_ACCEL_DONE 1u
#define MATMUL_ACCEL_ERROR 2u

/* ERRCODE values, valid when STATUS == ERROR */
#define MATMUL_ACCEL_ERR_NONE 0u
#define MATMUL_ACCEL_ERR_BAD_N 1u      /* N is 0 or above MATMUL_ACCEL_MAX_N */
#define MATMUL_ACCEL_ERR_GUEST_MEM 2u  /* A, B, or C not readable/writable */
#define MATMUL_ACCEL_ERR_DAEMON_IO 3u  /* daemon unreachable or timed out */
#define MATMUL_ACCEL_ERR_REJECTED 4u   /* daemon returned a protocol error */

#endif
