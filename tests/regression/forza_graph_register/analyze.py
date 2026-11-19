#!/usr/bin/env python3
"""Analyze bounded register aggregation against strong matched GPU controls.

The minimum among four declared GPU implementations is reported per graph,
explicitly as a retrospective measured selection, never as a deployed policy.
All earlier analyzers, sources and measurement files are read-only dependencies.
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
REFERENCE_SOURCE = ROOT / "tests/regression/forza_graph_message_opt/analyze.py"
SPEC = importlib.util.spec_from_file_location("message_opt_reference_analysis", REFERENCE_SOURCE)
MSG = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MSG)
REF = MSG.REF
NEW_MODES = ("msg_cache", "sw_cache", "gpu_cache", "gpu_range")
MODES = ("msg_local", "msg_warp", "msg_cache", "sw_warp", "sw_cache",
         "gpu_vertex", "gpu_warp", "gpu_range", "gpu_cache")
GPU_MODES = ("gpu_vertex", "gpu_warp", "gpu_range", "gpu_cache")
MESSAGE_MODES = ("msg_local", "msg_warp", "msg_cache")
CACHE_MODES = ("msg_cache", "sw_cache", "gpu_cache")
PRIMARY = REF.PRIMARY
ATOMIC_FIELDS = ("input_edges", "atomic_flushes", "expected_atomic_flushes")


def parse_new_log(path):
    lines = path.read_text().splitlines()
    if any(line.startswith("FAILED:") for line in lines):
        raise ValueError("failed correctness record")
    graph = REF.fields(REF.one_line(lines, "GRAPH mode="))
    memory = REF.fields(REF.one_line(lines, "MEMORY "))
    layout = REF.fields(REF.one_line(lines, "GRAPH output_layout="))
    result = REF.fields(REF.one_line(lines, "RESULT "))
    passed = REF.fields(REF.one_line(lines, "PASSED: exact CPU SpMV reference"))
    mode = result["mode"]
    if mode not in NEW_MODES:
        raise ValueError(f"unexpected register experiment mode: {mode}")
    for header, output in (("mode", "mode"), ("shape", "graph"), ("hash", "graph_hash"),
                           ("vertices", "vertices"), ("edges", "edges"), ("seed", "seed")):
        if graph[header] != result[output]:
            raise ValueError(f"GRAPH/RESULT disagree: {header}")
    for key in (*REF.FOOTPRINTS, "partial_rows_per_core"):
        if memory[key] != result[key]:
            raise ValueError(f"MEMORY/RESULT disagree: {key}")
    record = {**graph, **memory, **layout, **result, "cache_policy": "l1_off_shared_l2",
              "work_mapping": "balanced_contiguous_edge_ranges_per_warp"}
    numeric = ("vertices", "edges", "seed", "actors", "max_outdegree", "max_indegree",
               "cross_core_edges", *REF.PHASE_METRICS, *REF.COUNTERS, *REF.REGIONS,
               *REF.AGGREGATION, *REF.FOOTPRINTS, *MSG.EXTRAS, *ATOMIC_FIELDS,
               "local_capacity_per_core", "owned_stride", "one_shot_host_reset_excluded")
    for key in numeric:
        record[key] = int(record[key])
        if record[key] < 0:
            raise ValueError(f"negative {key}")
    record.pop("shape")
    record.pop("hash")
    if record["counter_status"] != "0/0/0" or record["cycle_counter_status"] != "0":
        raise ValueError("performance counter query failed")
    actors, stride = record["actors"], record["owned_stride"]
    correct = (int(passed["updates"]) == record["edges"] == record["input_edges"]
               and int(passed["published_vectors"]) == record["vectors_published"]
               and int(passed["reduced_vectors"]) == record["vectors_reduced"]
               and record["atomic_flushes"] == record["expected_atomic_flushes"] <= record["edges"]
               and stride == (record["vertices"] + actors - 1) // actors
               and record["local_bytes_per_core"] <= record["local_capacity_per_core"]
               and record["one_shot_host_reset_excluded"] == 1)
    if mode in ("gpu_cache", "gpu_range"):
        correct = (correct and record["output_layout"] == "contiguous"
                   and record["completion"] == "global_barrier"
                   and all(record[key] == 0 for key in (*REF.AGGREGATION, *REF.FOOTPRINTS, *MSG.EXTRAS)))
        if mode == "gpu_range":
            correct = correct and record["atomic_flushes"] == record["edges"]
    else:
        correct = (correct and record["output_layout"] == "owner_partitioned"
                   and record["vectors_published"] == actors
                   and record["vectors_reduced"] == 2 * actors
                   and record["local_vectors_applied"] == actors
                   and record["partial_rows_per_core"] == 1
                   and record["local_bytes_per_core"] == actors * stride * 8)
        if mode == "sw_cache":
            correct = (correct and record["global_vectors_bytes"] == 2 * actors * stride * 8
                       and record["global_active_vectors_bytes"] == actors * stride * 8
                       and record["tree_flags_bytes"] == actors * 8
                       and record["chunks_sent"] == record["chunks_received"] == 0)
        else:
            correct = (correct and record["global_vectors_bytes"] == record["global_active_vectors_bytes"] == 0
                       and record["tree_flags_bytes"] == 0
                       and record["chunks_sent"] == record["chunks_received"] == actors * ((stride + 5) // 6))
    if not correct:
        raise ValueError("incorrect input/atomic/vector/chunk/footprint record")
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
    metrics = (*REF.PHASE_METRICS, *REF.COUNTERS,
               *(REF.REGIONS if "max_fold_cycles" in summary else ("retries",)))
    static = (*REF.CASE_FIELDS, "work_mapping", "cross_core_edges")
    if "vectors_published" in summary:
        static += (*REF.AGGREGATION, *REF.FOOTPRINTS, *MSG.EXTRAS)
    if summary["mode"] in NEW_MODES:
        static += ATOMIC_FIELDS
    for key in static:
        if any(item[key] != records[0][key] for item in records):
            raise ValueError(f"replicate metadata differs: {key}")
    for key in metrics:
        value = statistics.median(item[key] for item in records)
        summary[key] = int(value) if value == int(value) else value
    summary["logs"] = [item["source_log"] for item in records]
    summary["replicates"] = len(records)
    summary["identical_replicates"] = all(
        all(item[key] == records[0][key] for key in metrics) for item in records)
    return summary


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
        if len({groups[(case, mode)]["atomic_flushes"] for mode in CACHE_MODES}) != 1:
            raise ValueError(f"cache modes emitted different atomic counts: {case}")
        best_gpu = min(GPU_MODES, key=lambda mode: groups[(case, mode)][PRIMARY])
        best_message = min(MESSAGE_MODES, key=lambda mode: groups[(case, mode)][PRIMARY])
        gpu_cycles = groups[(case, best_gpu)][PRIMARY]
        message_cycles = groups[(case, best_message)][PRIMARY]
        mapping_cross_counts = {}
        reference = groups[(case, "gpu_vertex")]
        for mode in MODES:
            record = groups[(case, mode)]
            for key in ("max_outdegree", "max_indegree"):
                if record[key] != reference[key]:
                    raise ValueError(f"intrinsic graph statistic differs: {case} {key}")
            mapping = record["work_mapping"]
            if mapping in mapping_cross_counts and mapping_cross_counts[mapping] != record["cross_core_edges"]:
                raise ValueError(f"cross-core edges differ within mapping: {case} {mapping}")
            mapping_cross_counts[mapping] = record["cross_core_edges"]
            row = {key: record[key] for key in REF.CASE_FIELDS}
            row.update({"mode": mode, "work_mapping": mapping,
                        "cross_core_edges": record["cross_core_edges"],
                        "cache_policy": record["cache_policy"], "counter_window": "post_readback_including_flush"})
            for key in (*REF.PHASE_METRICS, *REF.COUNTERS):
                row[key.replace("whole_kernel", "post_readback_including_flush")] = record[key]
            for key in (*REF.REGIONS, *REF.AGGREGATION, *REF.FOOTPRINTS, *MSG.EXTRAS, *ATOMIC_FIELDS):
                row[key] = record.get(key)
            row["dram_requests_post_readback_including_flush"] = (
                record["dram_reads_whole_kernel"] + record["dram_writes_whole_kernel"])
            row["best_measured_gpu_mode"] = best_gpu
            row["best_measured_gpu_cycles"] = gpu_cycles
            row["mode_cycles_over_best_measured_gpu_cycles"] = record[PRIMARY] / gpu_cycles
            row["best_measured_message_mode"] = best_message
            row["best_measured_message_cycles"] = message_cycles
            row["best_message_cycles_over_best_gpu_cycles"] = message_cycles / gpu_cycles
            row["matched_sw_cache_cycles_over_mode_cycles"] = groups[(case, "sw_cache")][PRIMARY] / record[PRIMARY]
            row["gpu_range_cycles_over_mode_cycles"] = groups[(case, "gpu_range")][PRIMARY] / record[PRIMARY]
            row["gpu_cache_cycles_over_mode_cycles"] = groups[(case, "gpu_cache")][PRIMARY] / record[PRIMARY]
            row["replicates"] = record["replicates"]
            row["identical_replicates"] = record["identical_replicates"]
            row["logs"] = ";".join(record["logs"])
            rows.append(row)
    return rows, problems


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    app = ROOT / "build64-graph-gpu/tests/regression"
    parser.add_argument("--new-log-dir", type=Path, default=app / "forza_graph_register")
    parser.add_argument("--message-reference-dir", type=Path, default=app / "forza_graph_message_opt")
    parser.add_argument("--gpu-reference-dir", type=Path, default=app / "forza_graph")
    parser.add_argument("--output-dir", type=Path, default=ROOT / "docs/proposals")
    parser.add_argument("--expected-cases", type=int, default=10)
    args = parser.parse_args()
    records, rejected = [], []
    for path in sorted(args.new_log_dir.glob("validation-*.log")):
        try:
            records.append(parse_new_log(path))
        except (ValueError, KeyError) as error:
            rejected.append({"source_log": str(path), "reason": str(error)})
    for mode in ("msg_local", "msg_warp", "sw_warp", "gpu_warp"):
        for path in sorted(args.message_reference_dir.glob(f"validation-{mode}-*.log")):
            try:
                records.append(MSG.parse_new_log(path))
            except (ValueError, KeyError) as error:
                rejected.append({"source_log": str(path), "reason": str(error)})
    for path in sorted(args.gpu_reference_dir.glob("validation-direct-gpu-*.log")):
        try:
            record = REF.parse_log(path)
            record["work_mapping"] = MSG.work_mapping(record["mode"])
            records.append(record)
        except (ValueError, KeyError) as error:
            rejected.append({"source_log": str(path), "reason": str(error)})
    if not records:
        parser.error("no verified register/reference logs found")
    rows, problems = compare(records, args.expected_cases)
    if not rows:
        parser.error("no complete matched nine-mode graph cases found")
    args.output_dir.mkdir(parents=True, exist_ok=True)
    with (args.output_dir / "forza_graph_register_measurements.csv").open("w", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    report = {
        "scope": "bounded balanced-edge-range register aggregation with matched direct-GPU optimization",
        "primary_metric": PRIMARY,
        "primary_window": "fresh device through host result readback/cache flush",
        "mapping_note": "hash/dimensions/seed/actors/degrees invariant; cross-core count checked only within mapping",
        "configuration": "same A/FORZA-enabled 2-core, 2-warp, 4-lane driver; private L1 off, shared L2",
        "cache_note": "two named scalar LRU entries/lane; every eviction/final valid entry emits AMO even if sum zero",
        "counts_note": "logical input edges, atomic flushes and transport chunks are distinct; cache3 flush counts equal",
        "selection_note": "per-case minimum measured median among declared variants, retrospective analysis; no adaptive policy",
        "declared_gpu_selection_modes": GPU_MODES,
        "declared_message_selection_modes": MESSAGE_MODES,
        "optimization_note": "new mapping/layout/cache changes are joint; gpu_cache vs gpu_range is the same-mapping cache control",
        "stage_note": "independent worker maxima must not be added; an unchanged mandatory fold prefix bounds completion",
        "expected_case_count": args.expected_cases,
        "measurements": rows, "raw_measurements": records,
        "rejected_logs": rejected, "incomplete_cases": problems,
    }
    manifests = [args.output_dir / "forza_graph_register_manifest.json"]
    for directory in (args.new_log_dir, args.message_reference_dir, args.gpu_reference_dir):
        manifests.extend(directory.glob("*run-manifest.json"))
    report["manifests"] = [
        {"path": str(path.resolve()), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
        for path in manifests if path.exists()]
    (args.output_dir / "forza_graph_register_measurements.json").write_text(
        json.dumps(report, indent=2, ensure_ascii=False) + "\n")
    for row in rows:
        print(f"{row['graph']:7} V={row['vertices']:3} E={row['edges']:4} seed={row['seed']:2} "
              f"{row['mode']:10} cycles={row[PRIMARY]:10g} "
              f"mode/bestGPU={row['mode_cycles_over_best_measured_gpu_cycles']:.4f} "
              f"atomics={row['atomic_flushes']}")
    print(f"Verified {len(records)} logs; {len(rows)} graph/mode measurements.")
    for item in rejected:
        print(f"REJECTED {item['source_log']}: {item['reason']}", file=sys.stderr)
    for problem in problems:
        print(f"INCOMPLETE {problem}", file=sys.stderr)
    return 1 if rejected or problems else 0


if __name__ == "__main__":
    sys.exit(main())
