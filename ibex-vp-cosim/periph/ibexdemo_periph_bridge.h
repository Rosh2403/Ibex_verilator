/*
 * QEMU Ibex Demo TCG <-> Verilator GPIO/UART/timer bridge
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_IBEXDEMO_IBEXDEMO_PERIPH_BRIDGE_H
#define HW_IBEXDEMO_IBEXDEMO_PERIPH_BRIDGE_H

#include "chardev/char-fe.h"
#include "hw/irq.h"
#include "hw/sysbus.h"
#include "qom/object.h"

#define TYPE_IBEXDEMO_PERIPH_BRIDGE "ibexdemo-periph-bridge"

OBJECT_DECLARE_SIMPLE_TYPE(IbexDemoPeriphBridgeState, IBEXDEMO_PERIPH_BRIDGE)

struct IbexDemoPeriphBridgeState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    CharFrontend console;
    struct QEMUTimer *idle;
    qemu_irq irq_timer;
    qemu_irq irq_uart;
    GByteArray *input;
    char *socket_path;
    uint32_t idle_cycles;
    int sock_fd;
};

#endif
