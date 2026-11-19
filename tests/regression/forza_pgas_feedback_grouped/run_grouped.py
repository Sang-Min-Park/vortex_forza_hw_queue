#!/usr/bin/env python3
"""Bounded grouped GPU control for feedback chains; leaves frozen controls intact."""
import argparse
import csv
import datetime
import hashlib
import json
import os
import re
import shutil
import subprocess
import time
from pathlib import Path

SOURCE = Path(__file__).resolve().parent
ROOT = SOURCE.parents[2]
CONFIG = ('-DVX_CFG_FORZA_ISA_ENABLE -DVX_CFG_FORZA_MMIO_VECTOR_ENABLE '
          '-DVX_CFG_FORZA_MMIO_ENABLE -DVX_CFG_EXT_FORZA_ENABLE '
          '-DVX_CFG_EXT_A_ENABLE -DVX_CFG_DCACHE_DISABLE -DVX_CFG_L2_ENABLE '
          '-DVX_CFG_NUM_CORES=2 -DVX_CFG_NUM_WARPS=2 -DVX_CFG_NUM_THREADS=4')


def fingerprint(path):
    path = Path(path).resolve()
    return {'path': str(path), 'bytes': path.stat().st_size,
            'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--cohort', default='validated')
    args = parser.parse_args()
    if not re.fullmatch(r'[a-z0-9_]+', args.cohort): parser.error('invalid cohort')
    build = args.build_dir.resolve()
    app = build / ('tests/regression/forza_pgas_feedback_grouped_' + args.cohort)
    app.mkdir(parents=True, exist_ok=False)
    manifest = app / 'feedback-manifest.json'
    report = ROOT / 'docs/proposals' / ('forza_pgas_feedback_grouped_' + args.cohort + '_results')
    env = os.environ.copy()
    env.pop('DEBUG', None)
    env.update(CONFIGS=CONFIG, VORTEX_DRIVER='simx', LD_LIBRARY_PATH=str(build / 'sw/runtime'))
    libraries = [build / 'sw/runtime' / name for name in ['libsimx.so', 'libvortex.so', 'libvortex-simx.so']]
    libraries += [build / 'sw/kernel/libvortex2.a']
    source_files = sorted(SOURCE.glob('*'))
    sources = []
    for source in source_files:
        if not source.is_file(): continue
        row = fingerprint(source)
        snapshot = app / 'source-freeze' / source.relative_to(ROOT)
        snapshot.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, snapshot)
        row['snapshot'] = str(snapshot)
        sources.append(row)
    (app / 'logs').mkdir()
    shutil.copy2(SOURCE / 'Makefile', app / 'Makefile')
    data = {'started_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
            'cohort': args.cohort, 'configs': CONFIG, 'source_artifacts': sources,
            'shared_artifacts': [fingerprint(path) for path in libraries],
            'builds': [], 'artifacts': [], 'runs': [],
            'contract': {'clients': 8, 'keys': 64, 'steps': 'response dependent per client',
                         'handler': 'same update_seed, updated_value, update_next semantics',
                         'grouping': 'one lock acquisition and commit per same-key warp group',
                         'parallel_prepare': 'update_seed hoisted before lock using immutable next field',
                         'extra_sram_bytes': 0, 'per_hop_global_barriers': 0,
                         'host_change': 'max work increased from 64 to 512 only'}}

    def save():
        text = json.dumps(data, indent=2) + '\n'
        manifest.write_text(text)
        report.with_suffix('.json').write_text(text)
        rows = [{'cohort': row['cohort'], **row['result'], 'wall_seconds': row['wall_seconds'],
                 'log': row['log']} for row in data['runs'] if row.get('result')]
        if rows:
            with report.with_suffix('.csv').open('w', newline='') as output:
                writer = csv.DictWriter(output, fieldnames=list(rows[0]))
                writer.writeheader(); writer.writerows(rows)

    def verify():
        for row in data['source_artifacts'] + data['shared_artifacts'] + data['artifacts']:
            actual = fingerprint(row['path'])
            if actual != {key: row[key] for key in actual}:
                raise RuntimeError('Frozen artifact changed: ' + row['path'])

    command = ['make', '-C', str(app), 'MODE=native_grouped', '-o', str(libraries[1]),
               '-o', str(libraries[3]), '-j2']
    log = app / 'logs/build.log'
    with log.open('w') as output:
        subprocess.run(command, env=env, stdout=output, stderr=subprocess.STDOUT, check=True)
    data['builds'].append({'command': command, 'log': fingerprint(log)})
    artifact = app / 'artifacts/native_grouped'
    artifact.mkdir(parents=True)
    for name in ['forza_feedback_native_grouped', 'kernel.vxbin', 'kernel.elf', 'kernel.dump']:
        shutil.copy2(app / name, artifact / name)
        data['artifacts'].append(fingerprint(artifact / name))
    save()
    cases = [('smoke', steps, 1, pattern) for steps, pattern in [(0, 'random'), (1, 'random'), (7, 'hot')]]
    cases += [('performance', 128, work, pattern) for work in [1, 8, 32] for pattern in ['random', 'hot']]
    cases += [('heavy', 128, work, pattern) for work in [128, 512] for pattern in ['random', 'hot']]
    for cohort, steps, work, pattern in cases:
        verify()
        workdir = app / 'runs' / f'{cohort}-native_grouped-update-s{steps}-w{work}-{pattern}-seed17'
        workdir.mkdir(parents=True)
        log = workdir / 'run.log'
        timeout = 900 if cohort == 'heavy' else 180
        command = [str(artifact / 'forza_feedback_native_grouped'), '-k', str(artifact / 'kernel.vxbin'),
                   '-n64', '-s' + str(steps), '-w' + str(work), '-oupdate', '-p' + pattern, '-r17', '-t' + str(timeout)]
        print('START', cohort, steps, work, pattern, flush=True)
        start = time.monotonic()
        with log.open('w') as output:
            try:
                code = subprocess.run(command, cwd=workdir, env=env, stdout=output,
                                      stderr=subprocess.STDOUT, timeout=timeout + 10).returncode
            except subprocess.TimeoutExpired:
                code = -1
        text = log.read_text()
        results = [dict(re.findall(r'(\w+)=([^\s]+)', line)) for line in text.splitlines() if line.startswith('RESULT ')]
        passed = code == 0 and 'PASSED:' in text and len(results) == 1
        data['runs'].append({'cohort': cohort, 'mode': 'native_grouped', 'handler': 'update',
                             'steps': steps, 'work': work, 'pattern': pattern, 'seed': 17,
                             'command': command, 'cwd': str(workdir), 'log': str(log),
                             'log_artifact': fingerprint(log), 'passed': passed, 'returncode': code,
                             'result': results[0] if results else None, 'wall_seconds': time.monotonic() - start})
        save()
        print('PASS' if passed else 'FAIL', steps, work, pattern,
              results[0]['device_cycles_post_readback_including_flush'] if results else '', flush=True)
        if not passed: raise RuntimeError('Exact validation failed: ' + str(log))
    verify()
    data['completed_utc'] = datetime.datetime.now(datetime.timezone.utc).isoformat()
    save()
    print('ALL_PASS', len(data['runs']), str(manifest), flush=True)


if __name__ == '__main__': main()
