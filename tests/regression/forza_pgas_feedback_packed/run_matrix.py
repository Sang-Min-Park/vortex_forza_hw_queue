#!/usr/bin/env python3
"""Dependent client chains with native GPU and generic owner-message handlers."""
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
MODES = ['native_packed']
CONFIG = ('-DVX_CFG_FORZA_ISA_ENABLE -DVX_CFG_FORZA_MMIO_VECTOR_ENABLE '
          '-DVX_CFG_FORZA_MMIO_ENABLE -DVX_CFG_EXT_FORZA_ENABLE '
          '-DVX_CFG_EXT_A_ENABLE -DVX_CFG_DCACHE_DISABLE -DVX_CFG_L2_ENABLE '
          '-DVX_CFG_NUM_CORES=2 -DVX_CFG_NUM_WARPS=2 -DVX_CFG_NUM_THREADS=4')


def fingerprint(path):
    path = Path(path).resolve()
    return {'path': str(path), 'bytes': path.stat().st_size,
            'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}


def fields(line):
    return dict(re.findall(r'(\w+)=([^\s]+)', line))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--stage', choices=['build', 'smoke', 'run', 'all'], default='all')
    parser.add_argument('--suite', choices=['quick', 'full'], default='quick')
    parser.add_argument('--modes', default=','.join(MODES))
    parser.add_argument('--cohort', default='v1')
    parser.add_argument('--timeout', type=int, default=180)
    parser.add_argument('--seed', type=int, default=17)
    parser.add_argument('--skip-smoke', action='store_true')
    args = parser.parse_args()
    selected = args.modes.split(',')
    if any(mode not in MODES for mode in selected): parser.error('unknown mode')
    if not re.fullmatch(r'[a-z0-9_]+', args.cohort): parser.error('unsafe cohort')
    build = args.build_dir.resolve()
    app = build / ('tests/regression/forza_pgas_feedback_packed_' + args.cohort)
    libs = build / 'sw/runtime'
    manifest = app / 'feedback-manifest.json'
    report = ROOT / 'docs/proposals' / ('forza_pgas_feedback_packed_' + args.cohort + '_results')
    env = os.environ.copy()
    env.pop('DEBUG', None)
    env.update(CONFIGS=CONFIG, VORTEX_DRIVER='simx', LD_LIBRARY_PATH=str(libs))

    def save(data):
        text = json.dumps(data, indent=2) + '\n'
        manifest.write_text(text)
        report.with_suffix('.json').write_text(text)
        rows = [{'cohort': row['cohort'], **row['result'], 'wall_seconds': row['wall_seconds'],
                 'log': row['log']} for row in data['runs'] if row.get('result')]
        if rows:
            with report.with_suffix('.csv').open('w', newline='') as output:
                writer = csv.DictWriter(output, fieldnames=list(rows[0]))
                writer.writeheader(); writer.writerows(rows)

    if args.stage in ['build', 'all']:
        if manifest.exists(): raise RuntimeError('Use a fresh cohort instead of replacing a manifest')
        app.mkdir(parents=True, exist_ok=True)
        (app / 'logs').mkdir(exist_ok=True)
        shutil.copy2(SOURCE / 'Makefile', app / 'Makefile')
        sources = list(SOURCE.glob('*.cpp')) + list(SOURCE.glob('*.h'))
        sources += [SOURCE / name for name in ['Makefile', 'README.md', 'run_matrix.py']]
        sources += [ROOT / 'sw/common' / name for name in
                    ['forza_abi.h', 'forza_mmio_abi.h', 'forza_isa_abi.h']]
        for name in ['sim/simx/forza/forza_zone.cpp', 'sim/simx/forza/forza_zone.h',
                     'sim/simx/forza/forza_types.h', 'sim/simx/forza/forza_fabric.cpp',
                     'sim/simx/decode.cpp', 'sim/simx/decode.h', 'sim/simx/sfu_unit.cpp',
                     'sim/simx/sfu_unit.h', 'sim/simx/instr_trace.h', 'sim/simx/instr.h',
                     'sim/simx/types.h', 'VX_config.toml']:
            path = ROOT / name
            if path.exists(): sources.append(path)
        required = [libs / name for name in ['libsimx.so', 'libvortex.so', 'libvortex-simx.so']]
        required += [build / 'sw/kernel/libvortex2.a']
        source_rows = []
        for source in sources:
            row = fingerprint(source)
            snapshot = app / 'source-freeze' / source.relative_to(ROOT)
            snapshot.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, snapshot)
            row['snapshot'] = str(snapshot)
            source_rows.append(row)
        data = {'started_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
                'cohort': args.cohort, 'build_dir': str(build), 'configs': CONFIG,
                'source_artifacts': source_rows,
                'shared_artifacts': [fingerprint(path) for path in required],
                'builds': [], 'artifacts': [], 'runs': []}
        for mode in selected:
            command = ['make', '-C', str(app), 'MODE=' + mode,
                       '-o', str(libs / 'libvortex.so'), '-o', str(build / 'sw/kernel/libvortex2.a')]
            log = app / 'logs' / ('build-' + mode + '.log')
            with log.open('w') as output:
                subprocess.run(command + ['clean'], env=env, stdout=output,
                               stderr=subprocess.STDOUT, check=True)
                subprocess.run(command + ['-j2'], env=env, stdout=output,
                               stderr=subprocess.STDOUT, check=True)
            target = app / 'artifacts' / mode
            target.mkdir(parents=True, exist_ok=True)
            for name in ['forza_feedback_packed_' + mode, 'kernel.vxbin', 'kernel.elf', 'kernel.dump']:
                shutil.copy2(app / name, target / name)
                data['artifacts'].append(fingerprint(target / name))
            data['builds'].append({'mode': mode, 'commands': [command + ['clean'], command + ['-j2']],
                                   'log': fingerprint(log)})
            save(data)
            print('BUILT', mode, flush=True)
    else:
        data = json.loads(manifest.read_text())

    def verify():
        for row in data['source_artifacts'] + data['shared_artifacts'] + data['artifacts']:
            actual = fingerprint(row['path'])
            if any(actual[key] != row[key] for key in actual):
                raise RuntimeError('Frozen artifact changed: ' + row['path'])
            if row.get('snapshot') and fingerprint(row['snapshot'])['sha256'] != row['sha256']:
                raise RuntimeError('Frozen source snapshot changed')

    verify()
    if args.stage == 'build': return
    smokes = [('walk', 0, 1, 'random'), ('walk', 1, 1, 'random'),
              ('walk', 7, 1, 'hot'), ('walk', 64, 1, 'random'),
              ('update', 0, 1, 'random'), ('update', 1, 1, 'random'),
              ('update', 7, 1, 'hot'), ('update', 64, 1, 'hot')]
    cases = [] if args.skip_smoke else [('smoke', mode, *case) for case in smokes for mode in selected]
    if args.stage != 'smoke':
        primary = [(kind, 128, 1, pattern) for kind in ['walk', 'update'] for pattern in ['random', 'hot']]
        primary += [(kind, 512, 1, 'hot') for kind in ['walk', 'update']]
        if args.suite == 'full':
            primary += [(kind, 128, work, pattern) for kind in ['walk', 'update']
                        for work in [8, 32] for pattern in ['random', 'hot']]
            primary += [(kind, 512, 1, 'random') for kind in ['walk', 'update']]
            primary += [(kind, 512, 8, 'hot') for kind in ['walk', 'update']]
        cases += [('performance', mode, *case) for case in primary for mode in selected]
    cases = [case for case in cases if case[2] == 'update']
    done = {(row['cohort'], row['mode'], row['handler'], row['steps'], row['work'],
             row['pattern'], row['seed']) for row in data['runs']}
    for cohort, mode, kind, steps, work_count, pattern in cases:
        identity = (cohort, mode, kind, steps, work_count, pattern, args.seed)
        if identity in done: continue
        verify()
        work_dir = app / 'runs' / f'{cohort}-{mode}-{kind}-s{steps}-w{work_count}-{pattern}-seed{args.seed}'
        work_dir.mkdir(parents=True, exist_ok=True)
        log = work_dir / 'run.log'
        binary = app / 'artifacts' / mode / ('forza_feedback_packed_' + mode)
        timeout = min(args.timeout, 60) if cohort == 'smoke' else args.timeout
        command = [str(binary), '-k', str(binary.parent / 'kernel.vxbin'), '-n', '64',
                   '-s', str(steps), '-w', str(work_count), '-o', kind, '-p', pattern,
                   '-r', str(args.seed), '-t', str(timeout)]
        print('START', *identity, flush=True)
        start = time.monotonic()
        timed_out = False
        with log.open('w') as output:
            try:
                code = subprocess.run(command, cwd=work_dir, env=env, stdout=output,
                                      stderr=subprocess.STDOUT, timeout=timeout + 10).returncode
            except subprocess.TimeoutExpired:
                code = -1; timed_out = True
        lines = log.read_text().splitlines()
        results = [fields(line) for line in lines if line.startswith('RESULT ')]
        inputs = [fields(line) for line in lines if line.startswith('INPUT ')]
        row = {'cohort': cohort, 'mode': mode, 'handler': kind, 'steps': steps,
               'work': work_count, 'pattern': pattern, 'seed': args.seed,
               'command': command, 'cwd': str(work_dir), 'log': str(log),
               'log_artifact': fingerprint(log), 'returncode': code, 'timed_out': timed_out,
               'wall_seconds': time.monotonic() - start,
               'passed': code == 0 and bool(results) and any(line.startswith('PASSED:') for line in lines),
               'result': results[-1] if results else None, 'input': inputs[-1] if inputs else None}
        data['runs'].append(row)
        verify(); save(data)
        print('PASS' if row['passed'] else 'FAIL', identity, row['result'], flush=True)
        if not row['passed']: raise SystemExit('Run failed: ' + str(log))
    data['completed_utc'] = datetime.datetime.now(datetime.timezone.utc).isoformat()
    save(data)
    print('COMPLETE', len(data['runs']), 'cases', flush=True)


if __name__ == '__main__': main()
