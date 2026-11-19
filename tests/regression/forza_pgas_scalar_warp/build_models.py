#!/usr/bin/env python3
"""Build isolated SimX machines for a physical single-lane warp comparison."""

import argparse
import concurrent.futures
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess


ROOT = Path(__file__).resolve().parents[3]
BASE = ["-DVX_CFG_FORZA_ISA_ENABLE", "-DVX_CFG_FORZA_MMIO_ENABLE",
        "-DVX_CFG_EXT_FORZA_ENABLE", "-DVX_CFG_EXT_A_ENABLE",
        "-DVX_CFG_DCACHE_DISABLE", "-DVX_CFG_L2_ENABLE", "-DVX_CFG_NUM_CORES=2",
        "-DVX_CFG_LMEM_LOG_SIZE=14", "-DVX_CFG_LMEM_NUM_BANKS=4",
        "-DVX_CFG_LSU_PENDING_SIZE=8"]
VARIANTS = {
    "simt2w4t": (2, 4, True),
    "scalar8w1t": (8, 1, True),
    "simt2w4t_port1": (2, 4, False),
    "scalar2w1t": (2, 1, True),
}
THIRD_PARTY = ROOT.parent / "vortex-forza-step12/third_party"


def fingerprint(path):
    path = Path(path).resolve()
    return {"path": str(path), "bytes": path.stat().st_size,
            "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}


def build_machine(name, configure_only):
    warps, threads, vector = VARIANTS[name]
    build = ROOT / ("build64-forza-warp-" + name)
    build.mkdir(exist_ok=True)
    flags = BASE + [f"-DVX_CFG_NUM_WARPS={warps}", f"-DVX_CFG_NUM_THREADS={threads}"]
    if vector:
        flags.append("-DVX_CFG_FORZA_MMIO_VECTOR_ENABLE")
    configs = " ".join(flags)
    env = os.environ.copy()
    env.pop("DEBUG", None)
    env.update(CONFIGS=configs, PERF="1", THIRD_PARTY_DIR=str(THIRD_PARTY))
    commands = [[str(ROOT / "configure"), "--xlen=64", "--osversion=ubuntu/focal",
                 "--tooldir=" + str(ROOT.parent / "tools"), "--prefix=" + str(build / "install")]]
    if not configure_only:
        if (build / "library-freeze.json").exists():
            raise RuntimeError("A frozen machine must not be rebuilt: " + str(build))
        commands += [["make", "-C", "sw/runtime/simx", "-j2"],
                     ["make", "-C", "sw/runtime/stub", "-j2"],
                     ["make", "-C", "sw/kernel", "libvortex2.a", "-j2"]]
    logs = []
    for index, command in enumerate(commands):
        log = build / ("scalar-configure.log" if index == 0 else f"scalar-build-{index}.log")
        if log.exists():
            suffix = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%d%H%M%S%f")
            log = log.with_name(log.stem + "-" + suffix + log.suffix)
        with log.open("w") as output:
            subprocess.run(command, cwd=build, env=env, stdout=output,
                           stderr=subprocess.STDOUT, check=True)
        logs.append(fingerprint(log))
    if configure_only:
        frozen = json.loads((build / "library-freeze.json").read_text())
        assert all(fingerprint(row["path"]) == row for row in frozen)
        print("RECONFIGURED", name, "frozen libraries unchanged", flush=True)
        return

    expanded = subprocess.check_output(
        ["python3", str(ROOT / "ci/gen_config.py"), "--config=" + str(ROOT / "VX_config.toml"),
         "--cflags=" + configs + " -DVX_CFG_XLEN=64"], cwd=build, env=env, text=True)
    libraries = [build / "sw/runtime" / name for name in
                 ["libsimx.so", "libvortex-simx.so", "libvortex.so"]]
    libraries.append(build / "sw/kernel/libvortex2.a")
    frozen = [fingerprint(path) for path in libraries]
    (build / "library-freeze.json").write_text(json.dumps(frozen, indent=2) + "\n")
    symbols = subprocess.check_output(["nm", "-C", str(libraries[0])], text=True)
    assert "ForzaZone::tick_isa" in symbols
    assert not any(name in symbols for name in
                   ["ForzaZone::tick_update", "ForzaZone::tick_record", "ForzaVm::"])
    source_paths = [ROOT / "VX_config.toml", ROOT / "sim/simx/Makefile"]
    source_paths += list((ROOT / "sim/simx/forza").glob("*.h"))
    source_paths += list((ROOT / "sim/simx/forza").glob("*.cpp"))
    source_paths += [ROOT / "sim/simx" / n for n in
                     ["decode.cpp", "core.cpp", "processor.cpp", "sfu_unit.cpp", "types.h",
                      "mem/local_mem.cpp", "mem/local_mem_switch.cpp", "mem/cache.cpp"]]
    source_paths += [ROOT / "sw/common" / n for n in
                     ["forza_abi.h", "forza_mmio_abi.h", "forza_isa_abi.h"]]
    sources = []
    for path in source_paths:
        target = build / "model-source-freeze" / path.relative_to(ROOT)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, target)
        sources += [fingerprint(path), fingerprint(target)]
    document = {
        "created_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "name": name, "build_dir": str(build), "configs": configs,
        "environment": {k: env[k] for k in ["CONFIGS", "PERF", "THIRD_PARTY_DIR"]},
        "commands": commands, "logs": logs, "expanded_config": expanded,
        "expanded_macros": dict(re.findall(r"-D(VX_CFG_\w+)(?:=([^\s]+))?", expanded)),
        "libraries": frozen, "sources": sources, "builder": fingerprint(__file__),
        "resources": {"cores": 2, "warps_per_core": warps, "threads_per_warp": threads,
                      "resident_threads_per_core": warps * threads, "lmem_bytes_per_core": 16384,
                      "lmem_banks_per_core": 4, "lsu_pending": 8,
                      "message_commands_per_core_cycle": threads if vector else 1,
                      "mailbox_data_bytes_per_core": warps * threads * 1024,
                      "physical_frame_bytes": 64, "math_engines": False},
        "scope": "Physical warp-width experiment in SimX; control/context and SIMD resources differ, no equal-area claim.",
    }
    (build / "scalar-machine.json").write_text(json.dumps(document, indent=2) + "\n")
    print("BUILT", name, "frozen4", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--machines", default=",".join(VARIANTS))
    parser.add_argument("--configure-only", action="store_true")
    args = parser.parse_args()
    selected = args.machines.split(",")
    assert all(name in VARIANTS for name in selected)
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
        futures = [pool.submit(build_machine, name, args.configure_only) for name in selected]
        for future in concurrent.futures.as_completed(futures):
            future.result()


if __name__ == "__main__":
    main()
