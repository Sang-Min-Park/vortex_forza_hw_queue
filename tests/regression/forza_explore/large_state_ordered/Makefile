override ROOT_DIR := $(realpath ../../../..)
include $(ROOT_DIR)/config.mk
MODE ?= msg
COMPACT ?= 0
ifeq ($(COMPACT),1)
MODE_SUFFIX := _compact
VX_CFLAGS += -DLARGE_COMPACT
CXXFLAGS += -DLARGE_COMPACT
endif
PROJECT := forza_large_ordered_$(MODE)$(MODE_SUFFIX)
SRC_DIR := $(VORTEX_HOME)/tests/regression/forza_explore/large_state_ordered
SRCS := $(SRC_DIR)/main.cpp
VX_SRCS := $(SRC_DIR)/kernel.cpp
VX_HDRS := $(SRC_DIR)/common.h
VX_CFLAGS += -I$(SRC_DIR)
CXXFLAGS += -I$(SRC_DIR)
ifneq ($(filter msg msg_full,$(MODE)),)
ifeq ($(MODE),msg_full)
override FORZA_VORTEX_SHORT_MESSAGES := 0
endif
include $(VORTEX_HOME)/tests/regression/forza_explore/runtime_ordered/vortex.mk
VX_CFLAGS += -DLARGE_MESSAGE
CXXFLAGS += -DLARGE_MESSAGE
ifeq ($(MODE),msg_full)
VX_CFLAGS += -DLARGE_NO_CACHE
CXXFLAGS += -DLARGE_NO_CACHE -DLARGE_FULL_MESSAGE
else
VX_CFLAGS += -DFORZA_VORTEX_SHORT_MESSAGES
endif
else
$(error isolated ordering regression supports MODE=msg or msg_full)
endif
KERNEL_LIB := vortex2
OPTS ?= -b262144 -n65536 -ppermute -s17
include ../../common.mk
all: kernel-$(MODE)$(MODE_SUFFIX).vxbin
kernel-$(MODE)$(MODE_SUFFIX).vxbin: kernel.vxbin
	cp $< $@
