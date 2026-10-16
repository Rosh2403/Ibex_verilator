// SPDX-License-Identifier: Apache-2.0

#include <verilated.h>

#include <Vcosim_gpio_top.h>

#include "cosim_protocol.h"

#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static volatile bool g_stop = false;

static void on_sigint(int) { g_stop = true; }

static void tick(Vcosim_gpio_top* top, VerilatedContext* ctx) {
    top->clk_i = 0;
    top->eval();
    ctx->timeInc(1);
    top->clk_i = 1;
    top->eval();
    ctx->timeInc(1);
}

static uint32_t do_mmio(Vcosim_gpio_top* top, VerilatedContext* ctx, uint32_t addr,
                        bool we, uint8_t be, uint32_t wdata) {
    top->host_req_i = 0;
    top->host_we_i = we;
    top->host_be_i = be;
    top->host_addr_i = addr;
    top->host_wdata_i = wdata;
    tick(top, ctx);

    top->host_req_i = 1;
    tick(top, ctx);

    // GPIO reads use a registered enable; hold req for another cycle.
    if (!we) tick(top, ctx);

    top->host_req_i = 0;
    tick(top, ctx);

    return top->host_rdata_o;
}

static bool addr_ok(uint32_t addr) {
    return addr >= COSIM_GPIO_BASE && addr < COSIM_GPIO_BASE + COSIM_GPIO_SIZE;
}

static int handle_client(int fd, Vcosim_gpio_top* top, VerilatedContext* ctx) {
    while (!g_stop) {
        CosimMmioReq req{};
        ssize_t n = recv(fd, &req, sizeof(req), MSG_WAITALL);
        if (n == 0) return 0;
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("recv");
            return -1;
        }
        if (n != (ssize_t)sizeof(req)) {
            fprintf(stderr, "cosim: short request (%zd)\n", n);
            return -1;
        }
        if (req.magic != COSIM_REQ_MAGIC) {
            fprintf(stderr, "cosim: bad req magic 0x%08x\n", req.magic);
            return -1;
        }
        if (!addr_ok(req.addr)) {
            fprintf(stderr, "cosim: addr 0x%08x out of range\n", req.addr);
            return -1;
        }

        CosimMmioResp resp{};
        resp.magic = COSIM_RESP_MAGIC;
        resp.rdata = do_mmio(top, ctx, req.addr, req.we != 0, req.be, req.wdata);

        if (send(fd, &resp, sizeof(resp), 0) != (ssize_t)sizeof(resp)) {
            perror("send");
            return -1;
        }
    }
    return 0;
}

static int run_server(const char* sock_path, Vcosim_gpio_top* top, VerilatedContext* ctx) {
    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    if (strlen(sock_path) >= sizeof(addr.sun_path)) {
        fprintf(stderr, "cosim: socket path too long\n");
        close(srv);
        return 1;
    }
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);
    unlink(sock_path);

    if (bind(srv, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("bind");
        close(srv);
        return 1;
    }
    if (listen(srv, 1) < 0) {
        perror("listen");
        close(srv);
        return 1;
    }

    printf("cosim: listening on unix:%s\n", sock_path);

    while (!g_stop) {
        int cli = accept(srv, nullptr, nullptr);
        if (cli < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }
        handle_client(cli, top, ctx);
        close(cli);
    }

    close(srv);
    unlink(sock_path);
    return 0;
}

int main(int argc, char** argv) {
    const char* sock_path = "/tmp/ibexdemo-cosim.sock";
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
            sock_path = argv[++i];
        }
    }

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    VerilatedContext* const ctx = new VerilatedContext;
    ctx->commandArgs(argc, argv);

    Vcosim_gpio_top* const top = new Vcosim_gpio_top{ctx};
    top->rst_ni = 0;
    top->gp_i = 0;
    for (int i = 0; i < 4; ++i) tick(top, ctx);
    top->rst_ni = 1;
    for (int i = 0; i < 4; ++i) tick(top, ctx);

    const int rc = run_server(sock_path, top, ctx);

    top->final();
    delete top;
    delete ctx;
    return rc;
}
