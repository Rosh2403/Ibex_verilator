// SPDX-License-Identifier: Apache-2.0
// GPIO + UART + timer RTL shell for QEMU TCG MMIO cosim.

module cosim_periph_top (
    input  logic        clk_i,
    input  logic        rst_ni,

    input  logic        host_req_i,
    input  logic [31:0] host_addr_i,
    input  logic        host_we_i,
    input  logic [3:0]  host_be_i,
    input  logic [31:0] host_wdata_i,
    output logic        host_rvalid_o,
    output logic [31:0] host_rdata_o,

    input  logic [7:0]  gp_i,
    output logic [15:0] gp_o,
    input  logic        uart_rx_i,
    output logic        uart_tx_o,
    output logic        uart_irq_o,
    output logic        timer_irq_o
);

  logic gpio_req, uart_req, timer_req;
  logic gpio_rvalid, uart_rvalid, timer_rvalid;
  logic [31:0] gpio_rdata, uart_rdata, timer_rdata;
  logic timer_err;

  assign gpio_req  = host_req_i && (host_addr_i[31:12] == 20'h80000);
  assign uart_req  = host_req_i && (host_addr_i[31:12] == 20'h80001);
  assign timer_req = host_req_i && (host_addr_i[31:12] == 20'h80002);

  gpio #(
    .GpiWidth(8),
    .GpoWidth(16)
  ) u_gpio (
    .clk_i,
    .rst_ni,
    .device_req_i   (gpio_req),
    .device_addr_i  (host_addr_i),
    .device_we_i    (host_we_i),
    .device_be_i    (host_be_i),
    .device_wdata_i (host_wdata_i),
    .device_rvalid_o(gpio_rvalid),
    .device_rdata_o (gpio_rdata),
    .gp_i,
    .gp_o
  );

  uart #(
    .ClockFrequency(50_000_000),
    .BaudRate      (115_200)
  ) u_uart (
    .clk_i,
    .rst_ni,
    .device_req_i   (uart_req),
    .device_addr_i  (host_addr_i),
    .device_we_i    (host_we_i),
    .device_be_i    (host_be_i),
    .device_wdata_i (host_wdata_i),
    .device_rvalid_o(uart_rvalid),
    .device_rdata_o (uart_rdata),
    .uart_rx_i,
    .uart_irq_o,
    .uart_tx_o
  );

  timer #(
    .DataWidth   (32),
    .AddressWidth(32)
  ) u_timer (
    .clk_i,
    .rst_ni,
    .timer_req_i   (timer_req),
    .timer_addr_i  (host_addr_i),
    .timer_we_i    (host_we_i),
    .timer_be_i    (host_be_i),
    .timer_wdata_i (host_wdata_i),
    .timer_rvalid_o(timer_rvalid),
    .timer_rdata_o (timer_rdata),
    .timer_err_o   (timer_err),
    .timer_intr_o  (timer_irq_o)
  );

  assign host_rvalid_o = gpio_rvalid | uart_rvalid | timer_rvalid;
  assign host_rdata_o  = gpio_rvalid  ? gpio_rdata  :
                         uart_rvalid  ? uart_rdata  :
                         timer_rvalid ? timer_rdata : 32'b0;

endmodule
