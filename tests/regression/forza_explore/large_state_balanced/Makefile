override ROOT_DIR := $(realpath ../../../..)
include $(ROOT_DIR)/config.mk
PROJECT := forza_large_msg_raw_balanced_compact
SRC_DIR := $(VORTEX_HOME)/tests/regression/forza_explore/large_state_balanced
SRCS := $(SRC_DIR)/main.cpp
VX_SRCS := $(SRC_DIR)/kernel.cpp
VX_HDRS := $(SRC_DIR)/common.h
VX_CFLAGS += -I$(SRC_DIR) -DLARGE_COMPACT -DLARGE_MESSAGE -DLARGE_RAW -DLARGE_NO_CACHE
CXXFLAGS += -I$(SRC_DIR) -DLARGE_COMPACT -DLARGE_MESSAGE -DLARGE_RAW -DLARGE_NO_CACHE
include $(VORTEX_HOME)/tests/regression/forza_explore/runtime/vortex.mk
KERNEL_LIB := vortex2
include ../../common.mk
all: kernel-msg_raw_balanced_compact.vxbin
kernel-msg_raw_balanced_compact.vxbin: kernel.vxbin
	cp $< $@
