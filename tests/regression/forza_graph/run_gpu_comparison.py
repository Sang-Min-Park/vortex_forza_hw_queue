#!/usr/bin/env python3
"""Run prebuilt, matched actor and direct-GPU graph kernels."""

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
]
MODES = ("hw", "gpu", "gpu_edge")


def fingerprint(path):
    return {"path": str(path), "bytes": path.stat().st_size,
            "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--cache-policy", choices=("l1_off", "l1_on"), required=True)
    parser.add_argument("--configs", required=True)
    parser.add_argument("--large", action="store_true", help="also test V256/E4096 graphs")
    args = parser.parse_args()
    build = args.build_dir.resolve()
    app = build / "tests/regression/forza_graph"
    libraries = build / "sw/runtime"
    required = [app / f"forza_graph_{mode}" for mode in MODES]
    required += [app / f"kernel-{mode}.{suffix}" for mode in MODES
                 for suffix in ("vxbin", "elf", "dump")]
    required += [libraries / name for name in ("libsimx.so", "libvortex.so", "libvortex-simx.so")]
    for path in required:
        if not path.is_file():
            parser.error(f"missing built artifact: {path}")
    manifest = {"started_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
                "cache_policy": args.cache_policy, "configs": args.configs,
                "build_directory": str(build),
                "artifacts": [fingerprint(path) for path in required], "runs": []}
    env = os.environ.copy()
    env.pop("DEBUG", None)
    env["VORTEX_DRIVER"] = "simx"
    env["LD_LIBRARY_PATH"] = str(libraries)
    cases = CASES + ([(shape, 256, 4096, 17) for shape in ("regular", "skew", "hub")]
                     if args.large else [])
    for shape, vertices, edges, seed in cases:
        for mode in MODES:
            command = [str(app / f"forza_graph_{mode}"), "-k", f"kernel-{mode}.vxbin",
                       "-g", shape, "-v", str(vertices), "-e", str(edges), "-s", str(seed)]
            log = app / f"validation-direct-{mode}-{shape}-v{vertices}-e{edges}-s{seed}.log"
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
            (app / "gpu-run-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
            for line in lines:
                if line.startswith(("RESULT ", "FAILED:")):
                    print(args.cache_policy, line, flush=True)
            if not success:
                raise SystemExit(f"Benchmark failed; inspect {log}")
    for old in manifest["artifacts"]:
        if fingerprint(Path(old["path"])) != old:
            raise SystemExit(f"Measured artifact changed during runs: {old['path']}")
    print(f"PASSED: {len(manifest['runs'])} matched runs; artifacts unchanged", flush=True)


if __name__ == "__main__":
    main()
