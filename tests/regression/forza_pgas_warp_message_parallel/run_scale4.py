#!/usr/bin/env python3
"""Bounded four-core scaling check using the frozen generic ISA machine."""
import argparse
import csv
import datetime
import hashlib
import json
import os
import re
import subprocess
import time
from pathlib import Path

SOURCE = Path(__file__).resolve().parent
ROOT = SOURCE.parents[2]
MAIN = ROOT / 'tests/regression/forza_pgas_warp_message'
MODES = ['gpu_lock', 'gpu_warp', 'gpu_owner', 'field_mmio8', 'warp_wait32']


def fingerprint(path):
    path = Path(path).resolve()
    return {'path': str(path), 'bytes': path.stat().st_size,
            'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}


def fields(line):
    return dict(re.findall(r'(\w+)=([^\s]+)', line))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    args = parser.parse_args()
    build = args.build_dir.resolve()
    work = build / 'tests/regression/forza_pgas_warp_message_scaling_four'
    work.mkdir(parents=True, exist_ok=True)
    manifest = work / 'scale4-manifest.json'
    if manifest.exists():
        raise RuntimeError('Refusing to replace a frozen scaling cohort')
    env = os.environ.copy()
    env.pop('DEBUG', None)
    env.update(VORTEX_DRIVER='simx', LD_LIBRARY_PATH=str(build / 'sw/runtime'))
    cohorts = []
    variants = []
    for group, source, modes, project in [
            ('main', MAIN, MODES, 'forza_warp_message_'),
            ('parallel', SOURCE, ['warp_wait32'], 'forza_warp_parallel_')]:
        report = ROOT / f'docs/proposals/forza_pgas_warp_message_scale4_build_{group}'
        command = ['python3', str(source / 'run_matrix.py'), '--build-dir', str(build),
                   '--stage', 'build', '--cohort', 'scale4', '--cores', '4',
                   '--modes', ','.join(modes), '--report-prefix', str(report)]
        log = work / f'build-{group}.log'
        app = build / f'tests/regression/{source.name}_scale4'
        component = app / 'record-manifest.json'
        reused = component.exists()
        if reused:
            log.write_text('Reusing frozen component build: ' + str(component) + '\n')
        else:
            with log.open('w') as output:
                subprocess.run(command, cwd=build, env=env, stdout=output,
                               stderr=subprocess.STDOUT, check=True)
        data = json.loads(component.read_text())
        cohorts.append({'group': group, 'command': command,
                        'build_reused': reused,
                        'log': fingerprint(log), 'manifest': fingerprint(component),
                        'data': data})
        for mode in modes:
            variant = mode if group == 'main' else 'parallel_' + mode
            binary = app / 'artifacts' / mode / (project + mode)
            variants.append((variant, mode, binary))

    data = {'started_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
            'build_dir': str(build), 'cores': 4, 'cohorts': cohorts,
            'runner': fingerprint(__file__), 'runs': []}
    report = ROOT / 'docs/proposals/forza_pgas_warp_message_scale4_results'

    def verify():
        for cohort in cohorts:
            original = cohort['data']
            for item in (original['source_artifacts'] + original['shared_artifacts']
                         + original['artifacts']):
                if fingerprint(item['path']) != {key: item[key] for key in ['path', 'bytes', 'sha256']}:
                    raise RuntimeError('Frozen artifact changed: ' + item['path'])
                if item.get('snapshot') and fingerprint(item['snapshot'])['sha256'] != item['sha256']:
                    raise RuntimeError('Source snapshot changed: ' + item['snapshot'])

    def save():
        text = json.dumps(data, indent=2) + '\n'
        manifest.write_text(text)
        report.with_suffix('.json').write_text(text)
        rows = [{'cohort': run['cohort'], 'variant': run['variant'], **run['result'],
                 'wall_seconds': run['wall_seconds'], 'log': run['log']}
                for run in data['runs'] if run.get('result')]
        if rows:
            with report.with_suffix('.csv').open('w', newline='') as output:
                writer = csv.DictWriter(output, fieldnames=list(rows[0]))
                writer.writeheader()
                writer.writerows(rows)

    cases = [('smoke', count, 'random') for count in [0, 1, 131]]
    cases += [('performance', 4096, pattern) for pattern in ['random', 'hotset', 'hot']]
    for cohort, count, pattern in cases:
        for variant, mode, binary in variants:
            verify()
            directory = work / 'runs' / f'{cohort}-{variant}-n{count}-{pattern}'
            directory.mkdir(parents=True, exist_ok=True)
            log = directory / 'run.log'
            command = [str(binary), '-k', str(binary.parent / 'kernel.vxbin'),
                       '-n', '64', '-u', str(count), '-p', pattern,
                       '-d', 'wide', '-s', '17', '-t', '600']
            print('START', cohort, variant, count, pattern, flush=True)
            start = time.monotonic()
            with log.open('w') as output:
                try:
                    code = subprocess.run(command, cwd=directory, env=env, stdout=output,
                                          stderr=subprocess.STDOUT, timeout=610).returncode
                except subprocess.TimeoutExpired:
                    code = -1
            lines = log.read_text().splitlines()
            results = [fields(line) for line in lines if line.startswith('RESULT ')]
            inputs = [fields(line) for line in lines if line.startswith('INPUT ')]
            passed = (code == 0 and bool(results) and bool(inputs)
                      and inputs[-1].get('cores') == '4'
                      and any(line.startswith('PASSED:') for line in lines))
            row = {'cohort': cohort, 'variant': variant, 'mode': mode, 'keys': 64,
                   'requests': count, 'pattern': pattern, 'seed': 17,
                   'command': command, 'cwd': str(directory), 'log': str(log),
                   'returncode': code, 'passed': bool(passed),
                   'wall_seconds': time.monotonic() - start,
                   'result': results[-1] if results else None,
                   'input': inputs[-1] if inputs else None}
            data['runs'].append(row)
            verify()
            save()
            print('PASS' if passed else 'FAIL', variant, row['result'], flush=True)
            if not passed:
                raise SystemExit('Run failed: ' + str(log))
    data['completed_utc'] = datetime.datetime.now(datetime.timezone.utc).isoformat()
    save()
    print('COMPLETE', len(data['runs']), 'cases', flush=True)


if __name__ == '__main__':
    main()
