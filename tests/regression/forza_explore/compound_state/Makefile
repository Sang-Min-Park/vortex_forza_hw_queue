override ROOT_DIR := $(realpath ../../../..)
include $(ROOT_DIR)/config.mk

MODE ?= mp
PROJECT := forza_compound_$(MODE)
SRC_DIR := $(VORTEX_HOME)/tests/regression/forza_explore/compound_state
SRCS := $(SRC_DIR)/main.cpp
VX_HDRS := $(SRC_DIR)/common.h $(SRC_DIR)/state.h
VX_CFLAGS += -I$(SRC_DIR)
ifeq ($(MODE),mp)
VX_SRCS := $(SRC_DIR)/kernel_mp.cpp
FORZA_VORTEX_SHORT_MESSAGES := 1
ifeq ($(ACTIVE_PROGRESS),1)
VX_CFLAGS += -DCOMPOUND_ACTIVE_GATING
endif
include $(VORTEX_HOME)/tests/regression/forza_explore/runtime/vortex.mk
else
VX_CFLAGS += -std=c++17 -mllvm -vortex-divergence-max-bbs=512
ifeq ($(MODE),gpu_lock)
VX_SRCS := $(SRC_DIR)/kernel_lock.cpp
else ifeq ($(MODE),gpu_soa)
VX_SRCS := $(SRC_DIR)/kernel_lock.cpp
VX_CFLAGS += -DCOMPOUND_SOA
CXXFLAGS += -DCOMPOUND_SOA
else ifeq ($(MODE),gpu_owner)
VX_SRCS := $(SRC_DIR)/kernel_bucket.cpp
VX_CFLAGS += -DCOMPOUND_OWNER
CXXFLAGS += -DCOMPOUND_OWNER
else
$(error MODE must be mp, gpu_lock, gpu_soa or gpu_owner)
endif
endif

CXXFLAGS += -DCOMPOUND_MODE=\"$(MODE)\"
KERNEL_LIB := vortex2
OPTS ?= -n64 -u128 -ghot
include ../../common.mk

all: kernel-$(MODE).vxbin
run-simx: kernel-$(MODE).vxbin

kernel-$(MODE).vxbin: kernel.vxbin
	cp $< $@
