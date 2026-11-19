#!/usr/bin/env python3
"""Analyze Mitul-derived graph aggregation and matched actor/GPU references.

Only A-enabled, private-L1-off/shared-L2 measurements from the matching build
belong here. Older actor-only and L1-on artifacts are never read or overwritten.
Primary ratios use device cycles after result readback and cache flush.
"""

import argparse
import csv
import hashlib
import json
from pathlib import Path
import re
import statistics
import sys


ROOT = Path(__file__).resolve().parents[3]
CASE_FIELDS = ("graph", "vertices", "edges", "seed", "graph_hash", "actors")
NEW_MODES = ("agg_amo", "agg_lmem", "agg_comb", "hw_agg")
MODES = ("software", "hardware", "agg_amo", "agg_lmem", "agg_comb", "hw_agg", "gpu_vertex")
PHASE_METRICS = ("phase_span_cycles", "data_span_cycles", "max_actor_cycles", "start_skew_cycles")
COUNTERS = ("dram_reads_whole_kernel", "dram_writes_whole_kernel",
            "retired_instructions_whole_kernel", "device_cycles_post_readback_including_flush")
REGIONS = ("max_local_init_cycles", "max_fold_cycles", "max_publish_sync_cycles", "max_reduce_cycles")
AGGREGATION = ("vectors_published", "vectors_reduced", "chunks_sent", "chunks_received")
FOOTPRINTS = ("global_vectors_bytes", "global_active_vectors_bytes", "local_bytes_per_core", "tree_flags_bytes")
SHAPES = {"regular": 0, "skew": 1, "hub": 2}
PRIMARY = "device_cycles_post_readback_including_flush"


def fields(line):
    return dict(re.findall(r"\b([A-Za-z_][A-Za-z_0-9]*)=([^\s;]+)", line))


def one_line(lines, prefix):
    matches = [line for line in lines if line.startswith(prefix)]
    if len(matches) != 1:
        raise ValueError(f"expected one {prefix.strip()} line, found {len(matches)}")
    return matches[0]


def case_key(record):
    return tuple(record[key] for key in CASE_FIELDS)


def parse_log(path):
    lines = path.read_text().splitlines()
    if any(line.startswith("FAILED:") for line in lines):
        raise ValueError("failed correctness record")
    graph = fields(one_line(lines, "GRAPH mode="))
    result = fields(one_line(lines, "RESULT "))
    passed = fields(one_line(lines, "PASSED: exact CPU SpMV reference"))
    mode = result["mode"]
    if mode not in MODES:
        raise ValueError(f"unexpected mode: {mode}")
    for header, output in (("mode", "mode"), ("shape", "graph"), ("hash", "graph_hash"),
                           ("vertices", "vertices"), ("edges", "edges"), ("seed", "seed")):
        if graph[header] != result[output]:
            raise ValueError(f"GRAPH/RESULT disagree: {header}")
    record = {**graph, **result, "cache_policy": "l1_off_shared_l2"}
    if mode in NEW_MODES:
        memory = fields(one_line(lines, "MEMORY "))
        for key in FOOTPRINTS:
            if memory[key] != result[key]:
                raise ValueError(f"MEMORY/RESULT disagree: {key}")
        record.update(memory)
        numeric = (*REGIONS, *AGGREGATION, *FOOTPRINTS, "local_capacity_per_core",
                   "owned_stride", "one_shot_host_reset_excluded")
    else:
        record.update(fields(one_line(lines, "GRAPH output_layout=")))
        numeric = ("retries", "software_queue_slots")
    for key in ("vertices", "edges", "seed", "actors", "max_outdegree", "max_indegree",
                "cross_core_edges", *PHASE_METRICS, *COUNTERS, *numeric):
        record[key] = int(record[key])
        if record[key] < 0:
            raise ValueError(f"negative {key}")
    record.pop("shape")
    record.pop("hash")
    if record["counter_status"] != "0/0/0" or record["cycle_counter_status"] != "0":
        raise ValueError("performance counter query failed")
    if mode in NEW_MODES:
        correct = (int(passed["updates"]) == record["edges"]
                   and int(passed["published_vectors"]) == record["vectors_published"]
                   and int(passed["reduced_vectors"]) == record["vectors_reduced"]
                   and record["vectors_published"] == record["vectors_reduced"]
                   and record["chunks_sent"] == record["chunks_received"]
                   and record["global_active_vectors_bytes"] <= record["global_vectors_bytes"]
                   and record["local_bytes_per_core"] <= record["local_capacity_per_core"]
                   and record["owned_stride"] == (record["vertices"] + record["actors"] - 1) // record["actors"]
                   and record["tree_flags_bytes"] == record["actors"] * 8
                   and record["one_shot_host_reset_excluded"] == 1)
        if mode == "hw_agg":
            correct = correct and record["chunks_sent"] == (
                record["vectors_published"] * ((record["owned_stride"] + 5) // 6))
        else:
            correct = correct and record["chunks_sent"] == 0
    elif mode == "gpu_vertex":
        correct = (int(passed["updates"]) == record["edges"]
                   and int(passed["handlers"]) == 0 and int(passed["DONE"]) == 0
                   and passed["completion"] == "global_barrier"
                   and record["completion"] == "global_barrier"
                   and record["output_layout"] == "contiguous")
    else:
        correct = (int(passed["sent"]) == record["edges"]
                   and int(passed["received"]) == record["edges"]
                   and int(passed["DONE"]) == record["actors"] ** 2
                   and int(passed["outstanding"]) == 0
                   and record["completion"] == "actor_done"
                   and record["output_layout"] == "owner_partitioned")
    if not correct:
        raise ValueError("incorrect update/aggregation/chunk/completion/footprint record")
    span = record["phase_span_cycles"]
    if not (0 < span and 0 <= record["data_span_cycles"] <= span
            and record["max_actor_cycles"] <= span
            <= record["max_actor_cycles"] + record["start_skew_cycles"]
            and record[PRIMARY] >= span):
        raise ValueError("inconsistent device timestamps")
    record["correctness"] = passed
    try:
        record["source_log"] = str(path.resolve().relative_to(ROOT))
    except ValueError:
        record["source_log"] = str(path.resolve())
    return record


def summarize(records):
    summary = dict(records[0])
    keys = (*PHASE_METRICS, *COUNTERS, *(REGIONS if summary["mode"] in NEW_MODES else ("retries",)))
    static = (*CASE_FIELDS, *AGGREGATION, *FOOTPRINTS) if summary["mode"] in NEW_MODES else CASE_FIELDS
    for key in static:
        if any(record[key] != records[0][key] for record in records):
            raise ValueError(f"replicate metadata differs: {key}")
    for key in keys:
        value = statistics.median(record[key] for record in records)
        summary[key] = int(value) if value == int(value) else value
    summary["logs"] = [record["source_log"] for record in records]
    summary["replicates"] = len(records)
    summary["identical_replicates"] = all(
        all(record[key] == records[0][key] for key in keys) for record in records)
    return summary


def compare(records, expected_cases):
    groups = {}
    for record in records:
        groups.setdefault((case_key(record), record["mode"]), []).append(record)
    groups = {key: summarize(value) for key, value in groups.items()}
    cases = sorted({key for key, _ in groups}, key=lambda key: (
        key[1], key[2], SHAPES.get(key[0], 99), key[3]))
    problems = []
    if len(cases) != expected_cases:
        problems.append(f"expected {expected_cases} graph cases; found {len(cases)}")
    rows = []
    for case in cases:
        missing = [mode for mode in MODES if (case, mode) not in groups]
        if missing:
            problems.append(f"missing {','.join(missing)} for {case}")
            continue
        reference = groups[(case, "gpu_vertex")]
        for mode in MODES:
            record = groups[(case, mode)]
            for key in ("max_outdegree", "max_indegree", "cross_core_edges"):
                if record[key] != reference[key]:
                    raise ValueError(f"graph statistics differ for {case}: {key}")
            row = {key: record[key] for key in CASE_FIELDS}
            row.update({"mode": mode, "cache_policy": record["cache_policy"],
                        "counter_window": "post_readback_including_flush"})
            for key in (*PHASE_METRICS, *COUNTERS):
                row[key.replace("whole_kernel", "post_readback_including_flush")] = record[key]
            row["dram_requests_post_readback_including_flush"] = (
                record["dram_reads_whole_kernel"] + record["dram_writes_whole_kernel"])
            for key in (*REGIONS, *AGGREGATION, *FOOTPRINTS):
                row[key] = record.get(key)
            row["software_queue_slots"] = record.get("software_queue_slots") if mode == "software" else None
            for baseline, label in (("software", "spsc_per_edge"),
                                    ("hardware", "hw_per_edge"), ("hw_agg", "hw_aggregated")):
                row[f"{label}_cycles_over_mode_cycles"] = groups[(case, baseline)][PRIMARY] / record[PRIMARY]
            row["mode_cycles_over_gpu_vertex_cycles"] = record[PRIMARY] / reference[PRIMARY]
            row["replicates"] = record["replicates"]
            row["identical_replicates"] = record["identical_replicates"]
            row["logs"] = ";".join(record["logs"])
            rows.append(row)
    return rows, problems


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    app = ROOT / "build64-graph-gpu/tests/regression"
    parser.add_argument("--new-log-dir", type=Path, default=app / "forza_graph_coalesced")
    parser.add_argument("--reference-log-dir", type=Path, default=app / "forza_graph")
    parser.add_argument("--output-dir", type=Path, default=ROOT / "docs/proposals")
    parser.add_argument("--expected-cases", type=int, default=10)
    args = parser.parse_args()
    paths = set(args.new_log_dir.glob("validation-*.log"))
    for pattern in ("validation-direct-hw-*.log", "validation-direct-gpu-*.log",
                    "validation-coalesced-reference-sw-*.log"):
        paths.update(args.reference_log_dir.glob(pattern))
    records, rejected = [], []
    for path in sorted(paths):
        try:
            records.append(parse_log(path))
        except (ValueError, KeyError) as error:
            rejected.append({"source_log": str(path), "reason": str(error)})
    if not records:
        parser.error("no verified matched coalesced/reference logs found")
    rows, problems = compare(records, args.expected_cases)
    if not rows:
        parser.error("no complete matched seven-mode graph cases found")
    args.output_dir.mkdir(parents=True, exist_ok=True)
    with (args.output_dir / "forza_graph_coalesced_measurements.csv").open("w", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    report = {
        "scope": "Mitul-derived graph aggregation adapters and same-SimX matched references; not physical-GPU performance",
        "primary_metric": PRIMARY,
        "primary_window": "fresh device through host result readback/cache flush",
        "phase_window": "software phase; plain output stores may drain after its final timestamp",
        "region_maxima_note": "max_init/fold/publish_sync/reduce are independent worker maxima; do not sum them",
        "strict_configuration": "same A/FORZA-enabled two-core/two-warp/four-lane driver; private L1 off, shared L2",
        "canonical_commit": "7ef9d3acf9d34c0df1c8a257fe3bf67759081472",
        "adaptation_note": "weighted int64 CSR graph, persistent lane actors, acknowledged barrier/publication, one-shot host reset",
        "agg_comb_note": "per-core combining/flush/reduction; no message queue",
        "hw_agg_note": "same sender AMO fold as agg_amo; payload56 header8+six int64 values; Selector DATA+DONE completion",
        "ratios": "baseline_cycles_over_mode_cycles >1 favors mode; mode_cycles_over_gpu_vertex_cycles >1 favors GPU",
        "expected_case_count": args.expected_cases,
        "measurements": rows, "raw_measurements": records,
        "rejected_logs": rejected, "incomplete_cases": problems,
    }
    manifests = []
    for manifest in (args.output_dir / "forza_graph_coalesced_manifest.json",
                     args.new_log_dir / "coalesced-run-manifest.json",
                     args.reference_log_dir / "gpu-run-manifest.json"):
        if manifest.exists():
            manifests.append({"path": str(manifest.resolve()),
                              "sha256": hashlib.sha256(manifest.read_bytes()).hexdigest()})
    report["manifests"] = manifests
    (args.output_dir / "forza_graph_coalesced_measurements.json").write_text(
        json.dumps(report, indent=2, ensure_ascii=False) + "\n")
    for row in rows:
        print(f"{row['graph']:7} V={row['vertices']:3} E={row['edges']:4} seed={row['seed']:2} "
              f"{row['mode']:10} cycles={row[PRIMARY]:10g} "
              f"SPSC/mode={row['spsc_per_edge_cycles_over_mode_cycles']:.4f} "
              f"mode/GPU={row['mode_cycles_over_gpu_vertex_cycles']:.4f}")
    print(f"Verified {len(records)} logs; {len(rows)} graph/mode measurements.")
    for item in rejected:
        print(f"REJECTED {item['source_log']}: {item['reason']}", file=sys.stderr)
    for problem in problems:
        print(f"INCOMPLETE {problem}", file=sys.stderr)
    return 1 if rejected or problems else 0


if __name__ == "__main__":
    sys.exit(main())
