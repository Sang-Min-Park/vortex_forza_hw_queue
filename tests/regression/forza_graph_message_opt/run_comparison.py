#!/usr/bin/env python3
"""Measure optimized message kernels and matching software/GPU controls."""

import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time


CASES = [
    ("regular", 32, 128, 17), ("skew", 64, 256, 17), ("hub", 64, 256, 17),
    ("regular", 64, 1024, 17), ("skew", 64, 1024, 17), ("hub", 64, 1024, 17),
    ("skew", 64, 1024, 29), ("regular", 256, 4096, 17),
    ("skew", 256, 4096, 17), ("hub", 256, 4096, 17),
]
MODES = ("msg_local", "msg_private", "msg_warp", "sw_warp", "gpu_warp")


def fingerprint(path):
    return {"path": str(path), "bytes": path.stat().st_size,
            "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=Path)
    args = parser.parse_args()
    build = args.build_dir.resolve()
    app = build / "tests/regression/forza_graph_message_opt"
    libraries = build / "sw/runtime"
    reference_path = build / "tests/regression/forza_graph_core_hw/core-hw-run-manifest.json"
    reference = json.loads(reference_path.read_text())
    for entry in reference["artifacts"]:
        path = Path(entry["path"])
        if path.parent == libraries and fingerprint(path) != entry:
            parser.error(f"driver differs from measured references: {path}")
    required = [app / f"forza_graph_{mode}" for mode in MODES]
    required += [app / f"kernel-{mode}.{suffix}" for mode in MODES
                 for suffix in ("vxbin", "elf", "dump")]
    required += [libraries / name for name in ("libsimx.so", "libvortex.so", "libvortex-simx.so")]
    proof = app / "compiled-fold-proof.txt"
    if proof.exists():
        required.append(proof)
    for path in required:
        if not path.is_file():
            parser.error(f"missing artifact: {path}")
    manifest = {"started_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
                "scope": "loop hoisting/local bypass/warp work distribution/private partial experiments",
                "configs": reference["configs"], "cache_policy": reference["cache_policy"],
                "reference_manifest": fingerprint(reference_path),
                "artifacts": [fingerprint(path) for path in required], "runs": []}
    env = os.environ.copy()
    env.pop("DEBUG", None)
    env["LD_LIBRARY_PATH"] = str(libraries)
    env["VORTEX_DRIVER"] = "simx"
    for shape, vertices, edges, seed in CASES:
        for mode in MODES:
            command = [str(app / f"forza_graph_{mode}"), "-k", str(app / f"kernel-{mode}.vxbin"),
                       "-g", shape, "-v", str(vertices), "-e", str(edges), "-s", str(seed)]
            log = app / f"validation-{mode}-{shape}-v{vertices}-e{edges}-s{seed}.log"
            start = time.monotonic()
            with log.open("w") as output:
                result = subprocess.run(command, cwd=app, env=env, stdout=output,
                                        stderr=subprocess.STDOUT, timeout=190)
            lines = log.read_text().splitlines()
            passed = result.returncode == 0 and any(
                line.startswith("PASSED: exact CPU SpMV reference") for line in lines)
            manifest["runs"].append({"command": command, "log": str(log),
                                     "returncode": result.returncode,
                                     "wall_seconds": time.monotonic() - start,
                                     "passed": passed})
            (app / "message-opt-run-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
            for line in lines:
                if line.startswith(("RESULT ", "FAILED:")):
                    print(line, flush=True)
            if not passed:
                raise SystemExit(f"Benchmark failed: {log}")
    for old in manifest["artifacts"]:
        if fingerprint(Path(old["path"])) != old:
            raise SystemExit(f"Artifact changed during measurement: {old['path']}")
    print(f"PASSED: {len(manifest['runs'])} runs; artifacts unchanged", flush=True)


if __name__ == "__main__":
    main()
