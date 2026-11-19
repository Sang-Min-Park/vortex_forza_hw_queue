override ROOT_DIR := $(realpath ../../..)
include $(ROOT_DIR)/config.mk
MODE ?= native_grouped
ifneq ($(MODE),native_grouped)
$(error unsupported MODE)
endif
PROJECT := forza_feedback_$(MODE)
SRC_DIR := $(VORTEX_HOME)/tests/regression/forza_pgas_feedback_grouped
SRCS := $(SRC_DIR)/main.cpp
VX_SRCS := $(SRC_DIR)/kernel.cpp
VX_HDRS := $(wildcard $(SRC_DIR)/*.h) $(VORTEX_HOME)/sw/common/forza_isa_abi.h $(VORTEX_HOME)/sw/common/forza_mmio_abi.h
VX_CFLAGS += -I$(SRC_DIR) -std=c++17 -DMESSAGE_TRANSPORT=0 -DSTATE_LOCAL=0 -mllvm -vortex-divergence-max-bbs=2048
CXXFLAGS += -I$(SRC_DIR) -DFEEDBACK_MODE=\"$(MODE)\"
KERNEL_LIB := vortex2
include ../common.mk
all run-simx: kernel-$(MODE).vxbin
kernel-$(MODE).vxbin: kernel.vxbin
	cp $< $@
