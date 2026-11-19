ROOT_DIR := $(realpath ../../..)
include $(ROOT_DIR)/config.mk

MODE ?= agg_amo
PROJECT := forza_graph_$(MODE)
SRC_DIR := $(VORTEX_HOME)/tests/regression/forza_graph_coalesced
SRCS := $(SRC_DIR)/main.cpp
VX_SRCS := $(SRC_DIR)/kernel.cpp
VX_HDRS := $(SRC_DIR)/common.h
VX_CFLAGS += -I$(SRC_DIR) -std=c++17 -mllvm -vortex-divergence-max-bbs=512

ifeq ($(MODE),agg_amo)
MODE_DEFINE := -DGRAPH_AGG_AMO
else ifeq ($(MODE),agg_lmem)
MODE_DEFINE := -DGRAPH_AGG_LMEM
else ifeq ($(MODE),agg_comb)
MODE_DEFINE := -DGRAPH_AGG_COMB
else ifeq ($(MODE),hw_agg)
MODE_DEFINE := -DGRAPH_HW_AGG
VX_SRCS := $(SRC_DIR)/kernel_hw_agg.cpp
FORZA_RUNTIME_HOME ?= $(VORTEX_HOME)/sw/forza_runtime
include $(FORZA_RUNTIME_HOME)/vortex.mk
else
$(error MODE must be agg_amo, agg_lmem, agg_comb or hw_agg)
endif
VX_CFLAGS += $(MODE_DEFINE)
CXXFLAGS += $(MODE_DEFINE)
KERNEL_LIB := vortex2
OPTS ?= -v64 -e256 -gskew

include ../common.mk

all: kernel-$(MODE).vxbin
run-simx: kernel-$(MODE).vxbin

kernel-$(MODE).vxbin: kernel.vxbin
	cp $< $@
