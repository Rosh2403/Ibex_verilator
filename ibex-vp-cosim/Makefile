# SPDX-License-Identifier: Apache-2.0

VERILATOR ?= verilator
IBEX_DEMO ?= ../ibex-demo-system
BUILD_DIR ?= build

RTL_GPIO := $(IBEX_DEMO)/rtl/system/gpio.sv
RTL_DBNC := $(IBEX_DEMO)/rtl/system/debounce.sv
RTL_TOP  := rtl/cosim_gpio_top.sv

VLTR_FLAGS := -Wall --cc --exe --build -j 0
VLTR_FLAGS += -I$(IBEX_DEMO)/rtl/system
VLTR_FLAGS += -CFLAGS "-std=c++14 -I$(CURDIR)/include"

.PHONY: all clean

all: $(BUILD_DIR)/Vcosim_gpio_top

$(BUILD_DIR)/Vcosim_gpio_top: $(RTL_TOP) $(RTL_GPIO) $(RTL_DBNC) cosim_main.cc include/cosim_protocol.h
	mkdir -p $(BUILD_DIR)
	$(VERILATOR) $(VLTR_FLAGS) -Mdir $(BUILD_DIR) -o Vcosim_gpio_server \
		$(RTL_TOP) $(RTL_GPIO) $(RTL_DBNC) cosim_main.cc

clean:
	rm -rf $(BUILD_DIR)
