#!/usr/bin/env bash
set -euo pipefail
unset DEBUG PERF SCOPE SAIF
export CONFIGS='-DVX_CFG_EXT_FORZA_ENABLE -DVX_CFG_NUM_CORES=4 -DVX_CFG_NUM_WARPS=2 -DVX_CFG_NUM_THREADS=8 -DVX_CFG_NUM_SFU_LANES=8'
export THIRD_PARTY_DIR=/export/nethomes/spark3100/vortex-forza-step12/third_party
experiment_build=$PWD
experiment_source=/export/nethomes/spark3100/vortex-forza-hwqueue/tests/regression/forza_runtime
../configure --xlen=64 --tooldir=/export/nethomes/spark3100/tools --osversion=ubuntu/focal --prefix="$PWD/install" > diagnostics/configure-phase.log 2>&1
mkdir -p tests/regression/forza_runtime_phase
cp tests/regression/forza_runtime/Makefile tests/regression/forza_runtime_phase/Makefile
make -j4 -C tests/regression/forza_runtime_phase PROJECT=forza_runtime_phase SRC_DIR="$experiment_source" SRCS="$experiment_build/diagnostics/main-long.cpp" VX_SRCS="$experiment_build/diagnostics/kernel-phases.cpp" > diagnostics/build-phase.log 2>&1
export LD_LIBRARY_PATH="$experiment_build/sw/runtime:${LD_LIBRARY_PATH:-}"
export VORTEX_DRIVER=simx
cd tests/regression/forza_runtime_phase
set +e
timeout --kill-after=5s 90s ./forza_runtime_phase -n1 -p10000 > "$experiment_build/diagnostics/run-phase.log" 2>&1
probe_status=$?
printf '%s\n' "$probe_status" > "$experiment_build/diagnostics/run-phase.exit"
tail -n 12 "$experiment_build/diagnostics/run-phase.log"
