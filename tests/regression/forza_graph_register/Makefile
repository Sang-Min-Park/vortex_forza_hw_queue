ROOT_DIR := $(realpath ../../..)
include $(ROOT_DIR)/config.mk

MODE ?= msg_cache
PROJECT := forza_graph_$(MODE)
SRC_DIR := $(VORTEX_HOME)/tests/regression/forza_graph_register
SRCS := $(SRC_DIR)/main.cpp
VX_SRCS := $(SRC_DIR)/kernel.cpp
VX_HDRS := $(SRC_DIR)/common.h $(SRC_DIR)/fold.h
VX_CFLAGS += -I$(SRC_DIR)
CXXFLAGS += -I$(SRC_DIR)
ifeq ($(MODE),msg_cache)
FORZA_RUNTIME_HOME ?= $(VORTEX_HOME)/sw/forza_runtime
include $(FORZA_RUNTIME_HOME)/vortex.mk
else
VX_CFLAGS += -std=c++17 -mllvm -vortex-divergence-max-bbs=512
ifeq ($(MODE),sw_cache)
VX_SRCS := $(SRC_DIR)/kernel_sw.cpp
MODE_DEFINE := -DGRAPH_REGISTER_SW
else ifneq ($(filter gpu_cache gpu_range,$(MODE)),)
VX_SRCS := $(SRC_DIR)/kernel_gpu.cpp
MODE_DEFINE := -DGRAPH_REGISTER_GPU
ifeq ($(MODE),gpu_range)
MODE_DEFINE += -DGRAPH_REGISTER_RANGE
endif
else
$(error MODE must be msg_cache, sw_cache, gpu_cache or gpu_range)
endif
VX_CFLAGS += $(MODE_DEFINE)
CXXFLAGS += $(MODE_DEFINE)
endif

KERNEL_LIB := vortex2
OPTS ?= -v64 -e256 -gskew
include ../common.mk

all: kernel-$(MODE).vxbin
run-simx: kernel-$(MODE).vxbin

kernel-$(MODE).vxbin: kernel.vxbin
	cp $< $@
