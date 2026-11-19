#!/usr/bin/env python3
"""Measure Mitul-derived graph combining and a preaggregated HW control."""

import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time


CASES = [
    ("regular", 32, 128, 17),
    ("skew", 64, 256, 17),
    ("hub", 64, 256, 17),
    ("regular", 64, 1024, 17),
    ("skew", 64, 1024, 17),
    ("hub", 64, 1024, 17),
    ("skew", 64, 1024, 29),
    ("regular", 256, 4096, 17),
    ("skew", 256, 4096, 17),
    ("hub", 256, 4096, 17),
]
MODES = ("agg_amo", "agg_lmem", "agg_comb", "hw_agg")


def fingerprint(path):
    return {"path": str(path), "bytes": path.stat().st_size,
            "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    args = parser.parse_args()
    build = args.build_dir.resolve()
    app = build / "tests/regression/forza_graph_coalesced"
    reference_app = build / "tests/regression/forza_graph"
    libraries = build / "sw/runtime"
    reference_path = build / "tests/regression/forza_graph/gpu-run-manifest.json"
    reference = json.loads(reference_path.read_text())
    for entry in reference["artifacts"]:
        path = Path(entry["path"])
        if path.parent == libraries and fingerprint(path) != entry:
            parser.error(f"driver no longer matches reference measurements: {path}")
    if reference["cache_policy"] != "l1_off":
        parser.error("the original tree polling ports require the L1-off comparison")
    paths = [app / f"forza_graph_{mode}" for mode in MODES]
    paths += [app / f"kernel-{mode}.{suffix}" for mode in MODES
              for suffix in ("vxbin", "elf", "dump")]
    paths += [libraries / name for name in ("libsimx.so", "libvortex.so", "libvortex-simx.so")]
    paths += [reference_app / "forza_graph_sw"]
    paths += [reference_app / f"kernel-sw.{suffix}" for suffix in ("vxbin", "elf", "dump")]
    for path in paths:
        if not path.is_file():
            parser.error(f"missing built artifact: {path}")
    manifest = {"started_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
                "scope": "one-shot weighted graph sum; Mitul histogram algorithm adaptations",
                "cache_policy": "l1_off", "configs": reference["configs"],
                "build_directory": str(build), "reference_manifest": fingerprint(reference_path),
                "artifacts": [fingerprint(path) for path in paths], "runs": []}
    env = os.environ.copy()
    env.pop("DEBUG", None)
    env["VORTEX_DRIVER"] = "simx"
    env["LD_LIBRARY_PATH"] = str(libraries)
    for shape, vertices, edges, seed in CASES:
        for mode in (*MODES, "software"):
            binary = reference_app / "forza_graph_sw" if mode == "software" else app / f"forza_graph_{mode}"
            kernel = reference_app / "kernel-sw.vxbin" if mode == "software" else app / f"kernel-{mode}.vxbin"
            command = [str(binary), "-k", str(kernel),
                       "-g", shape, "-v", str(vertices), "-e", str(edges), "-s", str(seed)]
            log = app / f"validation-{mode}-{shape}-v{vertices}-e{edges}-s{seed}.log"
            start = time.monotonic()
            with log.open("w") as output:
                result = subprocess.run(command, cwd=app, env=env, stdout=output,
                                        stderr=subprocess.STDOUT, timeout=190)
            lines = log.read_text().splitlines()
            success = result.returncode == 0 and any(
                line.startswith("PASSED: exact CPU SpMV reference") for line in lines)
            manifest["runs"].append({"command": command, "log": str(log),
                                     "returncode": result.returncode,
                                     "wall_seconds": time.monotonic() - start,
                                     "passed": success})
            (app / "coalesced-run-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
            for line in lines:
                if line.startswith(("RESULT ", "FAILED:")):
                    print(line, flush=True)
            if not success:
                raise SystemExit(f"Benchmark failed; inspect {log}")
    for old in manifest["artifacts"]:
        if fingerprint(Path(old["path"])) != old:
            raise SystemExit(f"Measured artifact changed during runs: {old['path']}")
    print(f"PASSED: {len(manifest['runs'])} matched runs; artifacts unchanged", flush=True)


if __name__ == "__main__":
    main()
