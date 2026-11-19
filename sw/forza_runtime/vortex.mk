FORZA_VORTEX_RUNTIME_ROOT := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))

# Inlining the runtime can exceed the compiler's default SIMT conversion budget.
VX_CFLAGS += -std=c++17 -I$(FORZA_VORTEX_RUNTIME_ROOT)/inc
VX_CFLAGS += -DFORZA_VORTEX_BACKEND -DFORZA_VORTEX_DIVERGENCE_OK
VX_CFLAGS += -mllvm -vortex-divergence-max-bbs=512
VX_HDRS += $(wildcard $(FORZA_VORTEX_RUNTIME_ROOT)/inc/*.h)
VX_HDRS += $(FORZA_VORTEX_RUNTIME_ROOT)/vortex.mk
