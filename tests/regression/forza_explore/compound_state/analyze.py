#!/usr/bin/env python3
"""Export validated transaction measurements without hiding failed experiments."""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import re


def fields(line):
    return dict(re.findall(r'([A-Za-z_][A-Za-z_0-9]*)=([^\s]+)', line))


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--off', type=Path, required=True)
    parser.add_argument('--on', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    rows, rejected, raw, repeats = [], [], [], []
    for cache, build in [('l1_off', args.off.resolve()), ('l1_on', args.on.resolve())]:
        app = build / 'tests/regression/forza_explore/compound_state'
        for log in sorted((app / 'logs').glob('validation-*.log')):
            text = log.read_text()
            result = next((fields(line) for line in text.splitlines() if line.startswith('RESULT ')), None)
            workload = next((fields(line) for line in text.splitlines() if line.startswith('WORKLOAD ')), {})
            memory = next((fields(line) for line in text.splitlines() if line.startswith('MEMORY ')), {})
            record = {'cache': cache, 'log': str(log), 'sha256': sha256(log),
                      'workload': workload, 'memory': memory, 'result': result}
            raw.append(record)
            okay = ('\nPASSED:' in text and result is not None
                    and 'RUNNER_EXIT=0 ' in text
                    and result.get('counter_status') == '0/0/0'
                    and result.get('cycle_counter_status') == '0'
                    and result.get('route_counter_status') == '0/0')
            if not okay:
                rejected.append(record)
                continue
            row = {'cache': cache, **workload, **memory, **result,
                   'cycle_window': 'all_launches_post_readback_including_flush',
                   'log': str(log), 'log_sha256': record['sha256']}
            for key, value in list(row.items()):
                if isinstance(value, str) and value.isdecimal():
                    row[key] = int(value)
            if '-repeat-' in log.name:
                repeats.append(row)
            else:
                rows.append(row)
    groups = {}
    for row in rows:
        key = (row['cache'], row['keys'], row['requests'], row['shape'], row['seed'])
        groups.setdefault(key, []).append(row)
    summaries = []
    for key, group in sorted(groups.items()):
        hashes = {row['input_hash'] for row in group}
        actors = {row['actors'] for row in group}
        if len(hashes) != 1 or len(actors) != 1:
            raise ValueError(f'unmatched workload for {key}')
        by_mode = {row['mode']: row for row in group}
        if len(by_mode) != len(group):
            raise ValueError(f'duplicate official mode for {key}')
        gpu = {mode: row['device_cycles_post_readback_including_flush']
               for mode, row in by_mode.items() if mode.startswith('gpu_')}
        summary = dict(zip(('cache', 'keys', 'requests', 'shape', 'seed'), key))
        summary['input_hash'] = next(iter(hashes))
        summary['modes'] = {mode: row['device_cycles_post_readback_including_flush']
                            for mode, row in by_mode.items()}
        if gpu and 'mp' in by_mode:
            best_gpu = min(gpu, key=gpu.get)
            summary['best_measured_gpu_mode'] = best_gpu
            summary['mp_over_best_measured_gpu_cycles'] = (
                summary['modes']['mp'] / gpu[best_gpu])
        summaries.append(summary)
    repeat_checks = []
    for repeat in repeats:
        match = next((row for row in rows if all(row[name] == repeat[name] for name in
                     ('cache', 'keys', 'requests', 'shape', 'seed', 'mode', 'input_hash'))), None)
        keys = ('device_cycles_post_readback_including_flush',
                'final_stage_phase_span_cycles', 'transactions', 'messages_sent',
                'messages_received', 'retries', 'dram_reads_all_stages_post_readback',
                'dram_writes_all_stages_post_readback', 'retired_instructions_post_readback')
        repeat_checks.append({'log': repeat['log'], 'matched': match is not None,
                              'exact': match is not None and all(repeat[k] == match[k] for k in keys)})
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    columns = list(dict.fromkeys(key for row in rows for key in row))
    with output.with_suffix('.csv').open('w', newline='') as file:
        writer = csv.DictWriter(file, fieldnames=columns)
        writer.writeheader()
        writer.writerows(rows)
    document = {'schema': 1, 'primary_metric': 'device_cycles_post_readback_including_flush',
                'cycle_window': 'all_launches_post_readback_including_flush',
                'official_rows': len(rows), 'raw_logs': len(raw), 'rejected_count': len(rejected),
                'rows': rows, 'summaries': summaries, 'repeats': repeat_checks,
                'rejected': rejected, 'raw': raw}
    output.with_suffix('.json').write_text(json.dumps(document, indent=2) + '\n')
    print(json.dumps({'rows': len(rows), 'raw_logs': len(raw), 'rejected': len(rejected),
                      'groups': len(groups), 'repeat_checks': repeat_checks}, indent=2))


if __name__ == '__main__':
    main()
