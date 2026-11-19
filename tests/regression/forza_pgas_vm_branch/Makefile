override ROOT_DIR := $(realpath ../../..)
include $(ROOT_DIR)/config.mk
MODE ?= vm_mailbox
PROGRAM ?= 2
BASE_MODE := $(patsubst %_pad,%,$(MODE))
PROJECT := forza_vm_branch_$(PROGRAM)_$(MODE)
SRC_DIR := $(VORTEX_HOME)/tests/regression/forza_pgas_vm_branch
SRCS := $(SRC_DIR)/main.cpp
VX_HDRS := $(wildcard $(SRC_DIR)/*.h) $(VORTEX_HOME)/sw/common/forza_vm_abi.h
VX_CFLAGS += -I$(SRC_DIR) -std=c++17 -DVM_PROGRAM=$(PROGRAM) -mllvm -vortex-divergence-max-bbs=1024
CXXFLAGS += -I$(SRC_DIR) -DVM_PROGRAM=$(PROGRAM) -DRECORD_MODE=\"$(BASE_MODE)\"
ifneq ($(filter %_pad,$(MODE)),)
VX_CFLAGS += -DVM_PADDED
CXXFLAGS += -DVM_PADDED
endif
ifeq ($(BASE_MODE),gpu_lock)
VX_SRCS := $(SRC_DIR)/kernel_lock.cpp
else ifeq ($(BASE_MODE),gpu_warp)
VX_SRCS := $(SRC_DIR)/kernel_warp.cpp
else ifeq ($(BASE_MODE),gpu_owner)
VX_SRCS := $(SRC_DIR)/kernel_bucket.cpp
else ifeq ($(BASE_MODE),vm_direct)
VX_SRCS := $(SRC_DIR)/kernel_engine.cpp
VX_CFLAGS += -DFORZA_VM_DIRECT_MODE
else ifeq ($(BASE_MODE),vm_mailbox)
VX_SRCS := $(SRC_DIR)/kernel_engine.cpp
else
$(error unsupported MODE)
endif
KERNEL_LIB := vortex2
include ../common.mk
