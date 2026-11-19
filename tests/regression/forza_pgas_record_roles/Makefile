override ROOT_DIR := $(realpath ../../..)
include $(ROOT_DIR)/config.mk
MODE ?= software_split
PROJECT := forza_record_roles_$(MODE)
SRC_DIR := $(VORTEX_HOME)/tests/regression/forza_pgas_record_roles
SRCS := $(SRC_DIR)/main.cpp
VX_HDRS := $(SRC_DIR)/common.h $(SRC_DIR)/device.h $(SRC_DIR)/handler.h $(SRC_DIR)/field_handler.h $(SRC_DIR)/transport.h $(VORTEX_HOME)/sw/common/forza_mmio_abi.h
VX_CFLAGS += -I$(SRC_DIR) -std=c++17 -mllvm -vortex-divergence-max-bbs=1024
CXXFLAGS += -I$(SRC_DIR) -DRECORD_MODE=\"$(MODE)\"
ifeq ($(MODE),gpu_field_core)
VX_SRCS := $(SRC_DIR)/kernel_field_core.cpp
else ifeq ($(MODE),software_split)
VX_SRCS := $(SRC_DIR)/kernel_split.cpp
else
$(error unsupported MODE)
endif
KERNEL_LIB := vortex2
include ../common.mk
all run-simx: kernel-$(MODE).vxbin
kernel-$(MODE).vxbin: kernel.vxbin
	cp $< $@
