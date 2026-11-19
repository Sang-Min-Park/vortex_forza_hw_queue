#!/usr/bin/env bash
set -euo pipefail
unset DEBUG PERF SCOPE SAIF
export CONFIGS='-DVX_CFG_EXT_FORZA_ENABLE -DVX_CFG_NUM_CORES=2 -DVX_CFG_NUM_WARPS=4 -DVX_CFG_NUM_THREADS=8 -DVX_CFG_NUM_SFU_LANES=8'
export THIRD_PARTY_DIR=/export/nethomes/spark3100/vortex-forza-step12/third_party
../configure --xlen=64 --tooldir=/export/nethomes/spark3100/tools --osversion=ubuntu/focal --prefix="$PWD/install" > diagnostics/configure.log 2>&1
printf '%s\n' "$CONFIGS" > diagnostics/configs.txt
printf '%s\n' 'Building RV64 kernel library'
make -j8 -C sw/kernel > diagnostics/build-kernel.log 2>&1
printf '%s\n' 'Building CSR SimX runtime'
make -j8 -C sw/runtime stub simx DESTDIR="$PWD/sw/runtime" > diagnostics/build-runtime.log 2>&1
printf '%s\n' 'Building queue and actor runtime tests'
make -j8 -C tests/regression/forza_queue > diagnostics/build-queue.log 2>&1
make -j8 -C tests/regression/forza_runtime > diagnostics/build-selector.log 2>&1
experiment_build=$PWD
export LD_LIBRARY_PATH="$experiment_build/sw/runtime:${LD_LIBRARY_PATH:-}"
export VORTEX_DRIVER=simx
set +e
for test_name in forza_queue forza_runtime; do
  test_opts='-n8'
  if [ "$test_name" = forza_runtime ]; then test_opts='-n1 -p10000'; fi
  printf '%s\n' "Running $test_name $test_opts"
  cd "$experiment_build/tests/regression/$test_name"
  /usr/bin/time -f 'wall_seconds=%e exit=%x' -o "$experiment_build/diagnostics/$test_name.time" timeout --kill-after=5s 140s ./"$test_name" $test_opts > "$experiment_build/diagnostics/$test_name.log" 2>&1
  test_status=$?
  printf '%s\n' "$test_status" > "$experiment_build/diagnostics/$test_name.exit"
  tail -n 7 "$experiment_build/diagnostics/$test_name.log"
  cat "$experiment_build/diagnostics/$test_name.time"
  cd "$experiment_build"
done
