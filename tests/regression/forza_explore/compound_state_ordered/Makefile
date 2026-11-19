override ROOT_DIR := $(realpath ../../../..)
include $(ROOT_DIR)/config.mk

PROJECT := forza_compound_ordered_mp
SRC_DIR := $(VORTEX_HOME)/tests/regression/forza_explore/compound_state_ordered
SRCS := $(SRC_DIR)/main.cpp
VX_SRCS := $(SRC_DIR)/kernel_mp.cpp
VX_HDRS := $(SRC_DIR)/common.h $(SRC_DIR)/state.h
VX_CFLAGS += -I$(SRC_DIR)
FORZA_VORTEX_SHORT_MESSAGES := 1
include $(VORTEX_HOME)/tests/regression/forza_explore/runtime_ordered/vortex.mk
CXXFLAGS += -DCOMPOUND_MODE=\"mp\"
KERNEL_LIB := vortex2
OPTS ?= -n64 -u128 -ghot -k kernel-ordered-mp.vxbin
include ../../common.mk

all: kernel-ordered-mp.vxbin
kernel-ordered-mp.vxbin: kernel.vxbin
	cp $< $@
