#!/usr/bin/env python3
"""Pair the corrected receive cohort with frozen native transaction controls."""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import re


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def parse(line):
    return dict(re.findall(r'([A-Za-z_][A-Za-z_0-9]*)=([^\s]+)', line))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--root', type=Path, required=True)
    args = parser.parse_args()
    root = args.root.resolve()
    reference_path = root / 'docs/proposals/forza_explore_compound_measurements.json'
    old = json.loads(reference_path.read_text())
    rows = [dict(row, cohort='unchanged_native_reference') for row in old['rows']
            if row['mode'] in ('gpu_lock', 'gpu_owner')]
    for row in rows:
        if sha(Path(row['log'])) != row['log_sha256']:
            raise RuntimeError('frozen reference log changed')
    raw, rejected = [], []
    for cache, build in [('l1_off', 'build64-forza-explore'), ('l1_on', 'build64-forza-explore-l1')]:
        app = root / build / 'tests/regression/forza_explore/compound_state_ordered'
        for path in sorted((app / 'logs').glob('validation-*.log')):
            text = path.read_text()
            selected = {tag: next((parse(line) for line in text.splitlines()
                                  if line.startswith(tag + ' ')), {})
                        for tag in ('WORKLOAD', 'MEMORY', 'RESULT')}
            record = {'cache': cache, 'log': str(path), 'log_sha256': sha(path), **selected}
            raw.append(record)
            r = selected['RESULT']
            okay = ('\nPASSED:' in text and 'RUNNER_EXIT=0 ' in text and r
                    and r.get('counter_status') == '0/0/0' and r.get('cycle_counter_status') == '0'
                    and r.get('route_counter_status') == '0/0')
            if not okay:
                rejected.append(record)
                continue
            row = {'cache': cache, **selected['WORKLOAD'], **selected['MEMORY'], **r,
                   'cycle_window': 'all_launches_post_readback_including_flush',
                   'log': str(path), 'log_sha256': record['log_sha256'],
                   'cohort': 'ordered_mp'}
            for key, value in list(row.items()):
                if isinstance(value, str) and value.isdecimal():
                    row[key] = int(value)
            rows.append(row)
    groups = {}
    for row in rows:
        key = (row['cache'], row['keys'], row['requests'], row['shape'], row['seed'])
        groups.setdefault(key, []).append(row)
    summaries = []
    for key, group in sorted(groups.items()):
        if len({row['input_hash'] for row in group}) != 1:
            raise RuntimeError('different numeric input')
        modes = {row['mode']: row['device_cycles_post_readback_including_flush'] for row in group}
        if len(modes) != len(group):
            raise RuntimeError('duplicate mode/case')
        summary = dict(zip(('cache', 'keys', 'requests', 'shape', 'seed'), key))
        summary['modes'] = modes
        if 'mp' in modes:
            best = min(('gpu_lock', 'gpu_owner'), key=modes.get)
            summary['best_measured_gpu'] = best
            summary['mp_over_best_measured_gpu'] = modes['mp'] / modes[best]
        summaries.append(summary)
    output = root / 'docs/proposals/forza_explore_compound_ordered_measurements'
    columns = list(dict.fromkeys(key for row in rows for key in row))
    with output.with_suffix('.csv').open('w', newline='') as file:
        writer = csv.DictWriter(file, fieldnames=columns)
        writer.writeheader()
        writer.writerows(rows)
    data = {'schema': 1, 'cohort': 'data_dependent_release',
            'primary_metric': 'device_cycles_post_readback_including_flush',
            'new_mp_runs': len(raw) - len(rejected), 'native_reference_rows': len(rows) - len(raw) + len(rejected),
            'rejected_count': len(rejected), 'rows': rows, 'raw_mp_logs': raw,
            'rejected': rejected, 'summaries': summaries,
            'reference_json': str(reference_path), 'reference_json_sha256': sha(reference_path)}
    output.with_suffix('.json').write_text(json.dumps(data, indent=2) + '\n')
    print(json.dumps({key: data[key] for key in ('new_mp_runs', 'native_reference_rows', 'rejected_count')}, indent=2))
    for summary in summaries:
        if summary['requests'] > 128:
            print(summary)


if __name__ == '__main__':
    main()
