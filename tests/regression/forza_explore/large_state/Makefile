override ROOT_DIR := $(realpath ../../../..)
include $(ROOT_DIR)/config.mk
MODE ?= msg
COMPACT ?= 0
ifeq ($(COMPACT),1)
MODE_SUFFIX := _compact
VX_CFLAGS += -DLARGE_COMPACT
CXXFLAGS += -DLARGE_COMPACT
endif
PROJECT := forza_large_$(MODE)$(MODE_SUFFIX)
SRC_DIR := $(VORTEX_HOME)/tests/regression/forza_explore/large_state
SRCS := $(SRC_DIR)/main.cpp
VX_SRCS := $(SRC_DIR)/kernel.cpp
VX_HDRS := $(SRC_DIR)/common.h
VX_CFLAGS += -I$(SRC_DIR)
CXXFLAGS += -I$(SRC_DIR)
ifneq ($(filter msg msg_plain msg_full msg_raw msg_batch,$(MODE)),)
ifeq ($(MODE),msg_full)
override FORZA_VORTEX_SHORT_MESSAGES := 0
endif
include $(VORTEX_HOME)/tests/regression/forza_explore/runtime/vortex.mk
VX_CFLAGS += -DLARGE_MESSAGE
CXXFLAGS += -DLARGE_MESSAGE
ifneq ($(filter msg_raw msg_batch,$(MODE)),)
ifneq ($(COMPACT),1)
$(error raw and batch modes require COMPACT=1)
endif
VX_SRCS := $(SRC_DIR)/kernel_raw.cpp
VX_CFLAGS += -DLARGE_RAW
CXXFLAGS += -DLARGE_RAW
ifeq ($(MODE),msg_batch)
VX_CFLAGS += -DLARGE_BATCH
CXXFLAGS += -DLARGE_BATCH
endif
endif
ifneq ($(MODE),msg)
VX_CFLAGS += -DLARGE_NO_CACHE
CXXFLAGS += -DLARGE_NO_CACHE
endif
ifeq ($(MODE),msg_full)
CXXFLAGS += -DLARGE_FULL_MESSAGE
else
VX_CFLAGS += -DFORZA_VORTEX_SHORT_MESSAGES
endif
else
VX_CFLAGS += -std=c++17 -mllvm -vortex-divergence-max-bbs=512
ifeq ($(MODE),gpu)
VX_CFLAGS += -DLARGE_GPU_PLAIN -DLARGE_NO_CACHE
CXXFLAGS += -DLARGE_GPU_PLAIN -DLARGE_NO_CACHE
else ifneq ($(MODE),gpu_cache)
$(error invalid MODE)
endif
endif
KERNEL_LIB := vortex2
OPTS ?= -b2097152 -n4096 -prandom
include ../../common.mk
all: kernel-$(MODE)$(MODE_SUFFIX).vxbin
kernel-$(MODE)$(MODE_SUFFIX).vxbin: kernel.vxbin
	cp $< $@
