override ROOT_DIR := $(realpath ../../../..)
include $(ROOT_DIR)/config.mk
MODE ?= plain
PROJECT := forza_owner_rmw_$(MODE)
SRC_DIR := $(VORTEX_HOME)/tests/regression/forza_explore/owner_rmw
SRCS := $(SRC_DIR)/main.cpp
VX_SRCS := $(SRC_DIR)/kernel.cpp
VX_HDRS := $(SRC_DIR)/common.h
VX_CFLAGS += -I$(SRC_DIR) -std=c++17 -mllvm -vortex-divergence-max-bbs=512
CXXFLAGS += -I$(SRC_DIR)
ifeq ($(MODE),ack)
VX_CFLAGS += -DOWNER_ACK_STORE
CXXFLAGS += -DOWNER_ACK_STORE
else ifneq ($(MODE),plain)
$(error MODE must be plain or ack)
endif
KERNEL_LIB := vortex2
include $(ROOT_DIR)/tests/regression/common.mk
all: kernel-$(MODE).vxbin
kernel-$(MODE).vxbin: kernel.vxbin
	cp $< $@
