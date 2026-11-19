ROOT_DIR := $(realpath ../../..)
include $(ROOT_DIR)/config.mk

TRANSPORT ?= hw
PROJECT := forza_graph_$(TRANSPORT)
SRC_DIR := $(VORTEX_HOME)/tests/regression/forza_graph
FORZA_RUNTIME_HOME ?= $(VORTEX_HOME)/sw/forza_runtime
SRCS := $(SRC_DIR)/main.cpp
VX_SRCS := $(SRC_DIR)/kernel.cpp
VX_HDRS := $(SRC_DIR)/common.h $(VORTEX_HOME)/sw/common/forza_abi.h
VX_HDRS += $(VORTEX_HOME)/sw/common/forza_software_abi.h
VX_CFLAGS += -I$(VORTEX_HOME)/sw/common -I$(SRC_DIR)

ifneq ($(filter gpu gpu_edge,$(TRANSPORT)),)
VX_SRCS := $(SRC_DIR)/kernel_gpu.cpp
VX_CFLAGS += -std=c++17 -mllvm -vortex-divergence-max-bbs=512 -DFORZA_GRAPH_GPU
CXXFLAGS += -DFORZA_GRAPH_GPU
ifeq ($(TRANSPORT),gpu_edge)
VX_CFLAGS += -DFORZA_GRAPH_GPU_EDGE
CXXFLAGS += -DFORZA_GRAPH_GPU_EDGE
endif
else
include $(FORZA_RUNTIME_HOME)/vortex.mk
ifeq ($(TRANSPORT),sw)
VX_CFLAGS += -DFORZA_GRAPH_SOFTWARE
VX_CFLAGS += -DFORZA_VORTEX_TRANSPORT_HEADER=\"forza_message_lib.software.h\"
CXXFLAGS += -DFORZA_GRAPH_SOFTWARE
else ifneq ($(TRANSPORT),hw)
$(error TRANSPORT must be hw, sw, gpu or gpu_edge)
endif
endif

KERNEL_LIB := vortex2
OPTS ?= -v64 -e256 -gskew

include ../common.mk

all: kernel-$(TRANSPORT).vxbin
run-simx: kernel-$(TRANSPORT).vxbin

kernel-$(TRANSPORT).vxbin: kernel.vxbin
	cp $< $@
