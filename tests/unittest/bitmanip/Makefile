ROOT_DIR := $(realpath ../../..)
include $(ROOT_DIR)/config.mk

SRC_DIR := $(VORTEX_HOME)/tests/unittest/bitmanip
SIMX := $(ROOT_DIR)/sim/simx/simx
BITMANIP_TEST_ARGS ?=

.PHONY: all run
all:
	$(MAKE) -C $(ROOT_DIR)/sim/simx

run: all
	python3 $(SRC_DIR)/run.py --xlen $(XLEN) --sim $(SIMX) \
	  --clang $(LLVM_PATH)/bin/clang --output $(CURDIR)/results $(BITMANIP_TEST_ARGS)
