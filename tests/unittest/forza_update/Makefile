ROOT_DIR := $(realpath ../../..)
include $(ROOT_DIR)/config.mk

PROJECT := forza_update
SRC_DIR := $(VORTEX_HOME)/tests/unittest/$(PROJECT)
SRCS := $(SRC_DIR)/main.cpp $(VORTEX_HOME)/sim/simx/forza/forza_zone.cpp
SRCS += $(VORTEX_HOME)/sim/simx/mem/local_mem.cpp
SRCS += $(VORTEX_HOME)/sim/simx/amo/amo_unit.cpp $(SIM_COMMON_DIR)/mem.cpp
SRCS += $(SW_COMMON_DIR)/util.cpp
CXXFLAGS += -I$(VORTEX_HOME)/sim/simx -I$(VORTEX_HOME)/sim/simx/amo
CXXFLAGS += -I$(SIM_COMMON_DIR) -I$(THIRD_PARTY_DIR)
CXXFLAGS += -DVX_CFG_XLEN=$(XLEN) -DVX_CFG_XLEN_$(XLEN)
CXXFLAGS += -DVX_CFG_EXT_FORZA_ENABLE -DVX_CFG_EXT_A_ENABLE
CXXFLAGS += -DVX_CFG_FORZA_MMIO_ENABLE -DVX_CFG_FORZA_UPDATE_ENABLE
LDFLAGS += -pthread
include ../common.mk
