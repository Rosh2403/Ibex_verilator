// SPDX-License-Identifier: Apache-2.0
//
// Matmul accelerator daemon. A separate host process that owns a Unix socket
// and computes C = A * B for any number of QEMU TCG plugin clients. Protocol:
// include/matmul_daemon_protocol.h.
//
//   matmul_daemon --socket PATH                 run in the foreground
//   matmul_daemon --socket PATH --daemon        detach; log + pidfile beside socket
//   matmul_daemon --socket PATH --ping          query a running daemon
//
// In --daemon mode the launching process exits only after the socket is
// listening, so a caller can connect as soon as the command returns.

#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "matmul_daemon_protocol.h"
#include "matmul_gemm.h"

static volatile sig_atomic_t g_stop;
static int g_wake_pipe[2] = {-1, -1};
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_served;
static unsigned g_next_client;

static char g_socket_path[PATH_MAX];
static char g_pid_path[PATH_MAX];
static char g_log_path[PATH_MAX];
static dev_t g_socket_dev;
static ino_t g_socket_ino;

static void log_msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void log_msg(const char *fmt, ...) {
    char line[512];
    struct timeval tv;
    struct tm tm;
    va_list ap;
    int off;

    gettimeofday(&tv, NULL);
    localtime_r(&tv.tv_sec, &tm);
    off = snprintf(line, sizeof(line), "%02d:%02d:%02d.%03d [matmul-daemon] ",
                   tm.tm_hour, tm.tm_min, tm.tm_sec, (int)(tv.tv_usec / 1000));
    va_start(ap, fmt);
    vsnprintf(line + off, sizeof(line) - (size_t)off, fmt, ap);
    va_end(ap);
    fprintf(stderr, "%s\n", line);
    fflush(stderr);
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

/* Returns 1 on success, 0 on clean EOF before any byte, -1 on error/short. */
static int read_full(int fd, void *buf, size_t len) {
    uint8_t *p = buf;
    size_t got = 0;

    while (got < len) {
        ssize_t r = read(fd, p + got, len - got);
        if (r > 0) {
            got += (size_t)r;
        } else if (r == 0) {
            return got == 0 ? 0 : -1;
        } else if (errno != EINTR) {
            return -1;
        }
    }
    return 1;
}

static bool write_full(int fd, const void *buf, size_t len) {
    const uint8_t *p = buf;

    while (len > 0) {
        ssize_t w = write(fd, p, len);
        if (w > 0) {
            p += w;
            len -= (size_t)w;
        } else if (w < 0 && errno != EINTR) {
            return false;
        }
    }
    return true;
}

static bool send_msg(int fd, uint16_t type, uint32_t seq, const void *a,
                     size_t alen, const void *b, size_t blen) {
    MmdHeader h = {
        .magic = mmd_le32(MMD_MAGIC),
        .version = mmd_le16(MMD_VERSION),
        .type = mmd_le16(type),
        .seq = mmd_le32(seq),
        .len = mmd_le32((uint32_t)(alen + blen)),
    };

    return write_full(fd, &h, sizeof(h)) && (alen == 0 || write_full(fd, a, alen)) &&
           (blen == 0 || write_full(fd, b, blen));
}

static bool send_error(int fd, uint32_t seq, uint32_t code, uint32_t detail) {
    MmdError e = {.code = mmd_le32(code), .detail = mmd_le32(detail)};

    return send_msg(fd, MMD_MSG_ERROR, seq, &e, sizeof(e), NULL, 0);
}

typedef struct Client {
    int fd;
    unsigned id;
} Client;

/* Handles one GEMM. Returns false if the connection must be closed. */
static bool handle_gemm(Client *cl, uint32_t seq, uint8_t *payload, uint32_t len) {
    static const size_t hdr = sizeof(MmdGemmReq);
    uint32_t c[MMD_MAX_N * MMD_MAX_N];
    MmdGemmReq req;
    MmdGemmDone done;
    uint32_t *a, *b;
    uint32_t n;
    size_t elems;
    uint64_t t0, dt, served;

    if (len < hdr) {
        send_error(cl->fd, seq, MMD_ERR_BAD_LENGTH, len);
        return false;
    }
    memcpy(&req, payload, hdr);
    n = mmd_le32(req.n);
    if (n == 0 || n > MMD_MAX_N || req.flags != 0) {
        log_msg("client %u seq %" PRIu32 ": rejected n=%" PRIu32, cl->id, seq, n);
        return send_error(cl->fd, seq, MMD_ERR_BAD_N, n);
    }
    elems = (size_t)n * n;
    if (len != hdr + 2 * elems * sizeof(uint32_t)) {
        log_msg("client %u seq %" PRIu32 ": bad length %" PRIu32 " for n=%" PRIu32,
                cl->id, seq, len, n);
        send_error(cl->fd, seq, MMD_ERR_BAD_LENGTH, len);
        return false;
    }

    /* The payload buffer is 4-byte aligned (malloc) and hdr is 8 bytes. */
    a = (uint32_t *)(payload + hdr);
    b = a + elems;
    for (size_t i = 0; i < 2 * elems; i++) {
        a[i] = mmd_le32(a[i]);
    }

    t0 = now_ns();
    mmd_gemm(n, a, b, c);
    dt = now_ns() - t0;

    for (size_t i = 0; i < elems; i++) {
        c[i] = mmd_le32(c[i]);
    }
    pthread_mutex_lock(&g_lock);
    served = ++g_served;
    pthread_mutex_unlock(&g_lock);

    done = (MmdGemmDone){
        .n = mmd_le32(n),
        .macs = mmd_le64((uint64_t)n * n * n),
        .compute_ns = mmd_le64(dt),
    };
    log_msg("client %u seq %" PRIu32 ": GEMM %" PRIu32 "x%" PRIu32 " (%" PRIu64
            " MACs) in %" PRIu64 " ns [total %" PRIu64 "]",
            cl->id, seq, n, n, (uint64_t)n * n * n, dt, served);
    return send_msg(cl->fd, MMD_MSG_GEMM_DONE, seq, &done, sizeof(done), c,
                    elems * sizeof(uint32_t));
}

static void *client_thread(void *arg) {
    Client *cl = arg;
    uint8_t *payload = malloc(MMD_MAX_PAYLOAD);
    MmdHeader h;

    if (!payload) {
        log_msg("client %u: out of memory", cl->id);
        goto out;
    }
    for (;;) {
        uint32_t seq, len;
        uint16_t type;
        int r = read_full(cl->fd, &h, sizeof(h));

        if (r == 0) {
            log_msg("client %u: disconnected", cl->id);
            break;
        }
        if (r < 0) {
            log_msg("client %u: read failed or truncated header", cl->id);
            break;
        }
        seq = mmd_le32(h.seq);
        len = mmd_le32(h.len);
        type = mmd_le16(h.type);
        if (mmd_le32(h.magic) != MMD_MAGIC) {
            log_msg("client %u: bad magic 0x%08" PRIx32 "; closing", cl->id,
                    mmd_le32(h.magic));
            send_error(cl->fd, seq, MMD_ERR_BAD_MAGIC, mmd_le32(h.magic));
            break;
        }
        if (mmd_le16(h.version) != MMD_VERSION) {
            log_msg("client %u: protocol version %u, expected %u; closing", cl->id,
                    mmd_le16(h.version), MMD_VERSION);
            send_error(cl->fd, seq, MMD_ERR_BAD_VERSION, mmd_le16(h.version));
            break;
        }
        if (len > MMD_MAX_PAYLOAD) {
            log_msg("client %u: payload %" PRIu32 " bytes exceeds limit; closing",
                    cl->id, len);
            send_error(cl->fd, seq, MMD_ERR_BAD_LENGTH, len);
            break;
        }
        if (len > 0 && read_full(cl->fd, payload, len) != 1) {
            log_msg("client %u: truncated payload", cl->id);
            break;
        }

        if (type == MMD_MSG_HELLO) {
            MmdHello hello;
            MmdHelloAck ack;

            if (len != sizeof(hello)) {
                send_error(cl->fd, seq, MMD_ERR_BAD_LENGTH, len);
                break;
            }
            memcpy(&hello, payload, sizeof(hello));
            pthread_mutex_lock(&g_lock);
            ack = (MmdHelloAck){
                .daemon_pid = mmd_le32((uint32_t)getpid()),
                .max_n = mmd_le32(MMD_MAX_N),
                .requests_served = mmd_le64(g_served),
            };
            pthread_mutex_unlock(&g_lock);
            log_msg("client %u: hello from pid %" PRIu32, cl->id,
                    mmd_le32(hello.client_pid));
            if (!send_msg(cl->fd, MMD_MSG_HELLO_ACK, seq, &ack, sizeof(ack), NULL, 0)) {
                break;
            }
        } else if (type == MMD_MSG_GEMM) {
            if (!handle_gemm(cl, seq, payload, len)) {
                break;
            }
        } else {
            log_msg("client %u: unknown message type %u", cl->id, type);
            if (!send_error(cl->fd, seq, MMD_ERR_BAD_TYPE, type)) {
                break;
            }
        }
    }
out:
    free(payload);
    close(cl->fd);
    free(cl);
    return NULL;
}

static void on_signal(int sig) {
    int saved = errno;
    uint8_t b = (uint8_t)sig;

    g_stop = 1;
    if (g_wake_pipe[1] >= 0) {
        (void)!write(g_wake_pipe[1], &b, 1);
    }
    errno = saved;
}

static bool make_absolute(char *dst, const char *src) {
    char cwd[PATH_MAX];

    if (src[0] == '/') {
        return snprintf(dst, PATH_MAX, "%s", src) < PATH_MAX;
    }
    if (!getcwd(cwd, sizeof(cwd))) {
        return false;
    }
    return snprintf(dst, PATH_MAX, "%s/%s", cwd, src) < PATH_MAX;
}

static bool fill_sockaddr(struct sockaddr_un *sa, const char *path) {
    memset(sa, 0, sizeof(*sa));
    sa->sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(sa->sun_path)) {
        fprintf(stderr, "matmul_daemon: socket path too long (max %zu): %s\n",
                sizeof(sa->sun_path) - 1, path);
        return false;
    }
    strcpy(sa->sun_path, path);
    return true;
}

/* Refuses to start over a live daemon; removes a stale socket file. */
static bool clear_stale_socket(const char *path) {
    struct sockaddr_un sa;
    struct stat st;
    int fd;

    if (lstat(path, &st) != 0) {
        return errno == ENOENT;
    }
    if (!S_ISSOCK(st.st_mode)) {
        fprintf(stderr, "matmul_daemon: %s exists and is not a socket\n", path);
        return false;
    }
    if (!fill_sockaddr(&sa, path) || (fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
        return false;
    }
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0) {
        close(fd);
        fprintf(stderr, "matmul_daemon: a daemon is already listening on %s\n", path);
        return false;
    }
    close(fd);
    if (errno != ECONNREFUSED) {
        fprintf(stderr, "matmul_daemon: cannot probe %s: %s\n", path, strerror(errno));
        return false;
    }
    if (unlink(path) != 0) {
        fprintf(stderr, "matmul_daemon: cannot remove stale %s: %s\n", path,
                strerror(errno));
        return false;
    }
    return true;
}

static int open_listener(const char *path) {
    struct sockaddr_un sa;
    struct stat st;
    mode_t old_umask;
    int fd;

    if (!fill_sockaddr(&sa, path) || !clear_stale_socket(path)) {
        return -1;
    }
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("matmul_daemon: socket");
        return -1;
    }
    old_umask = umask(077); /* only this user may connect */
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        umask(old_umask);
        fprintf(stderr, "matmul_daemon: bind %s: %s\n", path, strerror(errno));
        close(fd);
        return -1;
    }
    umask(old_umask);
    if (listen(fd, 16) != 0 || stat(path, &st) != 0) {
        perror("matmul_daemon: listen");
        close(fd);
        unlink(path);
        return -1;
    }
    g_socket_dev = st.st_dev;
    g_socket_ino = st.st_ino;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fd;
}

/* Only remove the socket if it is still the one this process created. */
static void remove_own_socket(void) {
    struct stat st;

    if (stat(g_socket_path, &st) == 0 && st.st_dev == g_socket_dev &&
        st.st_ino == g_socket_ino) {
        unlink(g_socket_path);
    }
}

static bool write_pidfile(const char *path) {
    char buf[32];
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    int len;
    bool ok;

    if (fd < 0) {
        fprintf(stderr, "matmul_daemon: pidfile %s: %s\n", path, strerror(errno));
        return false;
    }
    len = snprintf(buf, sizeof(buf), "%ld\n", (long)getpid());
    ok = write_full(fd, buf, (size_t)len);
    close(fd);
    return ok;
}

static void remove_own_pidfile(void) {
    char buf[32] = {0};
    int fd = open(g_pid_path, O_RDONLY);

    if (fd < 0) {
        return;
    }
    (void)!read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (strtol(buf, NULL, 10) == (long)getpid()) {
        unlink(g_pid_path);
    }
}

/*
 * Double fork. The original process waits on `ready_fd` and exits 0 only if
 * the grandchild reports that it is listening. Returns in the grandchild.
 */
static int daemonize(void) {
    int ready[2];
    pid_t pid;
    char status = 0;

    if (pipe(ready) != 0) {
        perror("matmul_daemon: pipe");
        exit(1);
    }
    pid = fork();
    if (pid < 0) {
        perror("matmul_daemon: fork");
        exit(1);
    }
    if (pid > 0) {
        ssize_t r;

        close(ready[1]);
        do {
            r = read(ready[0], &status, 1);
        } while (r < 0 && errno == EINTR);
        exit(r == 1 && status == 'R' ? 0 : 1);
    }
    close(ready[0]);
    if (setsid() < 0) {
        exit(1);
    }
    pid = fork();
    if (pid < 0) {
        exit(1);
    }
    if (pid > 0) {
        _exit(0);
    }
    return ready[1];
}

static int redirect_stdio(const char *log_path) {
    int null_fd = open("/dev/null", O_RDONLY);
    int log_fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);

    if (null_fd < 0 || log_fd < 0) {
        fprintf(stderr, "matmul_daemon: log %s: %s\n", log_path, strerror(errno));
        return -1;
    }
    dup2(null_fd, STDIN_FILENO);
    dup2(log_fd, STDOUT_FILENO);
    dup2(log_fd, STDERR_FILENO);
    close(null_fd);
    close(log_fd);
    return 0;
}

static int ping(const char *path) {
    struct sockaddr_un sa;
    MmdHeader h;
    MmdHello hello = {.client_pid = mmd_le32((uint32_t)getpid())};
    MmdHelloAck ack;
    struct timeval tv = {.tv_sec = 2};
    int fd;

    if (!fill_sockaddr(&sa, path) || (fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
        return 1;
    }
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        fprintf(stderr, "matmul_daemon: no daemon on %s: %s\n", path, strerror(errno));
        close(fd);
        return 1;
    }
    if (!send_msg(fd, MMD_MSG_HELLO, 1, &hello, sizeof(hello), NULL, 0) ||
        read_full(fd, &h, sizeof(h)) != 1 || mmd_le32(h.magic) != MMD_MAGIC ||
        mmd_le16(h.type) != MMD_MSG_HELLO_ACK || mmd_le32(h.len) != sizeof(ack) ||
        read_full(fd, &ack, sizeof(ack)) != 1) {
        fprintf(stderr, "matmul_daemon: bad reply from %s\n", path);
        close(fd);
        return 1;
    }
    close(fd);
    printf("matmul daemon pid %" PRIu32 " on %s: max_n %" PRIu32 ", %" PRIu64
           " GEMMs served\n",
           mmd_le32(ack.daemon_pid), path, mmd_le32(ack.max_n),
           mmd_le64(ack.requests_served));
    return 0;
}

static void usage(FILE *out) {
    fprintf(out,
            "usage: matmul_daemon [--socket PATH] [--daemon [--pidfile PATH] "
            "[--log PATH]]\n"
            "       matmul_daemon [--socket PATH] --ping\n"
            "\n"
            "  --socket PATH   Unix socket (default $%s or %s)\n"
            "  --daemon        detach from the terminal after the socket is ready\n"
            "  --pidfile PATH  daemon pidfile (default SOCKET.pid)\n"
            "  --log PATH      daemon log (default SOCKET.log)\n"
            "  --ping          query a running daemon and exit\n",
            MMD_SOCKET_ENV, MMD_DEFAULT_SOCKET);
}

int main(int argc, char **argv) {
    const char *sock = getenv(MMD_SOCKET_ENV);
    const char *pidfile = NULL, *logfile = NULL;
    bool detach = false, do_ping = false;
    struct sigaction sa;
    int listen_fd, ready_fd = -1;

    for (int i = 1; i < argc; i++) {
        bool has_val = i + 1 < argc;

        if (!strcmp(argv[i], "--socket") && has_val) {
            sock = argv[++i];
        } else if (!strcmp(argv[i], "--pidfile") && has_val) {
            pidfile = argv[++i];
        } else if (!strcmp(argv[i], "--log") && has_val) {
            logfile = argv[++i];
        } else if (!strcmp(argv[i], "--daemon")) {
            detach = true;
        } else if (!strcmp(argv[i], "--ping")) {
            do_ping = true;
        } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage(stdout);
            return 0;
        } else {
            usage(stderr);
            return 2;
        }
    }
    if (!sock || !*sock) {
        sock = MMD_DEFAULT_SOCKET;
    }
    if (!make_absolute(g_socket_path, sock)) {
        fprintf(stderr, "matmul_daemon: bad socket path\n");
        return 2;
    }
    if (do_ping) {
        return ping(g_socket_path);
    }
    if (detach) {
        bool ok = pidfile ? make_absolute(g_pid_path, pidfile)
                          : snprintf(g_pid_path, PATH_MAX, "%s.pid", g_socket_path) <
                                PATH_MAX;
        ok = ok && (logfile ? make_absolute(g_log_path, logfile)
                            : snprintf(g_log_path, PATH_MAX, "%s.log", g_socket_path) <
                                  PATH_MAX);
        if (!ok) {
            fprintf(stderr, "matmul_daemon: pidfile/log path too long\n");
            return 2;
        }
    }

    signal(SIGPIPE, SIG_IGN);
    if (pipe(g_wake_pipe) != 0) {
        perror("matmul_daemon: pipe");
        return 1;
    }
    fcntl(g_wake_pipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(g_wake_pipe[1], F_SETFD, FD_CLOEXEC);
    fcntl(g_wake_pipe[1], F_SETFL, O_NONBLOCK);
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    if (detach) {
        ready_fd = daemonize();
    }
    /* Errors before this point still reach the launching terminal. */
    listen_fd = open_listener(g_socket_path);
    if (listen_fd < 0) {
        return 1;
    }
    if (detach) {
        if (redirect_stdio(g_log_path) != 0 || !write_pidfile(g_pid_path)) {
            remove_own_socket();
            return 1;
        }
        if (chdir("/") != 0) {
            /* Paths are absolute already; staying put is harmless. */
        }
    }
    log_msg("pid %ld listening on %s (max_n %u)%s", (long)getpid(), g_socket_path,
            MMD_MAX_N, detach ? " [daemon]" : "");
    if (ready_fd >= 0) {
        (void)!write(ready_fd, "R", 1);
        close(ready_fd);
    }

    while (!g_stop) {
        struct pollfd fds[2] = {
            {.fd = listen_fd, .events = POLLIN},
            {.fd = g_wake_pipe[0], .events = POLLIN},
        };
        pthread_attr_t attr;
        pthread_t tid;
        Client *cl;
        int fd;

        if (poll(fds, 2, -1) < 0) {
            if (errno == EINTR) {
                continue;
            }
            log_msg("poll: %s", strerror(errno));
            break;
        }
        if (fds[1].revents || !(fds[0].revents & POLLIN)) {
            continue;
        }
        fd = accept(listen_fd, NULL, NULL);
        if (fd < 0) {
            if (errno != EINTR && errno != ECONNABORTED) {
                log_msg("accept: %s", strerror(errno));
            }
            continue;
        }
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        cl = malloc(sizeof(*cl));
        if (!cl) {
            close(fd);
            continue;
        }
        pthread_mutex_lock(&g_lock);
        cl->id = ++g_next_client;
        pthread_mutex_unlock(&g_lock);
        cl->fd = fd;
        log_msg("client %u: connected", cl->id);
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&tid, &attr, client_thread, cl) != 0) {
            log_msg("client %u: cannot start thread", cl->id);
            close(fd);
            free(cl);
        }
        pthread_attr_destroy(&attr);
    }

    close(listen_fd);
    remove_own_socket();
    if (detach) {
        remove_own_pidfile();
    }
    pthread_mutex_lock(&g_lock);
    log_msg("stopping; served %" PRIu64 " GEMMs", g_served);
    pthread_mutex_unlock(&g_lock);
    return 0;
}
