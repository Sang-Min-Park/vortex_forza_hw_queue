override ROOT_DIR := $(realpath ../../..)
include $(ROOT_DIR)/config.mk
MODE ?= gpu_warp
PROJECT := forza_record_opt_$(MODE)
SRC_DIR := $(VORTEX_HOME)/tests/regression/forza_pgas_record_gpu_opt
SRCS := $(SRC_DIR)/main.cpp
VX_HDRS := $(SRC_DIR)/common.h $(SRC_DIR)/device.h $(VORTEX_HOME)/sw/common/forza_record_abi.h $(VORTEX_HOME)/sw/common/forza_mmio_abi.h
VX_CFLAGS += -I$(SRC_DIR) -std=c++17 -mllvm -vortex-divergence-max-bbs=1024
CXXFLAGS += -I$(SRC_DIR) -DRECORD_MODE=\"$(MODE)\"
ifeq ($(MODE),gpu_lock)
VX_SRCS := $(SRC_DIR)/kernel_lock.cpp
else ifeq ($(MODE),gpu_warp)
VX_SRCS := $(SRC_DIR)/kernel_warp.cpp
else ifeq ($(MODE),gpu_owner)
VX_SRCS := $(SRC_DIR)/kernel_bucket.cpp
else ifeq ($(MODE),engine)
VX_SRCS := $(SRC_DIR)/kernel_engine.cpp
else
$(error unsupported MODE)
endif
KERNEL_LIB := vortex2
include ../common.mk
all run-simx: kernel-$(MODE).vxbin
kernel-$(MODE).vxbin: kernel.vxbin
	cp $< $@
