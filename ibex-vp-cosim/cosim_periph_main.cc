// SPDX-License-Identifier: Apache-2.0
// QEMU TCG <-> Verilator GPIO/UART/timer MMIO server.

#include <verilated.h>

#include <Vcosim_periph_top.h>

#include "cosim_protocol.h"

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

static volatile bool g_stop = false;

static void on_sigint(int) { g_stop = true; }

static const int kBaudDiv =
    static_cast<int>(COSIM_UART_CLOCK_HZ / COSIM_UART_BAUD);

static void tick(Vcosim_periph_top *top, VerilatedContext *ctx) {
    top->clk_i = 0;
    top->eval();
    ctx->timeInc(1);
    top->clk_i = 1;
    top->eval();
    ctx->timeInc(1);
}

class UartMonitor {
public:
    void sample(int pin) {
        switch (state_) {
        case Idle:
            if (last_ && !pin) {
                state_ = Start;
                count_ = 0;
            }
            break;
        case Start:
            if (++count_ >= kBaudDiv / 2) {
                if (!pin) {
                    state_ = Data;
                    count_ = 0;
                    bit_ = 0;
                    byte_ = 0;
                } else {
                    state_ = Idle;
                }
            }
            break;
        case Data:
            if (++count_ >= kBaudDiv) {
                count_ = 0;
                byte_ |= static_cast<uint8_t>(pin ? 1 : 0) << bit_;
                if (++bit_ >= 8) {
                    state_ = Stop;
                }
            }
            break;
        case Stop:
            if (++count_ >= kBaudDiv) {
                if (pin && tx_.size() < COSIM_PERIPH_MAX_UART) {
                    tx_.push_back(byte_);
                }
                state_ = Idle;
            }
            break;
        }
        last_ = pin;
    }

    void drain(std::vector<uint8_t> *out) {
        while (!tx_.empty() && out->size() < COSIM_PERIPH_MAX_UART) {
            out->push_back(tx_.front());
            tx_.pop_front();
        }
    }

private:
    enum { Idle, Start, Data, Stop } state_ = Idle;
    int last_ = 1;
    int count_ = 0;
    int bit_ = 0;
    uint8_t byte_ = 0;
    std::deque<uint8_t> tx_;
};

static void advance(Vcosim_periph_top *top, VerilatedContext *ctx,
                    UartMonitor *uart, uint32_t cycles) {
    for (uint32_t i = 0; i < cycles && !g_stop; ++i) {
        tick(top, ctx);
        uart->sample(top->uart_tx_o);
    }
}

static uint32_t do_mmio(Vcosim_periph_top *top, VerilatedContext *ctx,
                        UartMonitor *uart, uint32_t addr, bool we, uint8_t be,
                        uint32_t wdata) {
    top->host_req_i = 0;
    top->host_we_i = we;
    top->host_be_i = be;
    top->host_addr_i = addr;
    top->host_wdata_i = wdata;
    advance(top, ctx, uart, 1);

    top->host_req_i = 1;
    advance(top, ctx, uart, 1);
    if (!we) {
        advance(top, ctx, uart, 1);
    }
    const uint32_t rdata = top->host_rdata_o;
    top->host_req_i = 0;
    advance(top, ctx, uart, 1);
    advance(top, ctx, uart, static_cast<uint32_t>(kBaudDiv + 1));
    return rdata;
}

static bool addr_ok(uint32_t addr) {
    return addr >= COSIM_PERIPH_BASE &&
           addr < COSIM_PERIPH_BASE + COSIM_PERIPH_SIZE;
}

static bool send_all(int fd, const void *buf, size_t len) {
    const char *p = static_cast<const char *>(buf);
    while (len) {
        ssize_t n = send(fd, p, len, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        p += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

static bool recv_all(int fd, void *buf, size_t len) {
    char *p = static_cast<char *>(buf);
    while (len) {
        ssize_t n = recv(fd, p, len, MSG_WAITALL);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) {
                continue;
            }
            return false;
        }
        p += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

static void bitbang_rx(Vcosim_periph_top *top, VerilatedContext *ctx,
                       UartMonitor *uart, uint8_t byte) {
    auto bits = [&](int pin, int clocks) {
        top->uart_rx_i = pin;
        advance(top, ctx, uart, static_cast<uint32_t>(clocks));
    };
    bits(0, kBaudDiv);
    for (int i = 0; i < 8; ++i) {
        bits((byte >> i) & 1, kBaudDiv);
    }
    bits(1, kBaudDiv);
}

static bool reply(int fd, uint32_t rdata, Vcosim_periph_top *top,
                  UartMonitor *uart) {
    std::vector<uint8_t> bytes;
    uart->drain(&bytes);
    CosimPeriphResp resp{};
    resp.magic = COSIM_RESP_MAGIC;
    resp.rdata = rdata;
    if (top->timer_irq_o) {
        resp.irq |= COSIM_IRQ_TIMER;
    }
    if (top->uart_irq_o) {
        resp.irq |= COSIM_IRQ_UART;
    }
    resp.uart_len = static_cast<uint32_t>(bytes.size());
    if (!send_all(fd, &resp, sizeof(resp))) {
        return false;
    }
    return bytes.empty() || send_all(fd, bytes.data(), bytes.size());
}

static int handle_client(int fd, Vcosim_periph_top *top, VerilatedContext *ctx,
                         UartMonitor *uart) {
    while (!g_stop) {
        CosimMmioReq req{};
        if (!recv_all(fd, &req, sizeof(req))) {
            return 0;
        }
        if (req.magic != COSIM_REQ_MAGIC) {
            fprintf(stderr, "periph-cosim: bad req magic 0x%08x\n", req.magic);
            return -1;
        }

        uint32_t rdata = 0;
        if (req.cmd == COSIM_CMD_IDLE) {
            if (req.wdata == 0 || req.wdata > COSIM_PERIPH_MAX_IDLE) {
                fprintf(stderr, "periph-cosim: bad idle count %u\n", req.wdata);
                return -1;
            }
            advance(top, ctx, uart, req.wdata);
        } else if (req.cmd == COSIM_CMD_UART_RX) {
            bitbang_rx(top, ctx, uart, static_cast<uint8_t>(req.wdata));
        } else if (req.cmd == COSIM_CMD_MMIO) {
            if (!addr_ok(req.addr)) {
                fprintf(stderr, "periph-cosim: addr 0x%08x out of range\n",
                        req.addr);
                return -1;
            }
            rdata = do_mmio(top, ctx, uart, req.addr, req.we != 0, req.be,
                            req.wdata);
        } else {
            fprintf(stderr, "periph-cosim: bad cmd %u\n", req.cmd);
            return -1;
        }

        if (!reply(fd, rdata, top, uart)) {
            return -1;
        }
    }
    return 0;
}

static int run_server(const char *sock_path, Vcosim_periph_top *top,
                      VerilatedContext *ctx, UartMonitor *uart) {
    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    if (strlen(sock_path) >= sizeof(addr.sun_path)) {
        fprintf(stderr, "periph-cosim: socket path too long\n");
        close(srv);
        return 1;
    }
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);
    unlink(sock_path);

    if (bind(srv, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) <
        0) {
        perror("bind");
        close(srv);
        return 1;
    }
    if (listen(srv, 1) < 0) {
        perror("listen");
        close(srv);
        return 1;
    }

    printf("periph-cosim: listening on unix:%s\n", sock_path);
    fflush(stdout);

    while (!g_stop) {
        int cli = accept(srv, nullptr, nullptr);
        if (cli < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("accept");
            break;
        }
        handle_client(cli, top, ctx, uart);
        close(cli);
    }

    close(srv);
    unlink(sock_path);
    return 0;
}

int main(int argc, char **argv) {
    const char *sock_path = "/tmp/ibexdemo-cosim.sock";
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
            sock_path = argv[++i];
        }
    }

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    VerilatedContext *const ctx = new VerilatedContext;
    ctx->commandArgs(argc, argv);

    Vcosim_periph_top *const top = new Vcosim_periph_top{ctx};
    UartMonitor uart;
    top->rst_ni = 0;
    top->gp_i = 0;
    top->uart_rx_i = 1;
    top->host_req_i = 0;
    for (int i = 0; i < 8; ++i) {
        advance(top, ctx, &uart, 1);
    }
    top->rst_ni = 1;
    for (int i = 0; i < 8; ++i) {
        advance(top, ctx, &uart, 1);
    }

    const int rc = run_server(sock_path, top, ctx, &uart);

    top->final();
    delete top;
    delete ctx;
    return rc;
}
