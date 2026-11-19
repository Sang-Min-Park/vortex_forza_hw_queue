ROOT_DIR := $(realpath ../../..)
include $(ROOT_DIR)/config.mk

PROJECT := forza_fabric
SRC_DIR := $(VORTEX_HOME)/tests/unittest/$(PROJECT)

SRCS := $(SRC_DIR)/main.cpp $(VORTEX_HOME)/sim/simx/forza/forza_fabric.cpp

CXXFLAGS += -I$(VORTEX_HOME)/sim/simx -I$(SIM_COMMON_DIR) -I$(THIRD_PARTY_DIR)
CXXFLAGS += -DVX_CFG_XLEN=$(XLEN) -DVX_CFG_XLEN_$(XLEN)
LDFLAGS += -pthread

include ../common.mk
