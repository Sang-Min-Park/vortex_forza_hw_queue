override ROOT_DIR := $(realpath ../../..)
include $(ROOT_DIR)/config.mk
MODE ?= native_packed
PROJECT := forza_feedback_packed_$(MODE)
SRC_DIR := $(VORTEX_HOME)/tests/regression/forza_pgas_feedback_packed
SRCS := $(SRC_DIR)/main.cpp
VX_SRCS := $(SRC_DIR)/kernel.cpp
VX_HDRS := $(SRC_DIR)/common.h $(SRC_DIR)/device.h $(VORTEX_HOME)/sw/common/forza_isa_abi.h $(VORTEX_HOME)/sw/common/forza_mmio_abi.h
VX_CFLAGS += -I$(SRC_DIR) -std=c++17 -mllvm -vortex-divergence-max-bbs=2048
CXXFLAGS += -I$(SRC_DIR) -DFEEDBACK_MODE=\"$(MODE)\"
ifeq ($(MODE),native_packed)
VX_CFLAGS += -DMESSAGE_TRANSPORT=0 -DSTATE_LOCAL=0 -DPACKED_NATIVE=1
else ifeq ($(MODE),native)
VX_CFLAGS += -DMESSAGE_TRANSPORT=0 -DSTATE_LOCAL=0
else ifeq ($(MODE),native_replicated_walk)
VX_CFLAGS += -DMESSAGE_TRANSPORT=0 -DSTATE_LOCAL=2
else ifeq ($(MODE),mmio_global)
VX_CFLAGS += -DMESSAGE_TRANSPORT=1 -DSTATE_LOCAL=0
else ifeq ($(MODE),gpu_queue_global)
VX_CFLAGS += -DMESSAGE_TRANSPORT=4 -DSTATE_LOCAL=0
else ifeq ($(MODE),gpu_queue_local)
VX_CFLAGS += -DMESSAGE_TRANSPORT=4 -DSTATE_LOCAL=1
else ifeq ($(MODE),mmio_local)
VX_CFLAGS += -DMESSAGE_TRANSPORT=1 -DSTATE_LOCAL=1
else ifeq ($(MODE),isa_try_global)
VX_CFLAGS += -DMESSAGE_TRANSPORT=2 -DSTATE_LOCAL=0
else ifeq ($(MODE),isa_try_local)
VX_CFLAGS += -DMESSAGE_TRANSPORT=2 -DSTATE_LOCAL=1
else ifeq ($(MODE),isa_wait_global)
VX_CFLAGS += -DMESSAGE_TRANSPORT=3 -DSTATE_LOCAL=0
else ifeq ($(MODE),isa_wait_local)
VX_CFLAGS += -DMESSAGE_TRANSPORT=3 -DSTATE_LOCAL=1
else
$(error unsupported MODE)
endif
KERNEL_LIB := vortex2
include ../common.mk
all run-simx: kernel-$(MODE).vxbin
kernel-$(MODE).vxbin: kernel.vxbin
	cp $< $@
