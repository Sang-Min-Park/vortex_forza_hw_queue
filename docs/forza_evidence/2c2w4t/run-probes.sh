#!/usr/bin/env bash
set -euo pipefail
unset DEBUG PERF SCOPE SAIF
export CONFIGS='-DVX_CFG_EXT_FORZA_ENABLE -DVX_CFG_NUM_CORES=2 -DVX_CFG_NUM_WARPS=2 -DVX_CFG_NUM_THREADS=4 -DVX_CFG_NUM_SFU_LANES=4'
export THIRD_PARTY_DIR=/export/nethomes/spark3100/vortex-forza-step12/third_party
../configure --xlen=64 --tooldir=/export/nethomes/spark3100/tools --osversion=ubuntu/focal --prefix="$PWD/install" > diagnostics/configure-probes.log 2>&1
make -C tests/regression/forza_csr_generalize > diagnostics/build-probes.log 2>&1
experiment_build=$PWD
export LD_LIBRARY_PATH="$experiment_build/sw/runtime:${LD_LIBRARY_PATH:-}"
export VORTEX_DRIVER=simx
cd tests/regression/forza_csr_generalize
set +e
for probe_args in '1 physical' '1 logical' '2 physical' '2 logical' '4 physical' '4 logical'; do
  read -r waves pe_mode <<< "$probe_args"
  probe_name="probe-w${waves}-${pe_mode}"
  timeout --kill-after=5s 140s ./forza_csr_generalize -w "$waves" -m "$pe_mode" -n4 > "$experiment_build/diagnostics/$probe_name.log" 2>&1
  probe_status=$?
  printf '%s\n' "$probe_status" > "$experiment_build/diagnostics/$probe_name.exit"
  printf '%s exit=%s\n' "$probe_name" "$probe_status"
  tail -n 2 "$experiment_build/diagnostics/$probe_name.log"
done
