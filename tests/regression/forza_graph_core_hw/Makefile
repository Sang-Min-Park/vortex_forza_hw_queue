ROOT_DIR := $(realpath ../../..)
include $(ROOT_DIR)/config.mk

MODE ?= core_hw
PROJECT := forza_graph_$(MODE)
SRC_DIR := $(VORTEX_HOME)/tests/regression/forza_graph_core_hw
SHARED_DIR := $(VORTEX_HOME)/tests/regression/forza_graph_coalesced
SRCS := $(SRC_DIR)/main.cpp
VX_SRCS := $(SRC_DIR)/kernel.cpp
VX_HDRS := $(SHARED_DIR)/common.h
VX_CFLAGS += -I$(SHARED_DIR)
CXXFLAGS += -I$(SHARED_DIR)
FORZA_RUNTIME_HOME ?= $(VORTEX_HOME)/sw/forza_runtime
include $(FORZA_RUNTIME_HOME)/vortex.mk
ifeq ($(MODE),core_hw_fixed)
VX_CFLAGS += -DGRAPH_CORE_HW_FIXED
CXXFLAGS += -DGRAPH_CORE_HW_FIXED
else ifneq ($(MODE),core_hw)
$(error MODE must be core_hw or core_hw_fixed)
endif

KERNEL_LIB := vortex2
OPTS ?= -v64 -e256 -gskew
include ../common.mk

all: kernel-$(MODE).vxbin
run-simx: kernel-$(MODE).vxbin

kernel-$(MODE).vxbin: kernel.vxbin
	cp $< $@
