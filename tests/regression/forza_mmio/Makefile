override ROOT_DIR := $(realpath ../../..)
include $(ROOT_DIR)/config.mk
MODE ?= mmio
PROJECT := forza_mmio_smoke_$(MODE)
SRC_DIR := $(VORTEX_HOME)/tests/regression/forza_mmio
SRCS := $(SRC_DIR)/main.cpp
VX_SRCS := $(SRC_DIR)/kernel.cpp
VX_HDRS := $(SRC_DIR)/common.h
VX_HDRS += $(VORTEX_HOME)/tests/regression/forza_pgas_explore/transport.h
VX_HDRS += $(VORTEX_HOME)/sw/common/forza_mmio_abi.h
VX_CFLAGS += -I$(SRC_DIR) -I$(VORTEX_HOME)/tests/regression/forza_pgas_explore
ifeq ($(MODE),mmio)
VX_CFLAGS += -DPGAS_TRANSPORT_MMIO
endif
include $(VORTEX_HOME)/tests/regression/forza_explore/runtime_ordered/vortex.mk
KERNEL_LIB := vortex2
CXXFLAGS += -DTRANSPORT_NAME=\"$(MODE)\"
include ../common.mk
all: kernel-$(MODE).vxbin
kernel-$(MODE).vxbin: kernel.vxbin
	cp $< $@
