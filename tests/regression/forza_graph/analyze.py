#!/usr/bin/env python3
"""Validate and pair graph logs; export model-scoped measurements and plots.

The primary metric is the device phase span. Memory and instruction counters
are sampled after host result readback/cache flush, a different measurement
window. Matplotlib is optional; CSV/JSON export needs only the standard library.
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
KEYS = ("graph", "vertices", "edges", "seed", "graph_hash", "actors")
METRICS = (
    "phase_span_cycles", "data_span_cycles", "max_actor_cycles",
    "start_skew_cycles", "retries", "dram_reads_whole_kernel",
    "dram_writes_whole_kernel", "retired_instructions_whole_kernel",
)
SHAPES = {"regular": 0, "skew": 1, "hub": 2}
COUNTER_WINDOW = "kernel launch through host result readback/cache flush"


def fields(line):
    return dict(re.findall(r"\b([A-Za-z_][A-Za-z_0-9]*)=([^\s;]+)", line))


def one_line(lines, prefix):
    matches = [line for line in lines if line.startswith(prefix)]
    if len(matches) != 1:
        raise ValueError(f"expected one {prefix.strip()} line, found {len(matches)}")
    return matches[0]


def parse_log(path):
    lines = path.read_text().splitlines()
    if any(line.startswith("FAILED:") for line in lines):
        raise ValueError("failed correctness record")
    graph = fields(one_line(lines, "GRAPH "))
    result = fields(one_line(lines, "RESULT "))
    passed = fields(one_line(lines, "PASSED: exact CPU SpMV reference"))
    for header, output in (("mode", "mode"), ("shape", "graph"),
                           ("hash", "graph_hash"), ("vertices", "vertices"),
                           ("edges", "edges"), ("seed", "seed"),
                           ("software_queue_slots", "software_queue_slots")):
        if graph[header] != result[output]:
            raise ValueError(f"GRAPH/RESULT disagree: {header}")
    if result["mode"] not in ("hardware", "software"):
        raise ValueError("unknown transport")
    record = {**graph, **result}
    for key in ("vertices", "edges", "seed", "actors", "max_outdegree",
                "max_indegree", "cross_core_edges", "software_queue_slots", *METRICS):
        record[key] = int(record[key])
        if record[key] < 0:
            raise ValueError(f"negative {key}")
    record.pop("shape")
    record.pop("hash")
    if record["counter_status"] != "0/0/0":
        raise ValueError("performance counter query failed")
    if (int(passed["sent"]) != record["edges"]
            or int(passed["received"]) != record["edges"]
            or int(passed["DONE"]) != record["actors"] ** 2
            or int(passed["outstanding"]) != 0):
        raise ValueError("message/DONE/drain counts disagree")
    span = record["phase_span_cycles"]
    if not (0 < span and 0 <= record["data_span_cycles"] <= span
            and record["max_actor_cycles"] <= span
            <= record["max_actor_cycles"] + record["start_skew_cycles"]):
        raise ValueError("inconsistent device timestamps")
    record["correctness"] = {key: int(value) for key, value in passed.items()}
    try:
        record["source_log"] = str(path.resolve().relative_to(ROOT))
    except ValueError:
        record["source_log"] = str(path.resolve())
    return record


def key_of(record):
    return tuple(record[key] for key in KEYS)


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


def pair_records(records, primary_capacity):
    groups = {}
    for record in records:
        group = (key_of(record), record["mode"], record["software_queue_slots"])
        groups.setdefault(group, []).append(record)
    groups = {key: summarize(value) for key, value in groups.items()}
    pairs, unmatched = [], []
    for (key, mode, capacity), sw in groups.items():
        if mode != "software":
            continue
        hw = groups.get((key, "hardware", primary_capacity))
        if hw is None:
            unmatched.extend(sw["logs"])
            continue
        for field in ("max_outdegree", "max_indegree", "cross_core_edges"):
            if hw[field] != sw[field]:
                raise ValueError(f"graph statistics differ for {key}: {field}")
        pair = {field: sw[field] for field in KEYS}
        pair.update({field: sw[field] for field in (
            "max_outdegree", "max_indegree", "cross_core_edges")})
        pair["comparison"] = "primary" if capacity == primary_capacity else "capacity_sensitivity"
        pair["sw_queue_slots"] = capacity
        pair["counter_window"] = "post_readback_including_flush"
        pair["phase_speedup_sw_over_hw"] = sw["phase_span_cycles"] / hw["phase_span_cycles"]
        pair["data_speedup_sw_over_hw"] = sw["data_span_cycles"] / hw["data_span_cycles"]
        for prefix, record in (("hw", hw), ("sw", sw)):
            for metric in METRICS:
                label = metric.replace("whole_kernel", "post_readback_including_flush")
                pair[f"{prefix}_{label}"] = record[metric]
            pair[f"{prefix}_dram_requests_post_readback_including_flush"] = (
                record["dram_reads_whole_kernel"] + record["dram_writes_whole_kernel"])
            pair[f"{prefix}_replicates"] = record["replicates"]
            pair[f"{prefix}_identical_replicates"] = record["identical_replicates"]
            pair[f"{prefix}_logs"] = ";".join(record["logs"])
        pairs.append(pair)
    used_hw = {key_of(pair) for pair in pairs}
    for (key, mode, _), record in groups.items():
        if mode == "hardware" and key not in used_hw:
            unmatched.extend(record["logs"])
    pairs.sort(key=lambda pair: (
        pair["comparison"] != "primary", pair["vertices"], pair["edges"],
        SHAPES.get(pair["graph"], 99), pair["seed"], pair["sw_queue_slots"]))
    return pairs, unmatched


def plot(pairs, output_dir, primary_capacity):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        from matplotlib.patches import Patch
    except ImportError:
        return "matplotlib unavailable; CSV and JSON were generated"
    primary = [pair for pair in pairs if pair["comparison"] == "primary"]
    colors = {"regular": "#797979", "skew": "#2878b5", "hub": "#3c8d5a"}
    labels = [f"{p['graph']}\nV={p['vertices']}, E={p['edges']}\nseed={p['seed']}" for p in primary]
    speedups = [p["phase_speedup_sw_over_hw"] for p in primary]
    with plt.rc_context({"font.size": 10, "axes.spines.top": False,
                         "axes.spines.right": False, "svg.fonttype": "none"}):
        figure, axis = plt.subplots(figsize=(11, 5.5))
        axis.bar(range(len(primary)), speedups,
                 color=[colors.get(p["graph"], "#797979") for p in primary], width=0.65)
        axis.axhline(1, color="black", linewidth=1, linestyle="--")
        axis.set_xticks(range(len(primary)), labels)
        axis.set_ylim(0, max(speedups) * 1.22)
        axis.set_ylabel("Software phase cycles / hardware phase cycles")
        axis.set_title("Actor graph scatter: SimX model comparison")
        axis.grid(axis="y", alpha=0.2)
        axis.set_axisbelow(True)
        for index, value in enumerate(speedups):
            axis.text(index, value + 0.025, f"{value:.3f}×", ha="center", va="bottom")
        axis.legend(handles=[Patch(color=color, label=shape) for shape, color in colors.items()],
                    loc="upper left", frameon=False, ncols=3)
        figure.text(0.5, 0.035,
                    f"Same 2-core / 16-actor SimX, L1D disabled, shared L2; software pair queue = {primary_capacity} slots.\n"
                    "Primary metric includes DATA handlers and DONE completion. Model timing is not calibrated to physical RTL.",
                    ha="center", fontsize=9)
        figure.tight_layout(rect=(0, 0.13, 1, 1))
        for extension in ("png", "svg"):
            figure.savefig(output_dir / f"forza_graph_speedup.{extension}", dpi=180)
        plt.close(figure)
    return "PNG and SVG generated"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--log-dir", type=Path,
                        default=ROOT / "build64-graph/tests/regression/forza_graph")
    parser.add_argument("--output-dir", type=Path, default=ROOT / "docs/proposals")
    parser.add_argument("--primary-capacity", type=int, default=16)
    args = parser.parse_args()
    records, rejected = [], []
    for path in sorted(args.log_dir.glob("validation-*.log")):
        try:
            records.append(parse_log(path))
        except (ValueError, KeyError) as error:
            rejected.append({"source_log": str(path), "reason": str(error)})
    if not records:
        parser.error("no verified GRAPH/PASSED/RESULT records found")
    pairs, unmatched = pair_records(records, args.primary_capacity)
    if not pairs:
        parser.error("no matching hardware/software graph hashes found")
    args.output_dir.mkdir(parents=True, exist_ok=True)
    with (args.output_dir / "forza_graph_measurements.csv").open("w", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=list(pairs[0]))
        writer.writeheader()
        writer.writerows(pairs)
    plot_status = plot(pairs, args.output_dir, args.primary_capacity)
    report = {
        "scope": "same SimX model and Selector runtime; not a physical GPU speedup claim",
        "primary_metric": "max(actor end) - min(actor start), including DATA and DONE",
        "secondary_metric": "max(last DATA handler end) - min(actor start)",
        "counter_window": COUNTER_WINDOW,
        "counter_field_note": "raw names ending whole_kernel include host result readback/cache flush",
        "replicate_policy": "median per exact graph/mode/capacity; all raw records and equality checks retained",
        "primary_software_queue_capacity": args.primary_capacity,
        "plot_status": plot_status, "paired_measurements": pairs,
        "raw_measurements": records, "rejected_logs": rejected, "unmatched_logs": unmatched,
    }
    manifest = args.output_dir / "forza_graph_manifest.json"
    if manifest.exists():
        report["configuration_manifest"] = {
            "path": str(manifest.resolve()),
            "sha256": hashlib.sha256(manifest.read_bytes()).hexdigest(),
        }
    (args.output_dir / "forza_graph_measurements.json").write_text(
        json.dumps(report, indent=2, ensure_ascii=False) + "\n")
    for pair in pairs:
        print(f"{pair['comparison']:20} {pair['graph']:7} V={pair['vertices']:3} E={pair['edges']:4} "
              f"seed={pair['seed']:2} q={pair['sw_queue_slots']:2} "
              f"HW={pair['hw_phase_span_cycles']:9g} SW={pair['sw_phase_span_cycles']:9g} "
              f"speedup={pair['phase_speedup_sw_over_hw']:.6f}")
    print(f"Verified {len(records)} logs; {len(pairs)} pairs. {plot_status}.")
    for problem in rejected:
        print(f"REJECTED {problem['source_log']}: {problem['reason']}", file=sys.stderr)
    for path in unmatched:
        print(f"UNMATCHED {path}", file=sys.stderr)
    return 1 if rejected or unmatched else 0


if __name__ == "__main__":
    sys.exit(main())
