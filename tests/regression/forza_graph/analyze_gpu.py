#!/usr/bin/env python3
"""Compare newly measured HW actors with direct GPU kernels, per cache policy.

The earlier actor-vs-software-mailbox artifacts are never modified. Input
directories must contain matched extension-A-enabled measurements; graph hashes
and cache groups are never mixed when computing a ratio.
"""

import argparse
import csv
import hashlib
import json
from pathlib import Path
import statistics
import sys

from analyze import METRICS as ACTOR_METRICS, ROOT, SHAPES, fields, key_of, one_line


GPU_MODES = ("gpu_vertex", "gpu_edge")
METRICS = (*ACTOR_METRICS, "device_cycles_post_readback_including_flush")
HEADER_MAP = (("mode", "mode"), ("shape", "graph"), ("hash", "graph_hash"),
              ("vertices", "vertices"), ("edges", "edges"), ("seed", "seed"),
              ("software_queue_slots", "software_queue_slots"))


def parse_log(path, cache_policy):
    lines = path.read_text().splitlines()
    if any(line.startswith("FAILED:") for line in lines):
        raise ValueError("failed correctness record")
    graph = fields(one_line(lines, "GRAPH mode="))
    layout = fields(one_line(lines, "GRAPH output_layout="))
    result = fields(one_line(lines, "RESULT "))
    passed = fields(one_line(lines, "PASSED: exact CPU SpMV reference"))
    for header, output in HEADER_MAP:
        if graph[header] != result[output]:
            raise ValueError(f"GRAPH/RESULT disagree: {header}")
    mode = result["mode"]
    if mode not in ("hardware", *GPU_MODES):
        raise ValueError("expected hardware, gpu_vertex, or gpu_edge")
    record = {**graph, **layout, **result, "cache_policy": cache_policy}
    for key in ("vertices", "edges", "seed", "actors", "max_outdegree",
                "max_indegree", "cross_core_edges", "software_queue_slots", *METRICS):
        record[key] = int(record[key])
        if record[key] < 0:
            raise ValueError(f"negative {key}")
    record.pop("shape")
    record.pop("hash")
    if record["counter_status"] != "0/0/0" or record["cycle_counter_status"] != "0":
        raise ValueError("performance counter query failed")
    if mode == "hardware":
        correct = (int(passed["sent"]) == record["edges"]
                   and int(passed["received"]) == record["edges"]
                   and int(passed["DONE"]) == record["actors"] ** 2
                   and int(passed["outstanding"]) == 0
                   and record["completion"] == "actor_done"
                   and record["output_layout"] == "owner_partitioned")
    else:
        correct = (int(passed["updates"]) == record["edges"]
                   and int(passed["handlers"]) == 0 and int(passed["DONE"]) == 0
                   and passed["completion"] == "global_barrier"
                   and record["completion"] == "global_barrier"
                   and record["output_layout"] == "contiguous")
    if not correct:
        raise ValueError("incorrect update/DONE/completion/layout record")
    span = record["phase_span_cycles"]
    if not (0 < span and 0 <= record["data_span_cycles"] <= span
            and record["max_actor_cycles"] <= span
            <= record["max_actor_cycles"] + record["start_skew_cycles"]):
        raise ValueError("inconsistent device timestamps")
    record["correctness"] = passed
    try:
        record["source_log"] = str(path.resolve().relative_to(ROOT))
    except ValueError:
        record["source_log"] = str(path.resolve())
    return record


def summarize(records):
    summary = dict(records[0])
    for metric in METRICS:
        value = statistics.median(record[metric] for record in records)
        summary[metric] = int(value) if value == int(value) else value
    summary["logs"] = [record["source_log"] for record in records]
    summary["replicates"] = len(records)
    summary["identical_replicates"] = all(
        all(record[metric] == records[0][metric] for metric in METRICS)
        for record in records)
    return summary


def pair_records(records):
    groups = {}
    for record in records:
        key = (record["cache_policy"], key_of(record), record["mode"])
        groups.setdefault(key, []).append(record)
    groups = {key: summarize(value) for key, value in groups.items()}
    pairs, unmatched = [], []
    for (cache, key, mode), gpu in groups.items():
        if mode not in GPU_MODES:
            continue
        actor = groups.get((cache, key, "hardware"))
        if actor is None:
            unmatched.extend(gpu["logs"])
            continue
        for field in ("max_outdegree", "max_indegree", "cross_core_edges"):
            if actor[field] != gpu[field]:
                raise ValueError(f"graph statistics differ: {cache} {key} {field}")
        pair = {"cache_policy": cache, "gpu_mode": mode}
        pair.update({field: gpu[field] for field in (
            "graph", "vertices", "edges", "seed", "graph_hash", "actors",
            "max_outdegree", "max_indegree", "cross_core_edges")})
        pair["counter_window"] = "post_readback_including_flush"
        pair["gpu_speedup_actor_over_gpu"] = actor["phase_span_cycles"] / gpu["phase_span_cycles"]
        pair["actor_speedup_gpu_over_actor"] = gpu["phase_span_cycles"] / actor["phase_span_cycles"]
        pair["gpu_post_readback_speedup_actor_over_gpu"] = (
            actor["device_cycles_post_readback_including_flush"]
            / gpu["device_cycles_post_readback_including_flush"])
        for prefix, record in (("actor", actor), ("gpu", gpu)):
            pair[f"{prefix}_completion"] = record["completion"]
            pair[f"{prefix}_output_layout"] = record["output_layout"]
            for metric in METRICS:
                label = metric.replace("whole_kernel", "post_readback_including_flush")
                pair[f"{prefix}_{label}"] = record[metric]
            pair[f"{prefix}_dram_requests_post_readback_including_flush"] = (
                record["dram_reads_whole_kernel"] + record["dram_writes_whole_kernel"])
            pair[f"{prefix}_replicates"] = record["replicates"]
            pair[f"{prefix}_identical_replicates"] = record["identical_replicates"]
            pair[f"{prefix}_logs"] = ";".join(record["logs"])
        pairs.append(pair)
    for (cache, key, mode), record in groups.items():
        if mode == "hardware":
            for gpu_mode in GPU_MODES:
                if (cache, key, gpu_mode) not in groups:
                    unmatched.append(f"missing {gpu_mode} for {cache} {record['logs'][0]}")
    pairs.sort(key=lambda pair: (
        pair["cache_policy"], pair["vertices"], pair["edges"],
        SHAPES.get(pair["graph"], 99), pair["seed"], pair["gpu_mode"]))
    return pairs, unmatched


def log_directories(arguments):
    if not arguments:
        return {
            "l1_off": ROOT / "build64-graph-gpu/tests/regression/forza_graph",
            "l1_on": ROOT / "build64-graph-gpu-l1/tests/regression/forza_graph",
        }
    directories = {}
    for value in arguments:
        label, separator, path = value.partition("=")
        if not separator or not label or not path or label in directories:
            raise ValueError("--log-dir requires a unique CACHE_LABEL=DIRECTORY")
        directories[label] = Path(path)
    return directories


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--log-dir", action="append", metavar="CACHE_LABEL=DIRECTORY")
    parser.add_argument("--output-dir", type=Path, default=ROOT / "docs/proposals")
    parser.add_argument("--expected-cases", type=int, default=10,
                        help="distinct matched graph cases expected per cache group (default: 10)")
    args = parser.parse_args()
    try:
        directories = log_directories(args.log_dir)
    except ValueError as error:
        parser.error(str(error))
    records, rejected, missing_directories = [], [], []
    for cache, directory in directories.items():
        paths = sorted(directory.glob("validation-*.log"))
        if not paths:
            missing_directories.append({"cache_policy": cache, "path": str(directory)})
        for path in paths:
            try:
                records.append(parse_log(path, cache))
            except (ValueError, KeyError) as error:
                rejected.append({"source_log": str(path), "reason": str(error)})
    if not records:
        parser.error("no verified new hardware/GPU measurements found")
    pairs, unmatched = pair_records(records)
    if not pairs:
        parser.error("no matched hardware/GPU graphs in the same cache group")
    case_sets = {}
    for cache in directories:
        cases = {key_of(record) for record in records
                 if record["cache_policy"] == cache and record["mode"] == "hardware"}
        case_sets[cache] = cases
        if len(cases) != args.expected_cases:
            unmatched.append(f"{cache}: expected {args.expected_cases} graph cases; found {len(cases)}")
    if len({frozenset(cases) for cases in case_sets.values()}) > 1:
        unmatched.append("cache groups have different graph case sets")
    args.output_dir.mkdir(parents=True, exist_ok=True)
    with (args.output_dir / "forza_graph_gpu_measurements.csv").open("w", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=list(pairs[0]))
        writer.writeheader()
        writer.writerows(pairs)
    report = {
        "scope": "direct GPU atomic scatter versus HW-actor scatter in the same SimX model",
        "primary_metric": "max(end)-min(start), with each mode's complete DATA/completion protocol",
        "ratio_definition": {
            "gpu_speedup_actor_over_gpu": "actor phase cycles / GPU phase cycles; >1 favors GPU",
            "actor_speedup_gpu_over_actor": "GPU phase cycles / actor phase cycles; >1 favors actor",
        },
        "counter_window": "kernel launch through host result readback/cache flush",
        "pairing": "same cache label, graph hash/dimensions/seed and number of execution lanes",
        "output_layouts": "actor owner-partitioned slices; GPU contiguous y[destination]",
        "replicate_policy": "median per exact case/mode/cache; all records and equality checks retained",
        "expected_case_count_per_cache": args.expected_cases,
        "log_directories": {key: str(value) for key, value in directories.items()},
        "paired_measurements": pairs, "raw_measurements": records,
        "rejected_logs": rejected, "unmatched_logs": unmatched,
        "missing_log_directories": missing_directories,
    }
    manifests = {}
    for cache, directory in directories.items():
        manifest = directory / "gpu-run-manifest.json"
        if manifest.exists():
            manifests[cache] = {
                "path": str(manifest.resolve()),
                "sha256": hashlib.sha256(manifest.read_bytes()).hexdigest(),
            }
    report["cache_configuration_manifests"] = manifests
    manifest = args.output_dir / "forza_graph_gpu_manifest.json"
    if manifest.exists():
        report["configuration_manifest"] = {
            "path": str(manifest.resolve()),
            "sha256": hashlib.sha256(manifest.read_bytes()).hexdigest(),
        }
    (args.output_dir / "forza_graph_gpu_measurements.json").write_text(
        json.dumps(report, indent=2, ensure_ascii=False) + "\n")
    for pair in pairs:
        print(f"{pair['cache_policy']:7} {pair['graph']:7} V={pair['vertices']:3} "
              f"E={pair['edges']:4} seed={pair['seed']:2} {pair['gpu_mode']:10} "
              f"actor={pair['actor_phase_span_cycles']:10g} GPU={pair['gpu_phase_span_cycles']:10g} "
              f"GPU speedup={pair['gpu_speedup_actor_over_gpu']:.6f}")
    print(f"Verified {len(records)} logs and {len(pairs)} matched comparisons.")
    for problem in rejected:
        print(f"REJECTED {problem['source_log']}: {problem['reason']}", file=sys.stderr)
    for problem in unmatched:
        print(f"UNMATCHED {problem}", file=sys.stderr)
    for problem in missing_directories:
        print(f"MISSING {problem['cache_policy']} logs: {problem['path']}", file=sys.stderr)
    return 1 if rejected or unmatched or missing_directories else 0


if __name__ == "__main__":
    sys.exit(main())
