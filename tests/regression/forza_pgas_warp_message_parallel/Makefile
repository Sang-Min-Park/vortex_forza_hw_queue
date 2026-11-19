override ROOT_DIR := $(realpath ../../..)
include $(ROOT_DIR)/config.mk
MODE ?= warp_mmio8
PROJECT := forza_warp_parallel_$(MODE)
SRC_DIR := $(VORTEX_HOME)/tests/regression/forza_pgas_warp_message_parallel
SRCS := $(SRC_DIR)/main.cpp
VX_HDRS := $(wildcard $(SRC_DIR)/*.h) $(VORTEX_HOME)/sw/common/forza_mmio_abi.h $(VORTEX_HOME)/sw/common/forza_isa_abi.h
VX_CFLAGS += -I$(SRC_DIR) -std=c++17 -mllvm -vortex-divergence-max-bbs=1024
CXXFLAGS += -I$(SRC_DIR) -DRECORD_MODE=\"$(MODE)\"
ifeq ($(MODE),gpu_lock)
VX_SRCS := $(SRC_DIR)/kernel_lock.cpp
else ifeq ($(MODE),gpu_warp)
VX_SRCS := $(SRC_DIR)/kernel_warp.cpp
else ifeq ($(MODE),gpu_owner)
VX_SRCS := $(SRC_DIR)/kernel_bucket.cpp
else ifeq ($(MODE),gpu_field_core)
VX_SRCS := $(SRC_DIR)/kernel_field_core.cpp
else ifeq ($(MODE),gpu_warp_core)
VX_SRCS := $(SRC_DIR)/kernel_warp_core.cpp
else ifeq ($(MODE),field_mmio8)
VX_SRCS := $(SRC_DIR)/kernel_field_batch.cpp
VX_CFLAGS += -DMESSAGE_BATCH_SIZE=8
else ifneq ($(filter $(MODE),warp_mmio8 warp_mmio32 warp_isa8 warp_isa32 warp_wait8 warp_wait32 warp_coop_mmio32 warp_coop_isa32 warp_coop_wait32),)
VX_SRCS := $(SRC_DIR)/kernel_warp_batch.cpp
ifneq ($(findstring 32,$(MODE)),)
VX_CFLAGS += -DMESSAGE_BATCH_SIZE=32
else
VX_CFLAGS += -DMESSAGE_BATCH_SIZE=8
endif
ifneq ($(filter $(MODE),warp_isa8 warp_isa32 warp_wait8 warp_wait32 warp_coop_isa32 warp_coop_wait32),)
VX_CFLAGS += -DMESSAGE_USE_ISA
endif
ifneq ($(filter $(MODE),warp_wait8 warp_wait32 warp_coop_wait32),)
VX_CFLAGS += -DMESSAGE_WAIT
endif
ifneq ($(findstring coop,$(MODE)),)
VX_CFLAGS += -DMESSAGE_COOPERATIVE_PRODUCER
endif
else
$(error unsupported MODE)
endif
KERNEL_LIB := vortex2
include ../common.mk
all run-simx: kernel-$(MODE).vxbin
kernel-$(MODE).vxbin: kernel.vxbin
	cp $< $@
