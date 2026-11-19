#!/usr/bin/env bash
set -euo pipefail
cd /export/nethomes/spark3100/vortex-forza-hwqueue/build64-forza-csr-generalize-4c2w8t
export CONFIGS='-DVX_CFG_EXT_FORZA_ENABLE -DVX_CFG_NUM_CORES=4 -DVX_CFG_NUM_WARPS=2 -DVX_CFG_NUM_THREADS=8 -DVX_CFG_NUM_SFU_LANES=8'
export THIRD_PARTY_DIR=/export/nethomes/spark3100/vortex-forza-step12/third_party
export CCACHE_DISABLE=1
unset DEBUG PERF
printf '%s\n' "$CONFIGS" > diagnostics/config.txt
../configure --xlen=64 --tooldir=/export/nethomes/spark3100/tools --osversion=ubuntu/focal --prefix=/export/nethomes/spark3100/vortex-forza-hwqueue/build64-forza-csr-generalize-4c2w8t/install > diagnostics/configure.log 2>&1
make -j8 -C sw/kernel > diagnostics/build-kernel.log 2>&1
make -j8 -C sw/runtime stub simx > diagnostics/build-runtime.log 2>&1
make -j8 -C tests/regression/forza_queue > diagnostics/build-queue.log 2>&1
make -j8 -C tests/regression/forza_runtime > diagnostics/build-selector.log 2>&1
set +e
timeout --signal=TERM --kill-after=5s 140s make -C tests/regression/forza_queue run-simx OPTS='-n8' > diagnostics/run-queue.log 2>&1
queue_status=$?
printf '%s\n' "$queue_status" > diagnostics/run-queue.exit
timeout --signal=TERM --kill-after=5s 140s make -C tests/regression/forza_runtime run-simx OPTS='-n4 -p10000' > diagnostics/run-selector.log 2>&1
selector_status=$?
printf '%s\n' "$selector_status" > diagnostics/run-selector.exit
printf 'queue=%s selector=%s\n' "$queue_status" "$selector_status"
