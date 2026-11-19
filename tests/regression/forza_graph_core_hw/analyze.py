#!/usr/bin/env python3
"""Compare two core-combining HW paths with frozen same-driver references.

The primary metric includes host result readback/cache flush. Earlier measured
artifacts remain untouched; only this experiment's CSV and JSON are written.
"""

import argparse
import csv
import hashlib
import importlib.util
import json
from pathlib import Path
import statistics
import sys


ROOT = Path(__file__).resolve().parents[3]
REFERENCE_SOURCE = ROOT / "tests/regression/forza_graph_coalesced/analyze.py"
SPEC = importlib.util.spec_from_file_location("coalesced_reference_analysis", REFERENCE_SOURCE)
REF = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(REF)
NEW_MODES = ("core_hw", "core_hw_fixed")
MODES = ("agg_comb", *NEW_MODES, "hw_agg", "hardware", "gpu_vertex")
PRIMARY = REF.PRIMARY


def parse_core_log(path):
    lines = path.read_text().splitlines()
    if any(line.startswith("FAILED:") for line in lines):
        raise ValueError("failed correctness record")
    graph = REF.fields(REF.one_line(lines, "GRAPH mode="))
    memory = REF.fields(REF.one_line(lines, "MEMORY "))
    result = REF.fields(REF.one_line(lines, "RESULT "))
    passed = REF.fields(REF.one_line(lines, "PASSED: exact CPU SpMV reference"))
    if result["mode"] not in NEW_MODES:
        raise ValueError("expected core_hw or core_hw_fixed")
    for header, output in (("mode", "mode"), ("shape", "graph"), ("hash", "graph_hash"),
                           ("vertices", "vertices"), ("edges", "edges"), ("seed", "seed")):
        if graph[header] != result[output]:
            raise ValueError(f"GRAPH/RESULT disagree: {header}")
    for key in REF.FOOTPRINTS:
        if memory[key] != result[key]:
            raise ValueError(f"MEMORY/RESULT disagree: {key}")
    record = {**graph, **memory, **result, "cache_policy": "l1_off_shared_l2"}
    numeric = ("vertices", "edges", "seed", "actors", "max_outdegree", "max_indegree",
               "cross_core_edges", *REF.PHASE_METRICS, *REF.COUNTERS, *REF.REGIONS,
               *REF.AGGREGATION, *REF.FOOTPRINTS, "local_capacity_per_core",
               "owned_stride", "one_shot_host_reset_excluded")
    for key in numeric:
        record[key] = int(record[key])
        if record[key] < 0:
            raise ValueError(f"negative {key}")
    record.pop("shape")
    record.pop("hash")
    if record["counter_status"] != "0/0/0" or record["cycle_counter_status"] != "0":
        raise ValueError("performance counter query failed")
    correct = (int(passed["updates"]) == record["edges"]
               and int(passed["published_vectors"]) == record["vectors_published"]
               and int(passed["reduced_vectors"]) == record["vectors_reduced"]
               and record["vectors_published"] == record["vectors_reduced"] == 2 * record["actors"]
               and record["chunks_sent"] == record["chunks_received"]
               == record["vectors_published"] * ((record["owned_stride"] + 5) // 6)
               and record["owned_stride"] == (record["vertices"] + record["actors"] - 1) // record["actors"]
               and record["local_bytes_per_core"] == record["actors"] * record["owned_stride"] * 8
               and record["local_bytes_per_core"] <= record["local_capacity_per_core"]
               and record["global_vectors_bytes"] == record["global_active_vectors_bytes"] == 0
               and record["tree_flags_bytes"] == 0 and record["one_shot_host_reset_excluded"] == 1)
    if not correct:
        raise ValueError("incorrect update/vector/chunk/footprint record")
    span = record["phase_span_cycles"]
    if not (0 < span and 0 <= record["data_span_cycles"] <= span
            and record["max_actor_cycles"] <= span
            <= record["max_actor_cycles"] + record["start_skew_cycles"]
            and record[PRIMARY] >= span):
        raise ValueError("inconsistent device timestamps")
    record["correctness"] = passed
    record["completion"] = "selector_done" if record["mode"] == "core_hw" else "known_unique_chunks"
    record["prescribed_done_messages"] = record["actors"] ** 2 if record["mode"] == "core_hw" else 0
    try:
        record["source_log"] = str(path.resolve().relative_to(ROOT))
    except ValueError:
        record["source_log"] = str(path.resolve())
    return record


def summarize(records):
    record = dict(records[0])
    metrics = (*REF.PHASE_METRICS, *REF.COUNTERS,
               *(REF.REGIONS if "max_fold_cycles" in record else ("retries",)))
    static = (*REF.CASE_FIELDS, *REF.AGGREGATION, *REF.FOOTPRINTS) if "vectors_published" in record else REF.CASE_FIELDS
    for key in static:
        if any(item[key] != records[0][key] for item in records):
            raise ValueError(f"replicate metadata differs: {key}")
    for key in metrics:
        value = statistics.median(item[key] for item in records)
        record[key] = int(value) if value == int(value) else value
    record["logs"] = [item["source_log"] for item in records]
    record["replicates"] = len(records)
    record["identical_replicates"] = all(
        all(item[key] == records[0][key] for key in metrics) for item in records)
    return record


def compare(records, expected_cases):
    groups = {}
    for record in records:
        groups.setdefault((REF.case_key(record), record["mode"]), []).append(record)
    groups = {key: summarize(items) for key, items in groups.items()}
    cases = sorted({key for key, _ in groups}, key=lambda key: (
        key[1], key[2], REF.SHAPES.get(key[0], 99), key[3]))
    problems = []
    if len(cases) != expected_cases:
        problems.append(f"expected {expected_cases} graph cases; found {len(cases)}")
    rows = []
    for case in cases:
        missing = [mode for mode in MODES if (case, mode) not in groups]
        if missing:
            problems.append(f"missing {','.join(missing)} for {case}")
            continue
        gpu = groups[(case, "gpu_vertex")]
        for mode in MODES:
            record = groups[(case, mode)]
            for key in ("max_outdegree", "max_indegree", "cross_core_edges"):
                if record[key] != gpu[key]:
                    raise ValueError(f"graph statistics differ: {case} {key}")
            row = {key: record[key] for key in REF.CASE_FIELDS}
            row.update({"mode": mode, "cache_policy": record["cache_policy"],
                        "counter_window": "post_readback_including_flush"})
            for key in (*REF.PHASE_METRICS, *REF.COUNTERS):
                row[key.replace("whole_kernel", "post_readback_including_flush")] = record[key]
            for key in (*REF.REGIONS, *REF.AGGREGATION, *REF.FOOTPRINTS):
                row[key] = record.get(key)
            row["dram_requests_post_readback_including_flush"] = (
                record["dram_reads_whole_kernel"] + record["dram_writes_whole_kernel"])
            row["completion"] = {
                "agg_comb": "tree_barrier", "hw_agg": "selector_done", "hardware": "actor_done",
                "gpu_vertex": "global_barrier", "core_hw": "selector_done",
                "core_hw_fixed": "known_unique_chunks",
            }[mode]
            row["prescribed_done_messages"] = record["actors"] ** 2 if mode in (
                "hw_agg", "hardware", "core_hw") else 0
            for reference, label in (("agg_comb", "sw_core_combining"),
                                     ("hw_agg", "sender_hw_agg"), ("hardware", "per_edge_hw"),
                                     ("core_hw", "selector_core_hw")):
                row[f"{label}_cycles_over_mode_cycles"] = groups[(case, reference)][PRIMARY] / record[PRIMARY]
            row["mode_cycles_over_gpu_vertex_cycles"] = record[PRIMARY] / gpu[PRIMARY]
            row["replicates"] = record["replicates"]
            row["identical_replicates"] = record["identical_replicates"]
            row["logs"] = ";".join(record["logs"])
            rows.append(row)
    return rows, problems


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    app = ROOT / "build64-graph-gpu/tests/regression"
    parser.add_argument("--new-log-dir", type=Path, default=app / "forza_graph_core_hw")
    parser.add_argument("--coalesced-reference-dir", type=Path, default=app / "forza_graph_coalesced")
    parser.add_argument("--gpu-reference-dir", type=Path, default=app / "forza_graph")
    parser.add_argument("--output-dir", type=Path, default=ROOT / "docs/proposals")
    parser.add_argument("--expected-cases", type=int, default=10)
    args = parser.parse_args()
    records, rejected = [], []
    for path in sorted(args.new_log_dir.glob("validation-*.log")):
        try:
            records.append(parse_core_log(path))
        except (ValueError, KeyError) as error:
            rejected.append({"source_log": str(path), "reason": str(error)})
    paths = []
    for mode in ("agg_comb", "hw_agg"):
        paths.extend(args.coalesced_reference_dir.glob(f"validation-{mode}-*.log"))
    for mode in ("hw", "gpu"):
        paths.extend(args.gpu_reference_dir.glob(f"validation-direct-{mode}-*.log"))
    for path in sorted(paths):
        try:
            records.append(REF.parse_log(path))
        except (ValueError, KeyError) as error:
            rejected.append({"source_log": str(path), "reason": str(error)})
    if not records:
        parser.error("no verified core-HW/reference logs found")
    rows, problems = compare(records, args.expected_cases)
    if not rows:
        parser.error("no complete matched six-mode graph cases found")
    args.output_dir.mkdir(parents=True, exist_ok=True)
    with (args.output_dir / "forza_graph_core_hw_measurements.csv").open("w", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    report = {
        "scope": "same SimX core-combining algorithm with two HW delivery/completion variants and frozen references",
        "primary_metric": PRIMARY,
        "primary_window": "fresh device through host result readback/cache flush",
        "phase_window": "software phase; not proof of final plain-store DRAM completion",
        "configuration": "same A/FORZA-enabled 2-core, 2-warp, 4-lane driver; private L1 off, shared L2",
        "completion_note": "core_hw uses 256 Selector DONE; core_hw_fixed knows unique core/chunk counts and is not generic actor termination",
        "data_note": "both hybrids send all 32 dense core-owner vectors, including zeros, using source-core-tagged payload56 chunks",
        "stage_note": "publish_sync includes overlapped receiver handling; independent worker maxima must not be summed",
        "reference_note": "only previously frozen official reference logs; earlier repeat references remain in their original reports",
        "expected_case_count": args.expected_cases,
        "measurements": rows, "raw_measurements": records,
        "rejected_logs": rejected, "incomplete_cases": problems,
    }
    manifest_paths = [args.output_dir / "forza_graph_core_hw_manifest.json",
                      args.coalesced_reference_dir / "coalesced-run-manifest.json",
                      args.gpu_reference_dir / "gpu-run-manifest.json"]
    manifest_paths.extend(args.new_log_dir.glob("*run-manifest.json"))
    report["manifests"] = [
        {"path": str(path.resolve()), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
        for path in manifest_paths if path.exists()]
    (args.output_dir / "forza_graph_core_hw_measurements.json").write_text(
        json.dumps(report, indent=2, ensure_ascii=False) + "\n")
    for row in rows:
        print(f"{row['graph']:7} V={row['vertices']:3} E={row['edges']:4} seed={row['seed']:2} "
              f"{row['mode']:13} cycles={row[PRIMARY]:10g} "
              f"SWcomb/mode={row['sw_core_combining_cycles_over_mode_cycles']:.4f} "
              f"mode/GPU={row['mode_cycles_over_gpu_vertex_cycles']:.4f}")
    print(f"Verified {len(records)} logs; {len(rows)} graph/mode measurements.")
    for item in rejected:
        print(f"REJECTED {item['source_log']}: {item['reason']}", file=sys.stderr)
    for problem in problems:
        print(f"INCOMPLETE {problem}", file=sys.stderr)
    return 1 if rejected or problems else 0


if __name__ == "__main__":
    sys.exit(main())
