#!/usr/bin/env bash
set -uo pipefail
cd /export/nethomes/spark3100/vortex-forza-hwqueue/build64-forza-csr-generalize-4c2w8t
sha256sum tests/regression/forza_runtime/kernel.elf tests/regression/forza_runtime/kernel.vxbin > diagnostics/long-device-before.sha256
export LD_LIBRARY_PATH="$PWD/sw/runtime:${LD_LIBRARY_PATH:-}"
export VORTEX_DRIVER=simx
unset DEBUG PERF
cd tests/regression/forza_runtime
/usr/bin/time -f 'wall_seconds=%e user_seconds=%U system_seconds=%S exit=%x' -o ../../../diagnostics/run-selector-n1-long.time timeout --signal=TERM --kill-after=5s 315s ./forza_runtime_long -n1 -p10000 > ../../../diagnostics/run-selector-n1-long.log 2>&1
status=$?
printf '%s\n' "$status" > ../../../diagnostics/run-selector-n1-long.exit
cd ../../..
sha256sum tests/regression/forza_runtime/kernel.elf tests/regression/forza_runtime/kernel.vxbin > diagnostics/long-device-after.sha256
cmp diagnostics/long-device-before.sha256 diagnostics/long-device-after.sha256 > diagnostics/long-device-unchanged.txt 2>&1
printf 'run_exit=%s device_hash_match=%s\n' "$status" "$?"
exit "$status"
