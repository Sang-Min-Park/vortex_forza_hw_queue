#!/usr/bin/env python3
"""Build isolated apps and measure genuine reused large-state capacity cases."""
import argparse
import csv
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time

SOURCE = Path(__file__).resolve().parent
ROOT = SOURCE.parents[3]
BASE_MODES = ("msg_raw", "msg_batch")


def fingerprint(path):
    path = Path(path).resolve()
    return {"path": str(path), "bytes": path.stat().st_size,
            "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}


def fields(line):
    return dict(re.findall(r"(\w+)=([^\s]+)", line))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--build-dir", required=True, type=Path)
    p.add_argument("--cache-policy", choices=("l1_off", "l1_on"), required=True)
    p.add_argument("--stage", choices=("build", "run", "all"), default="all")
    p.add_argument("--compact", action="store_true")
    p.add_argument("--raw", action="store_true", help="include compact direct-backend and six-request batch controls")
    args = p.parse_args()
    build = args.build_dir.resolve()
    app = build / "tests/regression/forza_explore/large_state_active"
    libs = build / "sw/runtime"
    config = "-DVX_CFG_EXT_FORZA_ENABLE -DVX_CFG_EXT_A_ENABLE -DVX_CFG_L2_ENABLE -DVX_CFG_NUM_CORES=2 -DVX_CFG_NUM_WARPS=2 -DVX_CFG_NUM_THREADS=4"
    if args.cache_policy == "l1_off":
        config += " -DVX_CFG_DCACHE_DISABLE"
    report = ROOT / "docs/proposals" / ("forza_explore_large_active_" + args.cache_policy)
    manifest_path = app / "large-state-manifest.json"
    env = os.environ.copy()
    env.pop("DEBUG", None)
    env["CONFIGS"] = config
    env["VORTEX_DRIVER"] = "simx"
    env["LD_LIBRARY_PATH"] = str(libs)
    modes = [(m, 1) for m in BASE_MODES]

    def save(manifest):
        manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
        report.with_suffix(".json").write_text(json.dumps(manifest, indent=2) + "\n")
        rows = []
        for run in manifest["runs"]:
            if "result" in run:
                rows.append({"cache_policy": args.cache_policy, "cohort": run["cohort"],
                             **run["result"], "repeated_full_line_passes": run["input"].get("repeated_full_line_passes", ""),
                             "wall_seconds": run["wall_seconds"], "log": run["log"]})
        if rows:
            with report.with_suffix(".csv").open("w", newline="") as out:
                writer = csv.DictWriter(out, fieldnames=list(rows[0]))
                writer.writeheader()
                writer.writerows(rows)

    if args.stage in ("build", "all"):
        app.mkdir(parents=True, exist_ok=True)
        (app / "logs").mkdir(exist_ok=True)
        shutil.copyfile(SOURCE / "Makefile", app / "Makefile")
        required = [libs / n for n in ("libsimx.so", "libvortex.so", "libvortex-simx.so")]
        required += [build / "sw/kernel/libvortex2.a"]
        for path in required:
            if not path.is_file():
                p.error(f"shared prerequisite not ready: {path}")
        runtime = SOURCE.parent / "runtime"
        sources = list(SOURCE.glob("*.cpp")) + list(SOURCE.glob("*.h"))
        sources += [SOURCE / "Makefile", SOURCE / "README.md", Path(__file__).resolve(), runtime / "vortex.mk"]
        sources += list((runtime / "inc").glob("*.h"))
        manifest = {"started_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
                    "cache_policy": args.cache_policy, "configs": config,
                    "frozen_parent_sources": [fingerprint(SOURCE.parent / "large_state" / n) for n in ("common.h", "main.cpp", "Makefile", "kernel_raw.cpp")],
                    "build_directory": str(build), "source_artifacts": [fingerprint(x) for x in sources],
                    "shared_artifacts": [fingerprint(x) for x in required],
                    "builds": [], "artifacts": [], "runs": []}
        for mode, compact in modes:
            label = mode + "_compact_active"
            command = ["make", "-C", str(app), f"MODE={mode}", f"COMPACT={compact}",
                       "-o", str(libs / "libvortex.so"), "-o", str(build / "sw/kernel/libvortex2.a")]
            log = app / "logs" / f"build-{label}.log"
            with log.open("w") as out:
                subprocess.run(command + ["clean"], env=env, stdout=out, stderr=subprocess.STDOUT, check=True)
                subprocess.run(command + ["-j2"], env=env, stdout=out, stderr=subprocess.STDOUT, check=True)
            dest = app / "artifacts" / label
            dest.mkdir(parents=True, exist_ok=True)
            for original, final in [(f"forza_large_{label}", f"forza_large_{label}"),
                                    ("kernel.elf", "kernel.elf"), ("kernel.dump", "kernel.dump"),
                                    (f"kernel-{label}.vxbin", "kernel.vxbin")]:
                shutil.copy2(app / original, dest / final)
                manifest["artifacts"].append(fingerprint(dest / final))
            manifest["builds"].append({"mode": label, "commands": [command + ["clean"], command + ["-j2"]], "log": str(log)})
            save(manifest)
            print(f"BUILT {args.cache_policy} {label}", flush=True)
    else:
        manifest = json.loads(manifest_path.read_text())

    def verify():
        for item in manifest["source_artifacts"] + manifest["shared_artifacts"] + manifest["artifacts"]:
            if fingerprint(item["path"]) != item:
                raise RuntimeError(f"captured artifact changed: {item['path']}")

    if args.stage == "build":
        verify()
        return
    cases = [("smoke", mode + "_compact_active", 262144, 64, "random") for mode in BASE_MODES]
    cases += [("capacity", mode + "_compact_active", 2097152, 65536, "permute") for mode in BASE_MODES]
    for cohort, label, size, count, pattern in cases:
        verify()
        slug = f"{cohort}-{label}-b{size}-n{count}-{pattern}"
        work = app / "runs" / slug
        work.mkdir(parents=True, exist_ok=True)
        log = work / "run.log"
        binary = app / "artifacts" / label / f"forza_large_{label}"
        command = [str(binary), "-k", str(binary.parent / "kernel.vxbin"), "-b", str(size),
                   "-n", str(count), "-p", pattern, "-s", "17", "-t", "600"]
        print(f"START {args.cache_policy} {slug}", flush=True)
        start = time.monotonic()
        timed_out = False
        with log.open("w") as out:
            try:
                completed = subprocess.run(command, cwd=work, env=env, stdout=out, stderr=subprocess.STDOUT, timeout=610)
                code = completed.returncode
            except subprocess.TimeoutExpired:
                timed_out = True
                code = -1
        lines = log.read_text().splitlines()
        result_lines = [line for line in lines if line.startswith("RESULT ")]
        input_lines = [line for line in lines if line.startswith("INPUT ")]
        passed = code == 0 and bool(result_lines) and any(line.startswith("PASSED:") for line in lines)
        run = {"cohort": cohort, "command": command, "cwd": str(work), "log": str(log),
               "wall_seconds": time.monotonic() - start, "returncode": code, "timed_out": timed_out, "passed": passed}
        if result_lines:
            run["result"] = fields(result_lines[-1])
            run["input"] = fields(input_lines[-1])
        manifest["runs"].append(run)
        verify()
        save(manifest)
        print(f"{'PASS' if passed else 'FAIL'} {args.cache_policy} {slug} wall={run['wall_seconds']:.3f} "
              + (result_lines[-1] if result_lines else "NO RESULT"), flush=True)
        if not passed:
            raise SystemExit(f"Run failed; inspect {log}")
    manifest["completed_utc"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
    save(manifest)
    print(f"COMPLETE {args.cache_policy} {len(manifest['runs'])} validated runs; source/shared/binary hashes unchanged", flush=True)


if __name__ == "__main__":
    main()
