override ROOT_DIR := $(realpath ../../..)
include $(ROOT_DIR)/config.mk
MODE ?= hw_owner
TRANSPORT ?= csr
STATE ?= local
PROJECT := forza_pgas_striped_$(MODE)
SRC_DIR := $(VORTEX_HOME)/tests/regression/forza_pgas_striped
TRANSPORT_DIR := $(VORTEX_HOME)/tests/regression/forza_pgas_explore
SRCS := $(SRC_DIR)/main.cpp
VX_SRCS := $(SRC_DIR)/kernel.cpp
VX_HDRS := $(SRC_DIR)/common.h
VX_CFLAGS += -I$(SRC_DIR) -I$(TRANSPORT_DIR) -std=c++17 -mllvm -vortex-divergence-max-bbs=512
CXXFLAGS += -I$(SRC_DIR)
ifeq ($(MODE),hw_owner)
PROJECT := forza_pgas_striped_$(MODE)_$(TRANSPORT)_$(STATE)
include $(VORTEX_HOME)/tests/regression/forza_explore/runtime_ordered/vortex.mk
VX_CFLAGS += -DPGAS_HW
CXXFLAGS += -DPGAS_HW
VX_HDRS += $(TRANSPORT_DIR)/transport.h
VX_HDRS += $(VORTEX_HOME)/sw/common/forza_mmio_abi.h $(VORTEX_HOME)/sw/common/forza_abi.h
ifeq ($(TRANSPORT),mmio)
VX_CFLAGS += -DPGAS_TRANSPORT_MMIO
CXXFLAGS += -DPGAS_TRANSPORT_MMIO
else ifneq ($(TRANSPORT),csr)
$(error TRANSPORT must be csr or mmio)
endif
ifeq ($(STATE),global)
VX_CFLAGS += -DPGAS_STATE_GLOBAL
CXXFLAGS += -DPGAS_STATE_GLOBAL
else ifneq ($(STATE),local)
$(error STATE must be local or global)
endif
else ifeq ($(MODE),sw_owner)
VX_CFLAGS += -DPGAS_SW
CXXFLAGS += -DPGAS_SW
else ifeq ($(MODE),gpu_local)
VX_CFLAGS += -DPGAS_GPU_LOCAL
CXXFLAGS += -DPGAS_GPU_LOCAL
else ifeq ($(MODE),gpu_global)
VX_CFLAGS += -DPGAS_GPU_GLOBAL
CXXFLAGS += -DPGAS_GPU_GLOBAL
else
$(error MODE must be hw_owner, sw_owner, gpu_local or gpu_global)
endif
KERNEL_LIB := vortex2
include ../common.mk
all: kernel-$(PROJECT).vxbin
run-simx: kernel-$(PROJECT).vxbin
kernel-$(PROJECT).vxbin: kernel.vxbin
	cp $< $@
