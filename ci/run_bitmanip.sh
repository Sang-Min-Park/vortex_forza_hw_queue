#!/usr/bin/env bash
set -euo pipefail

bitmanip_source=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
bitmanip_xlen=${1:-64}
case "$bitmanip_xlen" in
  32|64) ;;
  *) printf 'Usage: %s [32|64] [build-directory]\n' "$0" >&2; exit 1 ;;
esac
bitmanip_build=${2:-"$bitmanip_source/build$bitmanip_xlen-bitmanip"}
mkdir -p "$bitmanip_build"
bitmanip_build=$(cd -- "$bitmanip_build" && pwd)
export THIRD_PARTY_DIR=${THIRD_PARTY_DIR:-"$bitmanip_source/third_party"}
bitmanip_tools=${TOOLDIR:-"$HOME/tools"}
bitmanip_jobs=${BITMANIP_JOBS:-8}
export CONFIGS=''
unset DEBUG PERF SCOPE SAIF
cd "$bitmanip_build"
"$bitmanip_source/configure" --xlen="$bitmanip_xlen" \
  --tooldir="$bitmanip_tools" --prefix="$bitmanip_build/install"
make -j"$bitmanip_jobs" -C tests/unittest/bitmanip run BITMANIP_TEST_ARGS=--negative
