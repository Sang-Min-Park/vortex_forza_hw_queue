ROOT_DIR := $(realpath ../../..)
include $(ROOT_DIR)/config.mk

MODE ?= msg_local
PROJECT := forza_graph_$(MODE)
SRC_DIR := $(VORTEX_HOME)/tests/regression/forza_graph_message_opt
SHARED_DIR := $(VORTEX_HOME)/tests/regression/forza_graph_coalesced
SRCS := $(SRC_DIR)/main.cpp
VX_SRCS := $(SRC_DIR)/kernel.cpp
VX_HDRS := $(SHARED_DIR)/common.h
VX_CFLAGS += -I$(SHARED_DIR)
CXXFLAGS += -I$(SHARED_DIR)
FORZA_RUNTIME_HOME ?= $(VORTEX_HOME)/sw/forza_runtime
ifneq ($(filter gpu_warp sw_warp,$(MODE)),)
VX_CFLAGS += -std=c++17 -mllvm -vortex-divergence-max-bbs=512 -DGRAPH_WARP
CXXFLAGS += -DGRAPH_WARP
ifeq ($(MODE),gpu_warp)
VX_SRCS := $(SRC_DIR)/kernel_gpu.cpp
VX_CFLAGS += -DGRAPH_GPU_WARP
CXXFLAGS += -DGRAPH_GPU_WARP
else
VX_SRCS := $(SRC_DIR)/kernel_sw.cpp
VX_CFLAGS += -DGRAPH_SW_WARP
CXXFLAGS += -DGRAPH_SW_WARP
endif
else
include $(FORZA_RUNTIME_HOME)/vortex.mk
ifeq ($(MODE),msg_private)
VX_CFLAGS += -DGRAPH_MSG_PRIVATE
CXXFLAGS += -DGRAPH_MSG_PRIVATE
else ifeq ($(MODE),msg_warp)
VX_CFLAGS += -DGRAPH_MSG_WARP -DGRAPH_WARP
CXXFLAGS += -DGRAPH_MSG_WARP -DGRAPH_WARP
else ifneq ($(MODE),msg_local)
$(error MODE must be msg_local, msg_private, msg_warp, sw_warp or gpu_warp)
endif
endif

KERNEL_LIB := vortex2
OPTS ?= -v64 -e256 -gskew
include ../common.mk

all: kernel-$(MODE).vxbin
run-simx: kernel-$(MODE).vxbin

kernel-$(MODE).vxbin: kernel.vxbin
	cp $< $@
