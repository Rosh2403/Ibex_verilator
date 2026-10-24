/*
 * TCG plugin: intercept a store to MATMUL_ACCEL_KICK, read A and B from
 * guest RAM, compute C = A * B on the host, write C back.
 *
 * Plugins are not a SysBus device. This uses qemu_plugin_read/write_memory_vaddr
 * (QEMU plugin ABI v5). It does not model RTL timing.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include <qemu-plugin.h>

#include "matmul_accel.h"

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

static int in_accel;

static uint32_t load_u32(uint64_t addr) {
    g_autoptr(GByteArray) buf = g_byte_array_new();
    uint32_t value = 0;

    if (!qemu_plugin_read_memory_vaddr(addr, buf, sizeof(value)) ||
        buf->len < sizeof(value)) {
        return 0;
    }
    memcpy(&value, buf->data, sizeof(value));
    return value;
}

static bool store_u32(uint64_t addr, uint32_t value) {
    g_autoptr(GByteArray) buf = g_byte_array_new();

    g_byte_array_append(buf, (const guint8 *)&value, sizeof(value));
    return qemu_plugin_write_memory_vaddr(addr, buf);
}

static bool read_matrix(uint64_t addr, int n, int32_t *out) {
    size_t bytes = (size_t)n * (size_t)n * sizeof(int32_t);
    g_autoptr(GByteArray) buf = g_byte_array_new();

    if (!qemu_plugin_read_memory_vaddr(addr, buf, bytes) || buf->len < bytes) {
        return false;
    }
    memcpy(out, buf->data, bytes);
    return true;
}

static bool write_matrix(uint64_t addr, int n, const int32_t *in) {
    size_t bytes = (size_t)n * (size_t)n * sizeof(int32_t);
    g_autoptr(GByteArray) buf = g_byte_array_new();

    g_byte_array_append(buf, (const guint8 *)in, bytes);
    return qemu_plugin_write_memory_vaddr(addr, buf);
}

static void run_matmul(void) {
    int32_t a[MATMUL_ACCEL_MAX_N * MATMUL_ACCEL_MAX_N];
    int32_t b[MATMUL_ACCEL_MAX_N * MATMUL_ACCEL_MAX_N];
    int32_t c[MATMUL_ACCEL_MAX_N * MATMUL_ACCEL_MAX_N];
    uint32_t n = load_u32(MATMUL_ACCEL_N);
    uint32_t a_addr = load_u32(MATMUL_ACCEL_A);
    uint32_t b_addr = load_u32(MATMUL_ACCEL_B);
    uint32_t c_addr = load_u32(MATMUL_ACCEL_C);
    int i, j, k;
    char msg[160];

    if (n == 0 || n > MATMUL_ACCEL_MAX_N) {
        snprintf(msg, sizeof(msg),
                 "[matmul plugin] bad N=%" PRIu32 "\n", n);
        qemu_plugin_outs(msg);
        fprintf(stderr, "%s", msg);
        return;
    }
    if (!read_matrix(a_addr, (int)n, a) || !read_matrix(b_addr, (int)n, b)) {
        qemu_plugin_outs("[matmul plugin] failed to read A/B from guest\n");
        fprintf(stderr, "[matmul plugin] failed to read A/B from guest\n");
        return;
    }

    for (i = 0; i < (int)n; i++) {
        for (j = 0; j < (int)n; j++) {
            int32_t acc = 0;
            for (k = 0; k < (int)n; k++) {
                acc += a[i * (int)n + k] * b[k * (int)n + j];
            }
            c[i * (int)n + j] = acc;
        }
    }

    if (!write_matrix(c_addr, (int)n, c) ||
        !store_u32(MATMUL_ACCEL_STATUS, MATMUL_ACCEL_DONE)) {
        qemu_plugin_outs("[matmul plugin] failed to write C/status\n");
        fprintf(stderr, "[matmul plugin] failed to write C/status\n");
        return;
    }

    snprintf(msg, sizeof(msg),
             "[matmul plugin] host %ux%u GEMM  A@0x%08" PRIx32
             " B@0x%08" PRIx32 " -> C@0x%08" PRIx32 "\n",
             n, n, a_addr, b_addr, c_addr);
    qemu_plugin_outs(msg);
    fprintf(stderr, "%s", msg);
}

static void vcpu_mem_cb(unsigned int vcpu_index, qemu_plugin_meminfo_t info,
                        uint64_t vaddr, void *udata) {
    (void)vcpu_index;
    (void)udata;

    if (in_accel || vaddr != MATMUL_ACCEL_KICK ||
        !qemu_plugin_mem_is_store(info)) {
        return;
    }
    in_accel = 1;
    run_matmul();
    in_accel = 0;
}

static void vcpu_tb_trans(qemu_plugin_id_t id, struct qemu_plugin_tb *tb) {
    size_t n = qemu_plugin_tb_n_insns(tb);
    size_t i;

    (void)id;
    for (i = 0; i < n; i++) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
        qemu_plugin_register_vcpu_mem_cb(insn, vcpu_mem_cb,
                                         QEMU_PLUGIN_CB_NO_REGS,
                                         QEMU_PLUGIN_MEM_W, NULL);
    }
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
                                           const qemu_info_t *info, int argc,
                                           char **argv) {
    (void)info;
    (void)argc;
    (void)argv;
    qemu_plugin_register_vcpu_tb_trans_cb(id, vcpu_tb_trans);
    qemu_plugin_outs("[matmul plugin] loaded; kick store at 0x0011C014\n");
    fprintf(stderr, "[matmul plugin] loaded; kick store at 0x0011C014\n");
    return 0;
}
