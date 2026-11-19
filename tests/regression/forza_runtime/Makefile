ROOT_DIR := $(realpath ../../..)
include $(ROOT_DIR)/config.mk

PROJECT := forza_runtime
SRC_DIR := $(VORTEX_HOME)/tests/regression/$(PROJECT)
FORZA_RUNTIME_HOME ?= $(VORTEX_HOME)/sw/forza_runtime
SRCS := $(SRC_DIR)/main.cpp
VX_SRCS := $(SRC_DIR)/kernel.cpp
VX_HDRS := $(SRC_DIR)/common.h $(VORTEX_HOME)/sw/common/forza_abi.h
include $(FORZA_RUNTIME_HOME)/vortex.mk
VX_CFLAGS += -I$(VORTEX_HOME)/sw/common
KERNEL_LIB := vortex2
OPTS ?= -n8

include ../common.mk
