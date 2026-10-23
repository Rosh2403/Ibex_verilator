/*
 * QEMU TCG CPU: forward GPIO/UART/timer MMIO to Verilator RTL.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "chardev/char-fe.h"
#include "hw/core/cpu.h"
#include "hw/ibexdemo/ibexdemo_periph_bridge.h"
#include "hw/ibexdemo/cosim_protocol.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "hw/sysbus.h"
#include "system/runstate.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static bool periph_connect(IbexDemoPeriphBridgeState *s, Error **errp)
{
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    int fd;

    if (s->sock_fd >= 0) {
        return true;
    }
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        error_setg_errno(errp, errno, "periph-cosim: socket()");
        return false;
    }
    if (g_strlcpy(addr.sun_path, s->socket_path, sizeof(addr.sun_path))
        >= sizeof(addr.sun_path)) {
        error_setg(errp, "periph-cosim: socket path too long");
        close(fd);
        return false;
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        error_setg_errno(errp, errno, "periph-cosim: connect(%s)",
                         s->socket_path);
        close(fd);
        return false;
    }
    s->sock_fd = fd;
    return true;
}

static void periph_disconnect(IbexDemoPeriphBridgeState *s)
{
    if (s->sock_fd >= 0) {
        close(s->sock_fd);
        s->sock_fd = -1;
    }
}

static bool periph_xfer(IbexDemoPeriphBridgeState *s, uint8_t cmd,
                        uint32_t addr, bool we, uint8_t be, uint32_t wdata,
                        uint32_t *rdata, Error **errp)
{
    struct CosimMmioReq req = {
        .magic = COSIM_REQ_MAGIC,
        .addr = addr,
        .we = we ? 1 : 0,
        .be = be,
        .cmd = cmd,
        .wdata = wdata,
    };
    struct CosimPeriphResp resp;
    uint8_t uart[COSIM_PERIPH_MAX_UART];
    ssize_t n;

    if (!periph_connect(s, errp)) {
        return false;
    }
    if (send(s->sock_fd, &req, sizeof(req), 0) != (ssize_t)sizeof(req)) {
        error_setg_errno(errp, errno, "periph-cosim: send");
        periph_disconnect(s);
        return false;
    }
    n = recv(s->sock_fd, &resp, sizeof(resp), MSG_WAITALL);
    if (n != (ssize_t)sizeof(resp) || resp.magic != COSIM_RESP_MAGIC ||
        resp.uart_len > COSIM_PERIPH_MAX_UART) {
        error_setg(errp, "periph-cosim: bad response");
        periph_disconnect(s);
        return false;
    }
    if (resp.uart_len) {
        n = recv(s->sock_fd, uart, resp.uart_len, MSG_WAITALL);
        if (n != (ssize_t)resp.uart_len) {
            error_setg_errno(errp, errno, "periph-cosim: uart recv");
            periph_disconnect(s);
            return false;
        }
        qemu_chr_fe_write_all(&s->console, uart, resp.uart_len);
    }
    qemu_set_irq(s->irq_timer, !!(resp.irq & COSIM_IRQ_TIMER));
    qemu_set_irq(s->irq_uart, !!(resp.irq & COSIM_IRQ_UART));
    if (rdata) {
        *rdata = resp.rdata;
    }
    return true;
}

static uint32_t periph_mmio(IbexDemoPeriphBridgeState *s, hwaddr offset,
                            bool is_write, uint32_t val, unsigned size,
                            Error **errp)
{
    uint8_t be;

    switch (size) {
    case 1:
        be = (1u << (offset & 3));
        break;
    case 2:
        be = (3u << (offset & 2));
        break;
    case 4:
        be = 0xFu;
        break;
    default:
        error_setg(errp, "periph-cosim: unsupported access size %u", size);
        return 0;
    }
    uint32_t rdata = 0;
    if (!periph_xfer(s, COSIM_CMD_MMIO, COSIM_PERIPH_BASE + (uint32_t)offset,
                     is_write, be, val, &rdata, errp)) {
        return 0;
    }
    return rdata;
}

static uint64_t periph_read(void *opaque, hwaddr offset, unsigned size)
{
    IbexDemoPeriphBridgeState *s = opaque;
    Error *err = NULL;
    uint32_t val = periph_mmio(s, offset, false, 0, size, &err);
    if (err) {
        error_report_err(err);
    }
    return val;
}

static void periph_write(void *opaque, hwaddr offset, uint64_t val,
                         unsigned size)
{
    IbexDemoPeriphBridgeState *s = opaque;
    Error *err = NULL;
    periph_mmio(s, offset, true, (uint32_t)val, size, &err);
    if (err) {
        error_report_err(err);
    }
}

static const MemoryRegionOps periph_ops = {
    .read = periph_read,
    .write = periph_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void periph_idle(void *opaque)
{
    IbexDemoPeriphBridgeState *s = opaque;
    Error *err = NULL;
    int64_t now = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);

    if (runstate_is_running() && s->sock_fd >= 0) {
        CPUState *cpu = first_cpu;
        if (s->input->len) {
            uint8_t byte = s->input->data[0];
            g_byte_array_remove_range(s->input, 0, 1);
            periph_xfer(s, COSIM_CMD_UART_RX, 0, false, 0, byte, NULL, &err);
            qemu_chr_fe_accept_input(&s->console);
        } else if (cpu && cpu->halted) {
            periph_xfer(s, COSIM_CMD_IDLE, 0, false, 0, s->idle_cycles, NULL,
                        &err);
        }
        if (err) {
            error_report_err(err);
        }
    }
    timer_mod(s->idle, now + 1);
}

static int periph_can_receive(void *opaque)
{
    IbexDemoPeriphBridgeState *s = opaque;
    return 4096 - (int)s->input->len;
}

static void periph_receive(void *opaque, const uint8_t *bytes, int length)
{
    IbexDemoPeriphBridgeState *s = opaque;
    g_byte_array_append(s->input, bytes, length);
}

static void periph_init(Object *obj)
{
    IbexDemoPeriphBridgeState *s = IBEXDEMO_PERIPH_BRIDGE(obj);

    memory_region_init_io(&s->mmio, obj, &periph_ops, s,
                          TYPE_IBEXDEMO_PERIPH_BRIDGE, COSIM_PERIPH_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq_timer);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq_uart);
    s->sock_fd = -1;
    s->socket_path = g_strdup("/tmp/ibexdemo-cosim.sock");
}

static void periph_realize(DeviceState *dev, Error **errp)
{
    IbexDemoPeriphBridgeState *s = IBEXDEMO_PERIPH_BRIDGE(dev);

    if (!s->socket_path || !s->socket_path[0]) {
        error_setg(errp, "periph-cosim: socket-path property required");
        return;
    }
    if (!s->idle_cycles || s->idle_cycles > COSIM_PERIPH_MAX_IDLE) {
        error_setg(errp, "periph-cosim: idle-cycles out of range");
        return;
    }
    s->input = g_byte_array_new();
    qemu_chr_fe_set_handlers(&s->console, periph_can_receive, periph_receive,
                             NULL, NULL, s, NULL, true);
    if (!periph_connect(s, errp)) {
        return;
    }
    s->idle = timer_new_ms(QEMU_CLOCK_REALTIME, periph_idle, s);
    timer_mod(s->idle, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 1);
}

static void periph_unrealize(DeviceState *dev)
{
    IbexDemoPeriphBridgeState *s = IBEXDEMO_PERIPH_BRIDGE(dev);

    if (s->idle) {
        timer_free(s->idle);
        s->idle = NULL;
    }
    periph_disconnect(s);
    if (s->input) {
        g_byte_array_free(s->input, TRUE);
        s->input = NULL;
    }
}

static const Property periph_properties[] = {
    DEFINE_PROP_STRING("socket-path", IbexDemoPeriphBridgeState, socket_path),
    DEFINE_PROP_UINT32("idle-cycles", IbexDemoPeriphBridgeState, idle_cycles,
                       50000u),
    DEFINE_PROP_CHR("chardev", IbexDemoPeriphBridgeState, console),
};

static void periph_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = periph_realize;
    dc->unrealize = periph_unrealize;
    device_class_set_props(dc, periph_properties);
}

static const TypeInfo periph_info = {
    .name = TYPE_IBEXDEMO_PERIPH_BRIDGE,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IbexDemoPeriphBridgeState),
    .instance_init = periph_init,
    .class_init = periph_class_init,
};

static void periph_register_types(void)
{
    type_register_static(&periph_info);
}

type_init(periph_register_types);
