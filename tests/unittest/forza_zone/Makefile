ROOT_DIR := $(realpath ../../..)
include $(ROOT_DIR)/config.mk

PROJECT := forza_zone
SRC_DIR := $(VORTEX_HOME)/tests/unittest/$(PROJECT)

SRCS := $(SRC_DIR)/main.cpp $(VORTEX_HOME)/sim/simx/forza/forza_zone.cpp

CXXFLAGS += -I$(VORTEX_HOME)/sim/simx -I$(SIM_COMMON_DIR) -I$(THIRD_PARTY_DIR)
CXXFLAGS += -DVX_CFG_XLEN=$(XLEN) -DVX_CFG_XLEN_$(XLEN) -DVX_CFG_EXT_FORZA_ENABLE
LDFLAGS += -pthread

include ../common.mk
