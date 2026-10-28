/*
 * QEMU TCG plugin: matmul accelerator doorbell -> matmul daemon.
 *
 * The plugin watches guest stores. On a store to MATMUL_ACCEL_KICK it reads
 * the mailbox and matrices A and B from guest RAM, sends them to the matmul
 * accelerator daemon over a Unix socket, waits for C, writes C back to guest
 * RAM, and sets STATUS. The guest instruction stream is held at the kicking
 * store until the daemon replies, so results are deterministic.
 *
 * The plugin does not compute anything itself. Arguments:
 *   socket=PATH       daemon socket (default $IBEX_MATMUL_SOCKET or
 *                     MMD_DEFAULT_SOCKET)
 *   timeout_ms=N      per-request send+reply timeout (default 5000)
 *   log=kicks|summary one line per request (default), or only per-size
 *                     timing totals at exit (for benchmarks)
 *
 * Built with -DMATMUL_PLUGIN_DIRECT, the same plugin computes the GEMM itself
 * instead of calling the daemon. That build exists only as the "direct"
 * baseline for ibex-vp-cosim/scripts/bench_npu.py: same instrumentation, same
 * mailbox handling, same kernel (matmul_gemm.h); only where the math runs differs.
 *
 * QEMU refuses to start if the daemon is not reachable at load time. A later
 * transport failure is retried once on a fresh connection (a GEMM request has
 * no side effects in the daemon); if that also fails the guest sees
 * STATUS = ERROR with ERRCODE = MATMUL_ACCEL_ERR_DAEMON_IO.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <qemu-plugin.h>

#include "matmul_accel.h"
#include "matmul_daemon_protocol.h"
#include "matmul_gemm.h"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0 /* macOS: SO_NOSIGPIPE is set on the socket instead */
#endif

#ifdef MATMUL_PLUGIN_DIRECT
/* The socket client code is unused in this benchmark-only build. */
#pragma GCC diagnostic ignored "-Wunused-function"
#define MODE_NAME "plugin (direct)"
#else
#define MODE_NAME "daemon"
#endif

_Static_assert(MATMUL_ACCEL_MAX_N <= MMD_MAX_N, "guest limit exceeds daemon limit");

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

#define MAX_ELEMS (MATMUL_ACCEL_MAX_N * MATMUL_ACCEL_MAX_N)

static struct {
    GMutex lock;
    char socket_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    int timeout_ms;
    int fd;
    uint32_t seq;
    uint64_t kicks, done, failed;
    bool log_kicks;
    /* Time spent handling successful requests, by matrix size. */
    uint64_t size_count[MATMUL_ACCEL_MAX_N + 1];
    uint64_t size_ns[MATMUL_ACCEL_MAX_N + 1];
} st = {.fd = -1, .timeout_ms = 5000, .log_kicks = true};

static void log_line(const char *fmt, ...) G_GNUC_PRINTF(1, 2);

/* stderr is visible with -nographic; qemu_plugin_outs feeds `-d plugin`. */
static void log_line(const char *fmt, ...) {
    g_autofree char *body = NULL;
    g_autofree char *line = NULL;
    va_list ap;

    va_start(ap, fmt);
    body = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    line = g_strdup_printf("[matmul plugin] %s\n", body);
    fputs(line, stderr);
    qemu_plugin_outs(line);
}

static uint64_t now_ns(void) {
    struct timespec ts;

#ifdef CLOCK_UPTIME_RAW
    clock_gettime(CLOCK_UPTIME_RAW, &ts); /* macOS: ns resolution */
#else
    clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ---------------------------------------------------------------- socket I/O */

/* Waits until `fd` is ready for `events` or the deadline passes. */
static bool wait_ready(int fd, short events, uint64_t deadline_ns) {
    for (;;) {
        uint64_t now = now_ns();
        struct pollfd pfd = {.fd = fd, .events = events};
        int pr;

        if (now >= deadline_ns) {
            errno = ETIMEDOUT;
            return false;
        }
        pr = poll(&pfd, 1, (int)((deadline_ns - now + 999999) / 1000000));
        if (pr > 0) {
            return true;
        }
        if (pr == 0) {
            errno = ETIMEDOUT;
            return false;
        }
        if (errno != EINTR) {
            return false;
        }
    }
}

/*
 * Requests can exceed the socket buffer (8 KiB by default on macOS), so a
 * daemon that stops reading would block a blocking send() forever. The socket
 * is O_NONBLOCK (macOS ignores MSG_DONTWAIT on Unix stream sockets); wait for
 * buffer space against the same deadline as the reply.
 */
static bool send_full(int fd, const void *buf, size_t len, uint64_t deadline_ns) {
    const uint8_t *p = buf;

    while (len > 0) {
        ssize_t w = send(fd, p, len, MSG_NOSIGNAL);
        if (w > 0) {
            p += w;
            len -= (size_t)w;
        } else if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (!wait_ready(fd, POLLOUT, deadline_ns)) {
                return false;
            }
        } else if (w < 0 && errno != EINTR) {
            return false;
        }
    }
    return true;
}

static bool recv_full(int fd, void *buf, size_t len, uint64_t deadline_ns) {
    uint8_t *p = buf;

    while (len > 0) {
        ssize_t r;

        if (!wait_ready(fd, POLLIN, deadline_ns)) {
            return false;
        }
        r = recv(fd, p, len, 0);
        if (r > 0) {
            p += r;
            len -= (size_t)r;
        } else if (r == 0) {
            errno = ECONNRESET;
            return false;
        } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
            return false;
        }
    }
    return true;
}

static void disconnect(void) {
    if (st.fd >= 0) {
        close(st.fd);
        st.fd = -1;
    }
}

/*
 * Send one request and receive its reply. On success the reply header is in
 * *rh and up to `cap` payload bytes are in `resp`. Any failure leaves the
 * stream in an unknown state, so the connection is dropped.
 */
static bool transact(uint16_t type, const void *a, size_t alen, const void *b,
                     size_t blen, MmdHeader *rh, void *resp, size_t cap) {
    uint32_t seq = ++st.seq;
    MmdHeader h = {
        .magic = mmd_le32(MMD_MAGIC),
        .version = mmd_le16(MMD_VERSION),
        .type = mmd_le16(type),
        .seq = mmd_le32(seq),
        .len = mmd_le32((uint32_t)(alen + blen)),
    };
    uint64_t deadline = now_ns() + (uint64_t)st.timeout_ms * 1000000ull;
    uint32_t len;

    if (!send_full(st.fd, &h, sizeof(h), deadline) ||
        (alen && !send_full(st.fd, a, alen, deadline)) ||
        (blen && !send_full(st.fd, b, blen, deadline))) {
        log_line("send to daemon failed: %s", strerror(errno));
        goto fail;
    }
    if (!recv_full(st.fd, rh, sizeof(*rh), deadline)) {
        log_line("no reply from daemon within %d ms: %s", st.timeout_ms,
                 strerror(errno));
        goto fail;
    }
    len = mmd_le32(rh->len);
    if (mmd_le32(rh->magic) != MMD_MAGIC || mmd_le16(rh->version) != MMD_VERSION ||
        mmd_le32(rh->seq) != seq || len > cap) {
        log_line("malformed reply from daemon (seq %" PRIu32 ", len %" PRIu32 ")",
                 mmd_le32(rh->seq), len);
        goto fail;
    }
    if (len && !recv_full(st.fd, resp, len, deadline)) {
        log_line("truncated reply from daemon: %s", strerror(errno));
        goto fail;
    }
    return true;
fail:
    disconnect();
    return false;
}

static bool connect_daemon(void) {
    struct sockaddr_un sa = {.sun_family = AF_UNIX};
    MmdHello hello = {.client_pid = mmd_le32((uint32_t)getpid())};
    union {
        MmdHelloAck ack;
        MmdError err;
    } r;
    MmdHeader rh;
    int fd;

    strcpy(sa.sun_path, st.socket_path); /* length checked at install */
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        log_line("socket: %s", strerror(errno));
        return false;
    }
#ifdef SO_NOSIGPIPE
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &(int){1}, sizeof(int));
#endif
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        log_line("cannot connect to matmul daemon at %s: %s", st.socket_path,
                 strerror(errno));
        close(fd);
        return false;
    }
    if (fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) != 0) {
        log_line("fcntl: %s", strerror(errno));
        close(fd);
        return false;
    }
    st.fd = fd;
    if (!transact(MMD_MSG_HELLO, &hello, sizeof(hello), NULL, 0, &rh, &r, sizeof(r))) {
        return false;
    }
    if (mmd_le16(rh.type) != MMD_MSG_HELLO_ACK || mmd_le32(rh.len) != sizeof(r.ack)) {
        log_line("daemon rejected hello (type %u)", mmd_le16(rh.type));
        disconnect();
        return false;
    }
    log_line("connected to daemon pid %" PRIu32 " at %s (max_n %" PRIu32 ", %" PRIu64
             " GEMMs served so far)",
             mmd_le32(r.ack.daemon_pid), st.socket_path, mmd_le32(r.ack.max_n),
             mmd_le64(r.ack.requests_served));
    return true;
}

/* -------------------------------------------------------------- guest memory */

static bool guest_read(uint64_t addr, void *out, size_t len) {
    g_autoptr(GByteArray) buf = g_byte_array_new();

    if (!qemu_plugin_read_memory_vaddr(addr, buf, len) || buf->len < len) {
        return false;
    }
    memcpy(out, buf->data, len);
    return true;
}

static bool guest_write(uint64_t addr, const void *in, size_t len) {
    g_autoptr(GByteArray) buf = g_byte_array_new();

    g_byte_array_append(buf, in, len);
    return qemu_plugin_write_memory_vaddr(addr, buf);
}

/* Guest is RV32 (little-endian) and so is the wire format. */
static void finish(uint32_t status, uint32_t errcode) {
    uint32_t words[2];

    if (status == MATMUL_ACCEL_DONE) {
        st.done++;
    } else {
        st.failed++;
    }
    words[0] = mmd_le32(errcode);
    guest_write(MATMUL_ACCEL_ERRCODE, &words[0], 4);
    words[1] = mmd_le32(status);
    guest_write(MATMUL_ACCEL_STATUS, &words[1], 4);
}

/* ----------------------------------------------------------------- doorbell */

static void handle_kick(void) {
    static uint32_t ab[2 * MAX_ELEMS];
    static _Alignas(8) uint8_t resp[sizeof(MmdGemmDone) + MAX_ELEMS * sizeof(uint32_t)];
    uint32_t mbox[4];
    MmdGemmReq req;
    MmdGemmDone done;
    MmdHeader rh;
    uint32_t n, a_addr, b_addr, c_addr, bytes;
    uint64_t t0, t_kick = now_ns();
    bool sent = false;

    st.kicks++;
    if (!guest_read(MATMUL_ACCEL_N, mbox, sizeof(mbox))) {
        log_line("kick #%" PRIu64 ": cannot read mailbox", st.kicks);
        finish(MATMUL_ACCEL_ERROR, MATMUL_ACCEL_ERR_GUEST_MEM);
        return;
    }
    n = mmd_le32(mbox[0]);
    a_addr = mmd_le32(mbox[1]);
    b_addr = mmd_le32(mbox[2]);
    c_addr = mmd_le32(mbox[3]);
    if (n == 0 || n > MATMUL_ACCEL_MAX_N) {
        log_line("kick #%" PRIu64 ": rejected N=%" PRIu32 " (allowed 1..%d)", st.kicks,
                 n, MATMUL_ACCEL_MAX_N);
        finish(MATMUL_ACCEL_ERROR, MATMUL_ACCEL_ERR_BAD_N);
        return;
    }
    bytes = n * n * (uint32_t)sizeof(uint32_t);
    if (!guest_read(a_addr, ab, bytes) || !guest_read(b_addr, ab + n * n, bytes)) {
        log_line("kick #%" PRIu64 ": cannot read A@0x%08" PRIx32 " / B@0x%08" PRIx32,
                 st.kicks, a_addr, b_addr);
        finish(MATMUL_ACCEL_ERROR, MATMUL_ACCEL_ERR_GUEST_MEM);
        return;
    }

#ifdef MATMUL_PLUGIN_DIRECT
    t0 = now_ns();
    {
        uint32_t *c = (uint32_t *)(resp + sizeof(MmdGemmDone));

        mmd_gemm(n, ab, ab + n * n, c);
        done = (MmdGemmDone){.n = n, .macs = (uint64_t)n * n * n};
        done.compute_ns = now_ns() - t0;
    }
    (void)req;
    (void)rh;
    (void)sent;
#else
    req = (MmdGemmReq){.n = mmd_le32(n)};
    t0 = now_ns();
    for (int attempt = 0; attempt < 2 && !sent; attempt++) {
        if (st.fd < 0 && !connect_daemon()) {
            continue;
        }
        sent = transact(MMD_MSG_GEMM, &req, sizeof(req), ab, 2 * (size_t)bytes, &rh,
                        resp, sizeof(resp));
    }
    if (!sent) {
        log_line("kick #%" PRIu64 ": daemon unavailable", st.kicks);
        finish(MATMUL_ACCEL_ERROR, MATMUL_ACCEL_ERR_DAEMON_IO);
        return;
    }
    if (mmd_le16(rh.type) == MMD_MSG_ERROR && mmd_le32(rh.len) == sizeof(MmdError)) {
        MmdError err;

        memcpy(&err, resp, sizeof(err));
        log_line("kick #%" PRIu64 ": daemon error %" PRIu32 " (detail %" PRIu32 ")",
                 st.kicks, mmd_le32(err.code), mmd_le32(err.detail));
        finish(MATMUL_ACCEL_ERROR, MATMUL_ACCEL_ERR_REJECTED);
        return;
    }
    memcpy(&done, resp, sizeof(done));
    if (mmd_le16(rh.type) != MMD_MSG_GEMM_DONE ||
        mmd_le32(rh.len) != sizeof(done) + bytes || mmd_le32(done.n) != n) {
        log_line("kick #%" PRIu64 ": unexpected reply type %u len %" PRIu32, st.kicks,
                 mmd_le16(rh.type), mmd_le32(rh.len));
        disconnect();
        finish(MATMUL_ACCEL_ERROR, MATMUL_ACCEL_ERR_REJECTED);
        return;
    }
#endif
    if (!guest_write(c_addr, resp + sizeof(done), bytes)) {
        log_line("kick #%" PRIu64 ": cannot write C@0x%08" PRIx32, st.kicks, c_addr);
        finish(MATMUL_ACCEL_ERROR, MATMUL_ACCEL_ERR_GUEST_MEM);
        return;
    }
    finish(MATMUL_ACCEL_DONE, MATMUL_ACCEL_ERR_NONE);
    st.size_count[n]++;
    st.size_ns[n] += now_ns() - t_kick;
    if (st.log_kicks) {
        log_line("kick #%" PRIu64 ": %" PRIu32 "x%" PRIu32 " GEMM A@0x%08" PRIx32
                 " B@0x%08" PRIx32 " -> C@0x%08" PRIx32 " by %s: %" PRIu64
                 " MACs, compute %.1f us, round trip %.1f us",
                 st.kicks, n, n, a_addr, b_addr, c_addr, MODE_NAME,
                 mmd_le64(done.macs), mmd_le64(done.compute_ns) / 1000.0,
                 (now_ns() - t0) / 1000.0);
    }
}

static void vcpu_mem_cb(unsigned int vcpu_index, qemu_plugin_meminfo_t info,
                        uint64_t vaddr, void *udata) {
    (void)vcpu_index;
    (void)udata;

    if (vaddr != MATMUL_ACCEL_KICK || !qemu_plugin_mem_is_store(info)) {
        return;
    }
    g_mutex_lock(&st.lock);
    handle_kick();
    g_mutex_unlock(&st.lock);
}

static void vcpu_tb_trans(qemu_plugin_id_t id, struct qemu_plugin_tb *tb) {
    size_t n = qemu_plugin_tb_n_insns(tb);

    (void)id;
    for (size_t i = 0; i < n; i++) {
        qemu_plugin_register_vcpu_mem_cb(qemu_plugin_tb_get_insn(tb, i), vcpu_mem_cb,
                                         QEMU_PLUGIN_CB_NO_REGS, QEMU_PLUGIN_MEM_W,
                                         NULL);
    }
}

static void plugin_exit(qemu_plugin_id_t id, void *udata) {
    (void)id;
    (void)udata;
    g_mutex_lock(&st.lock);
    log_line("exit: %" PRIu64 " kicks, %" PRIu64 " done, %" PRIu64 " failed", st.kicks,
             st.done, st.failed);
    for (int n = 1; n <= MATMUL_ACCEL_MAX_N; n++) {
        if (st.size_count[n]) {
            log_line("timing [%s] n=%d: %" PRIu64 " requests, avg %.2f us per request",
                     MODE_NAME, n, st.size_count[n],
                     st.size_ns[n] / 1000.0 / (double)st.size_count[n]);
        }
    }
    disconnect();
    g_mutex_unlock(&st.lock);
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
                                           const qemu_info_t *info, int argc,
                                           char **argv) {
    const char *sock = getenv(MMD_SOCKET_ENV);

    if (!info->system_emulation || strcmp(info->target_name, "riscv32") != 0) {
        fprintf(stderr, "[matmul plugin] requires qemu-system-riscv32 (got %s)\n",
                info->target_name);
        return -1;
    }
    for (int i = 0; i < argc; i++) {
        g_auto(GStrv) kv = g_strsplit(argv[i], "=", 2);

        if (kv[0] && kv[1] && !strcmp(kv[0], "socket")) {
            sock = argv[i] + strlen("socket=");
        } else if (kv[0] && kv[1] && !strcmp(kv[0], "timeout_ms")) {
            st.timeout_ms = atoi(kv[1]);
            if (st.timeout_ms <= 0) {
                fprintf(stderr, "[matmul plugin] bad timeout_ms=%s\n", kv[1]);
                return -1;
            }
        } else if (kv[0] && kv[1] && !strcmp(kv[0], "log") &&
                   (!strcmp(kv[1], "kicks") || !strcmp(kv[1], "summary"))) {
            st.log_kicks = !strcmp(kv[1], "kicks");
        } else {
            fprintf(stderr, "[matmul plugin] unknown argument '%s' "
                            "(use socket=PATH, timeout_ms=N, log=kicks|summary)\n",
                    argv[i]);
            return -1;
        }
    }
    if (!sock || !*sock) {
        sock = MMD_DEFAULT_SOCKET;
    }
    if (strlen(sock) >= sizeof(st.socket_path)) {
        fprintf(stderr, "[matmul plugin] socket path too long: %s\n", sock);
        return -1;
    }
    strcpy(st.socket_path, sock);
    g_mutex_init(&st.lock);

#ifndef MATMUL_PLUGIN_DIRECT
    if (!connect_daemon()) {
        fprintf(stderr, "[matmul plugin] start it first: "
                        "ibex-vp-cosim/matmul_daemon.sh start\n");
        return -1;
    }
#endif
    log_line("loaded; doorbell store at 0x%08x, mailbox at 0x%08x", MATMUL_ACCEL_KICK,
             MATMUL_ACCEL_BASE);
    qemu_plugin_register_vcpu_tb_trans_cb(id, vcpu_tb_trans);
    qemu_plugin_register_atexit_cb(id, plugin_exit, NULL);
    return 0;
}
