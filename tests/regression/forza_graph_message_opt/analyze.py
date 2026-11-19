#!/usr/bin/env python3
"""Analyze isolated message/graph-pipeline optimizations and frozen references.

Cross-core edge counts depend on work mapping. They are checked within a mapping
group, while graph hashes, dimensions and degree extrema must match everywhere.
Only this experiment's CSV/JSON are written; earlier artifacts remain unchanged.
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
REFERENCE_SOURCE = ROOT / "tests/regression/forza_graph_core_hw/analyze.py"
SPEC = importlib.util.spec_from_file_location("core_hw_reference_analysis", REFERENCE_SOURCE)
CORE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CORE)
REF = CORE.REF
NEW_MODES = ("msg_local", "msg_private", "msg_warp", "sw_warp", "gpu_warp")
MODES = ("core_hw_fixed", "msg_local", "msg_private", "msg_warp",
         "agg_comb", "sw_warp", "gpu_vertex", "gpu_warp")
PRIMARY = REF.PRIMARY
EXTRAS = ("local_vectors_applied", "partial_rows_per_core")


def work_mapping(mode):
    return "warp_rows_interleaved_cores" if mode in ("msg_warp", "sw_warp", "gpu_warp") else "thread_cyclic_vertices"


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
        raise ValueError(f"unexpected optimization mode: {mode}")
    for header, output in (("mode", "mode"), ("shape", "graph"), ("hash", "graph_hash"),
                           ("vertices", "vertices"), ("edges", "edges"), ("seed", "seed")):
        if graph[header] != result[output]:
            raise ValueError(f"GRAPH/RESULT disagree: {header}")
    for key in (*REF.FOOTPRINTS, "partial_rows_per_core"):
        if memory[key] != result[key]:
            raise ValueError(f"MEMORY/RESULT disagree: {key}")
    record = {**graph, **memory, **layout, **result,
              "cache_policy": "l1_off_shared_l2", "work_mapping": work_mapping(mode)}
    numeric = ("vertices", "edges", "seed", "actors", "max_outdegree", "max_indegree",
               "cross_core_edges", *REF.PHASE_METRICS, *REF.COUNTERS, *REF.REGIONS,
               *REF.AGGREGATION, *REF.FOOTPRINTS, *EXTRAS, "local_capacity_per_core",
               "owned_stride", "one_shot_host_reset_excluded")
    for key in numeric:
        record[key] = int(record[key])
        if record[key] < 0:
            raise ValueError(f"negative {key}")
    record.pop("shape")
    record.pop("hash")
    if record["counter_status"] != "0/0/0" or record["cycle_counter_status"] != "0":
        raise ValueError("performance counter query failed")
    actors, stride = record["actors"], record["owned_stride"]
    correct = (int(passed["updates"]) == record["edges"]
               and int(passed["published_vectors"]) == record["vectors_published"]
               and int(passed["reduced_vectors"]) == record["vectors_reduced"]
               and stride == (record["vertices"] + actors - 1) // actors
               and record["local_bytes_per_core"] <= record["local_capacity_per_core"]
               and record["one_shot_host_reset_excluded"] == 1)
    if mode == "gpu_warp":
        correct = (correct and record["output_layout"] == "contiguous"
                   and record["completion"] == "global_barrier"
                   and all(record[key] == 0 for key in (*REF.AGGREGATION, *REF.FOOTPRINTS, *EXTRAS)))
    else:
        rows = actors // 2 if mode == "msg_private" else 1
        correct = (correct and record["output_layout"] == "owner_partitioned"
                   and record["vectors_published"] == actors
                   and record["vectors_reduced"] == 2 * actors
                   and record["local_vectors_applied"] == actors
                   and record["partial_rows_per_core"] == rows
                   and record["local_bytes_per_core"] == rows * actors * stride * 8)
        if mode == "sw_warp":
            correct = (correct and record["global_vectors_bytes"] == 2 * actors * stride * 8
                       and record["global_active_vectors_bytes"] == actors * stride * 8
                       and record["tree_flags_bytes"] == actors * 8
                       and record["chunks_sent"] == record["chunks_received"] == 0)
        else:
            correct = (correct and record["global_vectors_bytes"] == record["global_active_vectors_bytes"] == 0
                       and record["tree_flags_bytes"] == 0
                       and record["chunks_sent"] == record["chunks_received"] == actors * ((stride + 5) // 6))
    if not correct:
        raise ValueError("incorrect update/vector/chunk/bypass/footprint record")
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
        static += (*REF.AGGREGATION, *REF.FOOTPRINTS)
    if summary["mode"] in NEW_MODES:
        static += EXTRAS
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
        mapping_cross_counts = {}
        gpu = groups[(case, "gpu_vertex")]
        for mode in MODES:
            record = groups[(case, mode)]
            for key in ("max_outdegree", "max_indegree"):
                if record[key] != gpu[key]:
                    raise ValueError(f"intrinsic graph statistic differs: {case} {key}")
            mapping = record["work_mapping"]
            if mapping in mapping_cross_counts and mapping_cross_counts[mapping] != record["cross_core_edges"]:
                raise ValueError(f"cross-core edges differ within work mapping: {case} {mapping}")
            mapping_cross_counts[mapping] = record["cross_core_edges"]
            row = {key: record[key] for key in REF.CASE_FIELDS}
            row.update({"mode": mode, "work_mapping": mapping,
                        "cross_core_edges": record["cross_core_edges"],
                        "cache_policy": record["cache_policy"], "counter_window": "post_readback_including_flush"})
            for key in (*REF.PHASE_METRICS, *REF.COUNTERS):
                row[key.replace("whole_kernel", "post_readback_including_flush")] = record[key]
            for key in (*REF.REGIONS, *REF.AGGREGATION, *REF.FOOTPRINTS, *EXTRAS):
                row[key] = record.get(key)
            row["dram_requests_post_readback_including_flush"] = (
                record["dram_reads_whole_kernel"] + record["dram_writes_whole_kernel"])
            for baseline, label in (("core_hw_fixed", "original_fixed"),
                                    ("agg_comb", "original_sw_comb"), ("sw_warp", "matched_sw_warp")):
                row[f"{label}_cycles_over_mode_cycles"] = groups[(case, baseline)][PRIMARY] / record[PRIMARY]
            row["mode_cycles_over_original_gpu_cycles"] = record[PRIMARY] / gpu[PRIMARY]
            row["mode_cycles_over_matched_gpu_warp_cycles"] = record[PRIMARY] / groups[(case, "gpu_warp")][PRIMARY]
            row["replicates"] = record["replicates"]
            row["identical_replicates"] = record["identical_replicates"]
            row["logs"] = ";".join(record["logs"])
            rows.append(row)
    return rows, problems


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    app = ROOT / "build64-graph-gpu/tests/regression"
    parser.add_argument("--new-log-dir", type=Path, default=app / "forza_graph_message_opt")
    parser.add_argument("--core-reference-dir", type=Path, default=app / "forza_graph_core_hw")
    parser.add_argument("--sw-reference-dir", type=Path, default=app / "forza_graph_coalesced")
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
    for directory, pattern, parse in (
            (args.core_reference_dir, "validation-core_hw_fixed-*.log", CORE.parse_core_log),
            (args.sw_reference_dir, "validation-agg_comb-*.log", REF.parse_log),
            (args.gpu_reference_dir, "validation-direct-gpu-*.log", REF.parse_log)):
        for path in sorted(directory.glob(pattern)):
            try:
                record = parse(path)
                record["work_mapping"] = work_mapping(record["mode"])
                records.append(record)
            except (ValueError, KeyError) as error:
                rejected.append({"source_log": str(path), "reason": str(error)})
    if not records:
        parser.error("no verified optimization/reference logs found")
    rows, problems = compare(records, args.expected_cases)
    if not rows:
        parser.error("no complete matched eight-mode graph cases found")
    args.output_dir.mkdir(parents=True, exist_ok=True)
    with (args.output_dir / "forza_graph_message_opt_measurements.csv").open("w", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    report = {
        "scope": "same SimX graph input, optimized message/fold pipelines and matched SW/GPU work mapping controls",
        "primary_metric": PRIMARY,
        "primary_window": "fresh device through host result readback/cache flush",
        "pairing_note": "graph hash/dimensions/seed/actors and degree extrema invariant; cross-core edge count only invariant within work mapping",
        "configuration": "same A/FORZA-enabled 2-core, 2-warp, 4-lane driver; private L1 off, shared L2",
        "optimization_note": "new modes combine several source and scheduling changes; no single-fix causal attribution",
        "private_note": "private LMEM rows still use acknowledged AMOs; 8x row storage/zeroing plus row reduction",
        "local_bypass_note": "removes same-core DATA delivery, not cross-core DATA; remote dense vectors/chunks remain",
        "stage_note": "independent worker maxima must not be added; unchanged full-fold prefix bounds completion",
        "expected_case_count": args.expected_cases,
        "measurements": rows, "raw_measurements": records,
        "rejected_logs": rejected, "incomplete_cases": problems,
    }
    manifests = [args.output_dir / "forza_graph_message_opt_manifest.json"]
    for directory in (args.new_log_dir, args.core_reference_dir, args.sw_reference_dir, args.gpu_reference_dir):
        manifests.extend(directory.glob("*run-manifest.json"))
    report["manifests"] = [
        {"path": str(path.resolve()), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
        for path in manifests if path.exists()]
    (args.output_dir / "forza_graph_message_opt_measurements.json").write_text(
        json.dumps(report, indent=2, ensure_ascii=False) + "\n")
    for row in rows:
        print(f"{row['graph']:7} V={row['vertices']:3} E={row['edges']:4} seed={row['seed']:2} "
              f"{row['mode']:13} cycles={row[PRIMARY]:10g} "
              f"oldfixed/mode={row['original_fixed_cycles_over_mode_cycles']:.4f} "
              f"mode/GPUwarp={row['mode_cycles_over_matched_gpu_warp_cycles']:.4f}")
    print(f"Verified {len(records)} logs; {len(rows)} graph/mode measurements.")
    for item in rejected:
        print(f"REJECTED {item['source_log']}: {item['reason']}", file=sys.stderr)
    for problem in problems:
        print(f"INCOMPLETE {problem}", file=sys.stderr)
    return 1 if rejected or problems else 0


if __name__ == "__main__":
    sys.exit(main())
