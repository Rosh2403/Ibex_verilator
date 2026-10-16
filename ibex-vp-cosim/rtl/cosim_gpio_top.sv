// SPDX-License-Identifier: Apache-2.0

module cosim_gpio_top (
    input  logic clk_i,
    input  logic rst_ni,

    input  logic                 host_req_i,
    input  logic [31:0]          host_addr_i,
    input  logic                 host_we_i,
    input  logic [3:0]           host_be_i,
    input  logic [31:0]          host_wdata_i,
    output logic                 host_rvalid_o,
    output logic [31:0]          host_rdata_o,

    input  logic [7:0]           gp_i,
    output logic [15:0]          gp_o
);

  gpio #(
    .GpiWidth (8),
    .GpoWidth (16)
  ) u_gpio (
    .clk_i,
    .rst_ni,

    .device_req_i   (host_req_i),
    .device_addr_i  (host_addr_i),
    .device_we_i    (host_we_i),
    .device_be_i    (host_be_i),
    .device_wdata_i (host_wdata_i),
    .device_rvalid_o(host_rvalid_o),
    .device_rdata_o (host_rdata_o),

    .gp_i,
    .gp_o
  );

endmodule
