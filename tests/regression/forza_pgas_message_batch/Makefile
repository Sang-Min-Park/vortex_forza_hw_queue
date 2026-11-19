override ROOT_DIR := $(realpath ../../..)
include $(ROOT_DIR)/config.mk
MODE ?= batch1
PROJECT := forza_message_batch_$(MODE)
SRC_DIR := $(VORTEX_HOME)/tests/regression/forza_pgas_message_batch
SRCS := $(SRC_DIR)/main.cpp
VX_HDRS := $(SRC_DIR)/common.h $(SRC_DIR)/device.h $(SRC_DIR)/field_handler.h $(SRC_DIR)/transport.h $(VORTEX_HOME)/sw/common/forza_mmio_abi.h
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
else ifeq ($(MODE),batch1)
VX_SRCS := $(SRC_DIR)/kernel_batch.cpp
VX_CFLAGS += -DMESSAGE_BATCH_SIZE=1
else ifeq ($(MODE),batch4)
VX_SRCS := $(SRC_DIR)/kernel_batch.cpp
VX_CFLAGS += -DMESSAGE_BATCH_SIZE=4
else ifeq ($(MODE),batch8)
VX_SRCS := $(SRC_DIR)/kernel_batch.cpp
VX_CFLAGS += -DMESSAGE_BATCH_SIZE=8
else
$(error unsupported MODE)
endif
KERNEL_LIB := vortex2
include ../common.mk
all run-simx: kernel-$(MODE).vxbin
kernel-$(MODE).vxbin: kernel.vxbin
	cp $< $@
