override ROOT_DIR := $(realpath ../../..)
include $(ROOT_DIR)/config.mk
MODE ?= engine_mailbox
PROJECT := forza_attribution_opt_$(MODE)
SRC_DIR := $(VORTEX_HOME)/tests/regression/forza_pgas_accel_attribution_opt
SRCS := $(SRC_DIR)/main.cpp
VX_HDRS := $(SRC_DIR)/common.h $(SRC_DIR)/device.h $(VORTEX_HOME)/sw/common/forza_record_abi.h $(VORTEX_HOME)/sw/common/forza_mmio_abi.h
VX_CFLAGS += -I$(SRC_DIR) -std=c++17 -mllvm -vortex-divergence-max-bbs=1024
CXXFLAGS += -I$(SRC_DIR) -DRECORD_MODE=\"$(MODE)\"
ifeq ($(MODE),engine_direct)
VX_SRCS := $(SRC_DIR)/kernel_engine.cpp
VX_CFLAGS += -DFORZA_RECORD_DIRECT_MODE
else ifeq ($(MODE),engine_mailbox)
VX_SRCS := $(SRC_DIR)/kernel_engine.cpp
else
$(error unsupported MODE)
endif
KERNEL_LIB := vortex2
include ../common.mk
all run-simx: kernel-$(MODE).vxbin
kernel-$(MODE).vxbin: kernel.vxbin
	cp $< $@
