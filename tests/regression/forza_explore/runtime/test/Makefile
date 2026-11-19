ROOT_DIR := $(abspath $(dir $(abspath $(lastword $(MAKEFILE_LIST))))/../../../../..)
ifneq ($(wildcard $(ROOT_DIR)/config.mk),)
include $(ROOT_DIR)/config.mk
endif
VORTEX_HOME ?= $(ROOT_DIR)
SRC_DIR := $(VORTEX_HOME)/tests/regression/forza_explore/runtime/test
BUILD_DIR ?= $(CURDIR)/build
CXXFLAGS += -std=c++17 -O1 -g -Wall -Wextra -Werror -UNDEBUG
CXXFLAGS += -I$(SRC_DIR)/stubs -I$(SRC_DIR)/../inc -I$(VORTEX_HOME)/sw/common
SANITIZERS ?= address,undefined
ifneq ($(strip $(SANITIZERS)),)
CXXFLAGS += -fsanitize=$(SANITIZERS) -fno-omit-frame-pointer
endif

.PHONY: all run
all: $(BUILD_DIR)/contract-full $(BUILD_DIR)/contract-short
$(BUILD_DIR)/contract-full: $(SRC_DIR)/contract.cpp $(wildcard $(SRC_DIR)/../inc/*.h) $(wildcard $(SRC_DIR)/stubs/*.h)
	mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $< -o $@
$(BUILD_DIR)/contract-short: $(SRC_DIR)/contract.cpp $(wildcard $(SRC_DIR)/../inc/*.h) $(wildcard $(SRC_DIR)/stubs/*.h)
	mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -DFORZA_VORTEX_SHORT_MESSAGES $< -o $@
run: all
	$(BUILD_DIR)/contract-full
	$(BUILD_DIR)/contract-short
