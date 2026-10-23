# SPDX-License-Identifier: Apache-2.0

VERILATOR ?= verilator
IBEX_DEMO ?= ../ibex-demo-system
BUILD_DIR ?= build

RTL_GPIO  := $(IBEX_DEMO)/rtl/system/gpio.sv
RTL_DBNC  := $(IBEX_DEMO)/rtl/system/debounce.sv
RTL_UART  := $(IBEX_DEMO)/rtl/system/uart.sv
RTL_TIMER := $(IBEX_DEMO)/vendor/lowrisc_ibex/shared/rtl/timer.sv
RTL_FIFO  := $(IBEX_DEMO)/vendor/lowrisc_ip/ip/prim/rtl/prim_fifo_sync.sv
RTL_FCNT  := $(IBEX_DEMO)/vendor/lowrisc_ip/ip/prim/rtl/prim_fifo_sync_cnt.sv
RTL_UTIL  := $(IBEX_DEMO)/vendor/lowrisc_ip/ip/prim/rtl/prim_util_pkg.sv
RTL_CNTP  := $(IBEX_DEMO)/vendor/lowrisc_ip/ip/prim/rtl/prim_count_pkg.sv
RTL_CNT   := $(IBEX_DEMO)/vendor/lowrisc_ip/ip/prim/rtl/prim_count.sv
RTL_TOP   := rtl/cosim_gpio_top.sv
RTL_PTOP  := rtl/cosim_periph_top.sv
PRIM_DIR  := $(IBEX_DEMO)/vendor/lowrisc_ip/ip/prim/rtl

VLTR_FLAGS := -Wall --cc --exe --build -j 0
VLTR_FLAGS += -Wno-UNOPTFLAT -Wno-UNUSED -Wno-DECLFILENAME -Wno-WIDTH
VLTR_FLAGS += -Wno-PINCONNECTEMPTY -Wno-TIMESCALEMOD -Wno-BLKANDNBLK
VLTR_FLAGS += -I$(IBEX_DEMO)/rtl/system -I$(PRIM_DIR)
VLTR_FLAGS += -CFLAGS "-std=c++14 -I$(CURDIR)/include"

.PHONY: all periph clean

all: $(BUILD_DIR)/Vcosim_gpio_server $(BUILD_DIR)/Vcosim_periph_server

periph: $(BUILD_DIR)/Vcosim_periph_server

$(BUILD_DIR)/Vcosim_gpio_server: $(RTL_TOP) $(RTL_GPIO) $(RTL_DBNC) cosim_main.cc include/cosim_protocol.h
	mkdir -p $(BUILD_DIR)/gpio
	$(VERILATOR) $(VLTR_FLAGS) -Mdir $(BUILD_DIR)/gpio -o Vcosim_gpio_server \
		$(RTL_TOP) $(RTL_GPIO) $(RTL_DBNC) cosim_main.cc
	cp $(BUILD_DIR)/gpio/Vcosim_gpio_server $(BUILD_DIR)/Vcosim_gpio_server

$(BUILD_DIR)/Vcosim_periph_server: $(RTL_PTOP) $(RTL_GPIO) $(RTL_DBNC) $(RTL_UART) \
		$(RTL_TIMER) $(RTL_FIFO) $(RTL_FCNT) $(RTL_UTIL) $(RTL_CNTP) $(RTL_CNT) \
		cosim_periph_main.cc include/cosim_protocol.h
	mkdir -p $(BUILD_DIR)/periph
	$(VERILATOR) $(VLTR_FLAGS) --top-module cosim_periph_top \
		-Mdir $(BUILD_DIR)/periph -o Vcosim_periph_server \
		$(RTL_CNTP) $(RTL_UTIL) $(RTL_CNT) $(RTL_PTOP) $(RTL_GPIO) $(RTL_DBNC) \
		$(RTL_UART) $(RTL_TIMER) $(RTL_FIFO) $(RTL_FCNT) cosim_periph_main.cc
	cp $(BUILD_DIR)/periph/Vcosim_periph_server $(BUILD_DIR)/Vcosim_periph_server

clean:
	rm -rf $(BUILD_DIR)
