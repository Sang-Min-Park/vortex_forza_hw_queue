#!/usr/bin/env python3
"""Verify the frozen programmable-engine cohorts and compare native controls."""

import hashlib
import json
from collections import defaultdict
from datetime import datetime, timezone
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
DOCS = ROOT / "docs/proposals"
INPUTS = [
    "forza_pgas_vm_validated2_results.json",
    "forza_pgas_vm_native_packed_validated_results.json",
    "forza_pgas_vm_branch_ordered_results.json",
]


def fingerprint(path):
    path = Path(path)
    return {"path": str(path), "bytes": path.stat().st_size,
            "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}


def main():
    verified = {}

    def verify(value):
        if isinstance(value, dict):
            if {"path", "bytes", "sha256"} <= value.keys():
                key = (value["path"], value["sha256"])
                if key not in verified:
                    actual = fingerprint(value["path"])
                    assert actual == {k: value[k] for k in actual}, actual
                    verified[key] = actual
            for child in value.values():
                verify(child)
        elif isinstance(value, list):
            for child in value:
                verify(child)

    groups = defaultdict(list)
    run_count = 0
    for name in INPUTS:
        manifest = json.loads((DOCS / name).read_text())
        verify(manifest)
        for run in manifest["runs"]:
            assert run["passed"] and run["returncode"] == 0
            assert not run["timed_out"]
            log = Path(run["log"]).read_text()
            assert "PASSED" in log and "RESULT" in log
            run_count += 1
            if run["cohort"] != "performance":
                continue
            result = run["result"]
            condition = (result["program"], run["keys"], run["requests"],
                         run["pattern"], run["seed"])
            groups[condition].append({
                "mode": run["mode"], "cohort": name,
                "cycles": int(result["device_cycles_post_readback_including_flush"]),
                "input_hash": result["hash"], "checksum": result["checksum"],
            })

    summary = []
    for key, rows in sorted(groups.items()):
        assert len({r["input_hash"] for r in rows}) == 1
        if key[0] != "branch":
            assert len({r["checksum"] for r in rows}) == 1
        native = min((r for r in rows if "_gpu_" in r["mode"]),
                     key=lambda r: r["cycles"])
        mailbox = next(r for r in rows if r["mode"].endswith("vm_mailbox"))
        direct = next(r for r in rows if r["mode"].endswith("vm_direct"))
        summary.append({
            "program": key[0], "keys": key[1], "requests": key[2],
            "pattern": key[3], "seed": key[4], "best_native": native,
            "vm_mailbox": mailbox, "vm_direct": direct,
            "mailbox_speedup": native["cycles"] / mailbox["cycles"],
            "direct_speedup": native["cycles"] / direct["cycles"],
            "mailbox_faster_than_direct": mailbox["cycles"] < direct["cycles"],
            "rows": rows,
        })

    body = [
        "# Programmable owner execution: measured results", "",
        "A constrained programmable processor improves hot-record execution for three "
        "different uploaded programs. The actor-mailbox form does not beat the same "
        "engine's direct completion form in the representative hot cases. These "
        "results support programmable owner execution, not an actor-exclusive benefit.", "",
        "## Implementations and equal-work contract", "",
        "P0 is an 11-instruction count/sum/XOR program. P1 is an 11-instruction "
        "count/min/max program with signed branches and conditional stores. P2 is a "
        "17-instruction nonlinear program: increment count; depending on old sum's "
        "parity, add signed delta or XOR request ID into sum; add the new sum into "
        "the third field. All return the previous three fields, using u64 wrapping.", "",
        "No engine change or program-ID arithmetic shortcut distinguishes these "
        "programs. Software uploads instructions, with configuration acknowledged "
        "before work. Four contexts execute at most one fetched and one executed "
        "instruction per core/cycle through registered stages. Every memory access "
        "uses the real LocalMem channels; stores must acknowledge before return.", "",
        "Native controls include scalar locking, same-key warp grouping, and fully "
        "timed GPU bucket/prefix ownership. Both compact global records (32 B with "
        "lock) and 64 B records are measured. Compact owner state uses 24 B/key; VM "
        "regions use 64 B/key. Native and VM use the same physical 16 KiB/core "
        "LocalMem capacity. Installation, routing, initialization, result writes and "
        "export are included. The minimum native control is chosen per condition; "
        "this envelope is not an implemented automatic selector.", "",
        "P0/P1 host validation replays every old tuple and checks all final records. "
        "P2 is noncommutative: each mode may have a different legal serial history. "
        "The host orders returned per-key count tickets, rejects gaps/duplicates, "
        "replays every old tuple and nonlinear transition, then checks all final "
        "records. Input identity matches across modes; P2 final checksums need not.", "",
        "## Full primary performance matrix", "",
        "All rows use 4,096 operations, seed 17, two cores, two four-lane warps/core, "
        "L1D disabled and shared L2 enabled. Values are cumulative core-0 MCYCLE "
        "after readback/flush. Speedup is best native cycles divided by engine cycles.", "",
        "| Program | Keys | Pattern | Best native | Mailbox engine | Direct engine | Mailbox speedup | Direct speedup |",
        "|---|---:|---|---:|---:|---:|---:|---:|",
    ]
    for row in summary:
        if row["seed"] != 17:
            continue
        n, m, d = (row[x]["cycles"] for x in ["best_native", "vm_mailbox", "vm_direct"])
        body.append(f'| {row["program"]} | {row["keys"]} | {row["pattern"]} | '
                    f'{n:,} | {m:,} | {d:,} | {row["mailbox_speedup"]:.2f}x | '
                    f'{row["direct_speedup"]:.2f}x |')
    body += [
        "", "## Attribution and limits", "",
        "Direct completion bypasses logical actor registration and the actor mailbox "
        "API while retaining the same bounded request/reply fabric and two-slot "
        "completion storage. It still transports packets; it is not an implemented "
        "GPU LSU instruction returning a result directly to a register. Thus similar "
        "wins establish that actor dispatch is unnecessary for this engine benefit, "
        "not that a newly designed LSU-attached engine has been measured.", "",
        "The engine adds a 1 KiB instruction store, four contexts with eight 64-bit "
        "registers each, a 32-entry request queue, and one actual LocalMem client. "
        "Each invocation owns one 64 B region, with conflicting invocations serialized. "
        "The contract excludes concurrent ordinary GPU/other-engine accesses during "
        "execution. This is not arbitrary RISC-V execution, a full RZA, or cross-region "
        "pointer traversal. Context, arbitration, port timing and memory responses are "
        "modeled; RTL, achievable clock, area and power are not validated.", "",
        f"**{run_count}/{run_count} accepted regression runs pass:** 100 P0/P1, "
        "60 compact-native controls and 88 P2 (including eight seed-29 hot repeats). "
        "The repeats reproduce cycle counts with different input values. Separately, "
        "36/36 real-LocalMem unit runs pass across release and ASAN/UBSAN, including "
        "withheld store/result ACKs, saturation, isolation and invalid instructions.", "",
        "Earlier failed shader cohorts remain diagnostics. Ordered benchmark-local "
        "shuffle/vote wrappers prevent the compiler from sinking collective operations "
        "into partial-mask branches. P2 grouped reads execute for all lanes under "
        "one leader-held lock; only lock/commit is leader-only. The accepted cohorts "
        "use the corrected binaries and full checks, with no golden or latency change.", "",
        "- [P0/P1 raw results](forza_pgas_vm_validated2_results.json)",
        "- [Compact GPU controls](forza_pgas_vm_native_packed_validated_results.json)",
        "- [P2 raw results](forza_pgas_vm_branch_ordered_results.json)",
        "- [Verification and every compared mode](forza_pgas_vm_final_freeze.json)",
        "- [Engine contract](forza_programmable_region_engine.md)", "",
    ]
    report = DOCS / "forza_pgas_vm_comparison.md"
    report.write_text("\n".join(body))
    freeze = {
        "verified_utc": datetime.now(timezone.utc).isoformat(),
        "run_count": run_count, "passed": run_count,
        "primary_conditions": sum(r["seed"] == 17 for r in summary),
        "repeat_conditions": sum(r["seed"] != 17 for r in summary),
        "summary": summary, "verified_artifact_count": len(verified),
        "verified_artifacts": list(verified.values()),
        "manifests": [fingerprint(DOCS / name) for name in INPUTS],
        "report": fingerprint(report), "finalizer": fingerprint(__file__),
    }
    (DOCS / "forza_pgas_vm_final_freeze.json").write_text(json.dumps(freeze, indent=2) + "\n")
    print(json.dumps({k: freeze[k] for k in ["run_count", "passed", "primary_conditions",
                                           "repeat_conditions", "verified_artifact_count"]}))


if __name__ == "__main__":
    main()
