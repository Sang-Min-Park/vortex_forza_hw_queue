override ROOT_DIR := $(realpath ../../../../../..)
include $(ROOT_DIR)/config.mk
SHORT ?= 0
ifeq ($(SHORT),1)
MODE := short
CXXFLAGS += -DRUNTIME_SHORT
else
MODE := full
endif
PROJECT := forza_runtime_smoke_$(MODE)
SRC_DIR := $(VORTEX_HOME)/tests/regression/forza_explore/runtime/test/simx
SRCS := $(SRC_DIR)/main.cpp
VX_SRCS := $(SRC_DIR)/kernel.cpp
VX_HDRS := $(SRC_DIR)/common.h
VX_CFLAGS += -I$(SRC_DIR)
FORZA_VORTEX_SHORT_MESSAGES := $(SHORT)
include $(VORTEX_HOME)/tests/regression/forza_explore/runtime/vortex.mk
KERNEL_LIB := vortex2
include $(ROOT_DIR)/tests/regression/common.mk
all: kernel-$(MODE).vxbin
kernel-$(MODE).vxbin: kernel.vxbin
	cp $< $@
