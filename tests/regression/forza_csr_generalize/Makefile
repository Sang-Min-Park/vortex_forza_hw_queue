ROOT_DIR := $(realpath ../../..)
include $(ROOT_DIR)/config.mk

PROJECT := forza_csr_generalize
SRC_DIR := $(VORTEX_HOME)/tests/regression/$(PROJECT)
SRCS := $(SRC_DIR)/main.cpp
VX_SRCS := $(SRC_DIR)/kernel.cpp
VX_HDRS := $(SRC_DIR)/common.h $(VORTEX_HOME)/sw/common/forza_abi.h
VX_CFLAGS += -I$(VORTEX_HOME)/sw/common
VX_CFLAGS += -mllvm -vortex-divergence-max-bbs=512
KERNEL_LIB := vortex2
OPTS ?= -w1 -m physical -n4

include ../common.mk
