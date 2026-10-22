#include "qemu/osdep.h"
#include "qapi/error.h"
#include "chardev/char-fe.h"
#include "hw/boards.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "qemu/sockets.h"
#include "qemu/timer.h"
#include "system/runstate.h"
#include "system/system.h"
#include "cosim_soc_protocol.h"

#define TYPE_IBEXDEMO_SOC_COSIM MACHINE_TYPE_NAME("ibexdemo-soc-cosim")
OBJECT_DECLARE_SIMPLE_TYPE(IbexSocCosim, IBEXDEMO_SOC_COSIM)

struct IbexSocCosim {
    MachineState parent_obj;
    char *socket_path;
    int socket_fd;
    CharFrontend console;
    QEMUTimer *timer;
    Notifier exit_notifier;
    uint8_t request[sizeof(CosimSocReq)];
    size_t written;
    uint8_t response[sizeof(CosimSocResp) + COSIM_SOC_MAX_UART_CHUNK];
    size_t received;
    size_t expected;
    GByteArray *input;
    bool pending;
    bool ready;
    bool step_due;
    bool failed;
    bool ending;
    int64_t deadline;
};

static void cosim_read(void *opaque);
static void cosim_write(void *opaque);

static void cosim_watch(IbexSocCosim *state)
{
    qemu_set_fd_handler(state->socket_fd, cosim_read,
                        state->pending && state->written < sizeof(CosimSocReq) ?
                            cosim_write : NULL, state);
}

static void cosim_fail(IbexSocCosim *state, const char *message)
{
    if (state->failed || state->ending) {
        return;
    }
    state->failed = true;
    error_report("full-SoC cosim: %s", message);
    qemu_set_fd_handler(state->socket_fd, NULL, NULL, NULL);
    timer_del(state->timer);
    qemu_system_shutdown_request_with_code(SHUTDOWN_CAUSE_HOST_ERROR, 1);
}

static void cosim_request(IbexSocCosim *state, uint32_t command, uint64_t parameter)
{
    CosimSocReq request = {
        .magic = cosim_soc_le32(COSIM_SOC_REQ_MAGIC),
        .cmd = cosim_soc_le32(command),
        .param = cosim_soc_le64(parameter),
    };
    memcpy(state->request, &request, sizeof(request));
    state->written = 0;
    state->received = 0;
    state->expected = sizeof(CosimSocResp);
    state->pending = true;
    state->deadline = qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 10000;
    cosim_watch(state);
}

static void cosim_write(void *opaque)
{
    IbexSocCosim *state = opaque;
    ssize_t count = send(state->socket_fd, state->request + state->written,
                         sizeof(CosimSocReq) - state->written, 0);
    if (count < 0 && (errno == EAGAIN || errno == EINTR)) {
        return;
    }
    if (count <= 0) {
        cosim_fail(state, "socket write failed");
        return;
    }
    state->written += count;
    cosim_watch(state);
}

static void cosim_read(void *opaque)
{
    IbexSocCosim *state = opaque;
    CosimSocResp header;
    uint32_t length;
    ssize_t count;

    if (!state->pending) {
        cosim_fail(state, "unexpected socket data or disconnect");
        return;
    }
    count = recv(state->socket_fd, state->response + state->received,
                 state->expected - state->received, 0);
    if (count < 0 && (errno == EAGAIN || errno == EINTR)) {
        return;
    }
    if (count <= 0) {
        cosim_fail(state, "Verilator disconnected");
        return;
    }
    state->received += count;
    if (state->received < sizeof(header)) {
        return;
    }
    memcpy(&header, state->response, sizeof(header));
    length = cosim_soc_le32(header.uart_len);
    if (cosim_soc_le32(header.magic) != COSIM_SOC_RESP_MAGIC ||
        length > COSIM_SOC_MAX_UART_CHUNK ||
        (header.flags & ~(COSIM_SOC_FLAG_STOPPED | COSIM_SOC_FLAG_ERROR))) {
        cosim_fail(state, "invalid response (check protocol/build versions)");
        return;
    }
    state->expected = sizeof(header) + length;
    if (state->received < state->expected) {
        return;
    }
    if (length) {
        qemu_chr_fe_write_all(&state->console,
                              state->response + sizeof(header), length);
    }
    state->pending = false;
    state->ready = true;
    cosim_watch(state);
    if (header.flags & COSIM_SOC_FLAG_ERROR) {
        cosim_fail(state, "Verilator rejected request");
    } else if (header.flags & COSIM_SOC_FLAG_STOPPED) {
        state->ending = true;
        timer_del(state->timer);
        qemu_set_fd_handler(state->socket_fd, NULL, NULL, NULL);
        qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
    }
}

static void cosim_tick(void *opaque)
{
    IbexSocCosim *state = opaque;
    int64_t now = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    if (state->failed || state->ending) {
        return;
    }
    if (state->pending && now >= state->deadline) {
        cosim_fail(state, "response timed out");
        return;
    }
    if (!state->pending && runstate_is_running()) {
        if (!state->ready) {
            cosim_request(state, COSIM_SOC_CMD_PING, COSIM_SOC_VERSION);
        } else if (state->input->len && !state->step_due) {
            uint8_t byte = state->input->data[0];
            g_byte_array_remove_range(state->input, 0, 1);
            cosim_request(state, COSIM_SOC_CMD_UART_RX, byte);
            state->step_due = true;
            qemu_chr_fe_accept_input(&state->console);
        } else {
            cosim_request(state, COSIM_SOC_CMD_STEP, 100000);
            state->step_due = false;
        }
    }
    timer_mod(state->timer, now + 1);
}

static int cosim_can_receive(void *opaque)
{
    IbexSocCosim *state = opaque;
    return state->failed || state->ending ? 0 : 4096 - state->input->len;
}

static void cosim_receive(void *opaque, const uint8_t *bytes, int length)
{
    IbexSocCosim *state = opaque;
    g_byte_array_append(state->input, bytes, length);
}

static void cosim_exit(Notifier *notifier, void *data)
{
    IbexSocCosim *state = container_of(notifier, IbexSocCosim, exit_notifier);
    CosimSocReq request = {
        .magic = cosim_soc_le32(COSIM_SOC_REQ_MAGIC),
        .cmd = cosim_soc_le32(COSIM_SOC_CMD_SHUTDOWN),
    };
    state->ending = true;
    timer_del(state->timer);
    qemu_set_fd_handler(state->socket_fd, NULL, NULL, NULL);
    if (!state->pending || state->written == sizeof(CosimSocReq)) {
        send(state->socket_fd, &request, sizeof(request), 0);
    }
    close(state->socket_fd);
    state->socket_fd = -1;
}

static void cosim_init(MachineState *machine)
{
    IbexSocCosim *state = IBEXDEMO_SOC_COSIM(machine);
    struct sockaddr_un address = { .sun_family = AF_UNIX };

    if (machine->kernel_filename || machine->cpu_type) {
        error_report("full-SoC cosim: load the ELF into Verilator; do not use -kernel or -cpu");
        exit(1);
    }
    if (strlen(state->socket_path) >= sizeof(address.sun_path)) {
        error_report("full-SoC cosim: socket path too long");
        exit(1);
    }
    memcpy(address.sun_path, state->socket_path, strlen(state->socket_path) + 1);
    state->socket_fd = qemu_socket(AF_UNIX, SOCK_STREAM, 0);
    if (state->socket_fd < 0 ||
        connect(state->socket_fd, (struct sockaddr *)&address, sizeof(address))) {
        error_report("full-SoC cosim: connect %s: %s", state->socket_path,
                      strerror(errno));
        exit(1);
    }
    if (fcntl(state->socket_fd, F_SETFL, O_NONBLOCK) < 0) {
        error_report("full-SoC cosim: cannot make socket nonblocking");
        exit(1);
    }
    state->input = g_byte_array_new();
    qemu_chr_fe_init(&state->console, serial_hd(0), &error_fatal);
    qemu_chr_fe_set_handlers(&state->console, cosim_can_receive, cosim_receive,
                             NULL, NULL, state, NULL, true);
    state->timer = timer_new_ms(QEMU_CLOCK_REALTIME, cosim_tick, state);
    state->exit_notifier.notify = cosim_exit;
    qemu_add_exit_notifier(&state->exit_notifier);
    timer_mod(state->timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME));
}

static char *cosim_get_socket(Object *object, Error **errp)
{
    return g_strdup(IBEXDEMO_SOC_COSIM(object)->socket_path);
}

static void cosim_set_socket(Object *object, const char *value, Error **errp)
{
    IbexSocCosim *state = IBEXDEMO_SOC_COSIM(object);
    g_free(state->socket_path);
    state->socket_path = g_strdup(value);
}

static void cosim_instance_init(Object *object)
{
    IbexSocCosim *state = IBEXDEMO_SOC_COSIM(object);
    state->socket_fd = -1;
    state->socket_path = g_strdup(COSIM_SOC_DEFAULT_SOCKET);
}

static void cosim_class_init(ObjectClass *object_class, const void *data)
{
    MachineClass *machine_class = MACHINE_CLASS(object_class);
    machine_class->desc = "Ibex Demo full-SoC RTL via Verilator (no QEMU CPU)";
    machine_class->init = cosim_init;
    machine_class->max_cpus = 1;
    machine_class->default_ram_size = 0;
    machine_class->no_parallel = true;
    machine_class->no_floppy = true;
    machine_class->no_cdrom = true;
    object_class_property_add_str(object_class, "socket", cosim_get_socket,
                                  cosim_set_socket);
}

static const TypeInfo cosim_type = {
    .name = TYPE_IBEXDEMO_SOC_COSIM,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(IbexSocCosim),
    .instance_init = cosim_instance_init,
    .class_init = cosim_class_init,
};

static void cosim_register_types(void)
{
    type_register_static(&cosim_type);
}

type_init(cosim_register_types)
