#!/usr/bin/env bash
set -euo pipefail

forza_source=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
forza_build=${1:-"$forza_source/build64-forza-csr"}
mkdir -p "$forza_build"
forza_build=$(cd -- "$forza_build" && pwd)
export FORZA_RUNTIME_HOME=${FORZA_RUNTIME_HOME:-"$forza_source/sw/forza_runtime"}
export THIRD_PARTY_DIR=${THIRD_PARTY_DIR:-"$forza_source/third_party"}
forza_tools=${TOOLDIR:-"$HOME/tools"}
forza_cores=${FORZA_CORES:-2}
forza_warps=${FORZA_WARPS:-2}
forza_threads=${FORZA_THREADS:-4}
forza_jobs=${FORZA_JOBS:-8}

if [[ ! -f "$FORZA_RUNTIME_HOME/vortex.mk" ]]; then
  printf 'Missing runtime adapter: %s\n' "$FORZA_RUNTIME_HOME" >&2
  exit 1
fi
if [[ ! -f "$THIRD_PARTY_DIR/softfloat/build/Linux-x86_64-GCC/softfloat.a" || ! -f "$THIRD_PARTY_DIR/ramulator/libramulator.so" ]]; then
  printf 'Build SoftFloat/Ramulator or set THIRD_PARTY_DIR to their existing build.\n' >&2
  exit 1
fi

export CONFIGS="-DVX_CFG_EXT_FORZA_ENABLE -DVX_CFG_NUM_CLUSTERS=1 -DVX_CFG_NUM_CORES=$forza_cores -DVX_CFG_NUM_WARPS=$forza_warps -DVX_CFG_NUM_THREADS=$forza_threads -DVX_CFG_NUM_SFU_LANES=$forza_threads"
unset DEBUG PERF SCOPE SAIF
cd "$forza_build"
mkdir -p diagnostics
printf '%s\n' "$CONFIGS" > diagnostics/configs.txt
"$forza_source/configure" --xlen=64 --tooldir="$forza_tools" --prefix="$forza_build/install"
make -j"$forza_jobs" -C sw/kernel
make -j"$forza_jobs" -C sw/runtime stub simx DESTDIR="$forza_build/sw/runtime"
for forza_test in forza_queue forza_runtime forza_csr_generalize; do
  make -j"$forza_jobs" -C "tests/regression/$forza_test"
done

export LD_LIBRARY_PATH="$forza_build/sw/runtime:${LD_LIBRARY_PATH:-}"
export VORTEX_DRIVER=simx
make -j"$forza_jobs" -C tests/unittest/bitmanip run BITMANIP_TEST_ARGS=--negative \
  2>&1 | tee "$forza_build/diagnostics/bitmanip.log"
forza_run() {
  local forza_test=$1
  local forza_log=$2
  shift 2
  (
    cd "$forza_build/tests/regression/$forza_test"
    timeout --kill-after=5s 140s "./$forza_test" "$@"
  ) 2>&1 | tee "$forza_build/diagnostics/$forza_log.log"
}

forza_run forza_queue queue -n8
forza_run forza_runtime runtime -n4 -p10000
forza_run forza_csr_generalize reuse-physical -w2 -m physical -n4
