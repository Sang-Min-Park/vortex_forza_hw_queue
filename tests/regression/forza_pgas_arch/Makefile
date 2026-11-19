override ROOT_DIR := $(realpath ../../..)
include $(ROOT_DIR)/config.mk
MODE ?= gpu_global
PROJECT := forza_arch_$(MODE)
SRC_DIR := $(VORTEX_HOME)/tests/regression/forza_pgas_arch
SRCS := $(SRC_DIR)/main.cpp
VX_SRCS := $(SRC_DIR)/kernel.cpp
VX_HDRS := $(SRC_DIR)/common.h $(VORTEX_HOME)/sw/common/forza_update_abi.h $(VORTEX_HOME)/sw/common/forza_mmio_abi.h
VX_CFLAGS += -I$(SRC_DIR) -std=c++17 -mllvm -vortex-divergence-max-bbs=512
CXXFLAGS += -I$(SRC_DIR)
ifeq ($(MODE),gpu_global)
MODE_DEFINE := ARCH_GPU_GLOBAL
else ifeq ($(MODE),gpu_local)
MODE_DEFINE := ARCH_GPU_LOCAL
else ifeq ($(MODE),gpu_core_comb)
MODE_DEFINE := ARCH_GPU_COMB
else ifeq ($(MODE),engine_hybrid)
MODE_DEFINE := ARCH_ENGINE_HYBRID
else ifeq ($(MODE),engine_comb)
MODE_DEFINE := ARCH_ENGINE_COMB
else
$(error unsupported MODE)
endif
VX_CFLAGS += -D$(MODE_DEFINE)
CXXFLAGS += -D$(MODE_DEFINE)
KERNEL_LIB := vortex2
include ../common.mk
all run-simx: kernel-$(MODE).vxbin
kernel-$(MODE).vxbin: kernel.vxbin
	cp $< $@
