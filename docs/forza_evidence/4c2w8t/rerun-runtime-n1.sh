#!/usr/bin/env bash
set -uo pipefail
cd /export/nethomes/spark3100/vortex-forza-hwqueue/build64-forza-csr-generalize-4c2w8t
export CONFIGS='-DVX_CFG_EXT_FORZA_ENABLE -DVX_CFG_NUM_CORES=4 -DVX_CFG_NUM_WARPS=2 -DVX_CFG_NUM_THREADS=8 -DVX_CFG_NUM_SFU_LANES=8'
export THIRD_PARTY_DIR=/export/nethomes/spark3100/vortex-forza-step12/third_party
export CCACHE_DISABLE=1
unset DEBUG PERF
/usr/bin/time -f 'wall_seconds=%e user_seconds=%U system_seconds=%S exit=%x' -o diagnostics/run-selector-n1.time timeout --signal=TERM --kill-after=5s 140s make -j8 -C tests/regression/forza_runtime run-simx OPTS='-n1 -p10000' > diagnostics/run-selector-n1.log 2>&1
status=$?
printf '%s\n' "$status" > diagnostics/run-selector-n1.exit
exit "$status"
