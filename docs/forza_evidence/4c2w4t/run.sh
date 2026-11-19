#!/usr/bin/env bash
set -euo pipefail
unset DEBUG PERF SCOPE SAIF
export CONFIGS='-DVX_CFG_EXT_FORZA_ENABLE -DVX_CFG_NUM_CORES=4 -DVX_CFG_NUM_WARPS=2 -DVX_CFG_NUM_THREADS=4 -DVX_CFG_NUM_SFU_LANES=4'
export THIRD_PARTY_DIR=/export/nethomes/spark3100/vortex-forza-step12/third_party
../configure --xlen=64 --tooldir=/export/nethomes/spark3100/tools --osversion=ubuntu/focal --prefix="$PWD/install" > diagnostics/configure.log 2>&1
printf '%s\n' "$CONFIGS" > diagnostics/configs.txt
printf '%s\n' 'Building kernel library'
make -j8 -C sw/kernel > diagnostics/build-kernel.log 2>&1
printf '%s\n' 'Building CSR SimX driver'
make -j8 -C sw/runtime stub simx DESTDIR="$PWD/sw/runtime" > diagnostics/build-runtime.log 2>&1
printf '%s\n' 'Building queue and runtime tests'
make -C tests/regression/forza_queue > diagnostics/build-queue.log 2>&1
make -C tests/regression/forza_runtime > diagnostics/build-actor-runtime.log 2>&1
set +e
for test_name in forza_queue forza_runtime; do
  test_opts='-n8'
  if [ "$test_name" = forza_runtime ]; then test_opts='-n4 -p10000'; fi
  printf '%s\n' "Running $test_name $test_opts"
  timeout 140s make -C tests/regression/"$test_name" run-simx OPTS="$test_opts" > diagnostics/"$test_name".log 2>&1
  test_status=$?
  printf '%s\n' "$test_status" > diagnostics/"$test_name".exit
  tail -n 6 diagnostics/"$test_name".log
  printf 'exit=%s\n' "$test_status"
done
