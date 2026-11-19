#!/usr/bin/env python3
"""Two uploaded region programs, strong GPU controls, mailbox and direct completion."""
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
MODES = ['p2_'+m for m in ['gpu_lock','gpu_warp','gpu_owner','gpu_lock_pad','gpu_warp_pad','gpu_owner_pad','vm_mailbox','vm_direct']]
def project(mode): return 'forza_vm_branch_'+mode[1]+'_'+mode[3:]
CONFIG = ('-DVX_CFG_FORZA_VM_ENABLE '
          '-DVX_CFG_FORZA_MMIO_VECTOR_ENABLE -DVX_CFG_FORZA_MMIO_ENABLE '
          '-DVX_CFG_EXT_FORZA_ENABLE -DVX_CFG_EXT_A_ENABLE '
          '-DVX_CFG_DCACHE_DISABLE -DVX_CFG_L2_ENABLE '
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
    parser.add_argument('--modes', default=','.join(MODES))
    parser.add_argument('--sizes', default='64,256')
    parser.add_argument('--cohort', default='v1')
    parser.add_argument('--skip-smoke', action='store_true')
    parser.add_argument('--patterns', default='random,hotset,hot')
    parser.add_argument('--seed', type=int, default=17)
    parser.add_argument('--requests', type=int, default=4096)
    parser.add_argument('--timeout', type=int, default=600)
    parser.add_argument('--report-prefix', type=Path,
                        default=ROOT / 'docs/proposals/forza_pgas_vm_branch_results')
    args = parser.parse_args()
    selected = args.modes.split(',')
    sizes = [int(value) for value in args.sizes.split(',')]
    patterns = args.patterns.split(',')
    if any(mode not in MODES for mode in selected):
        parser.error('unknown mode')
    if any(pattern not in ['random', 'hotset', 'hot'] for pattern in patterns):
        parser.error('unknown pattern')
    build = args.build_dir.resolve()
    app = build / ('tests/regression/forza_pgas_vm_branch_'+args.cohort)
    libs = build / 'sw/runtime'
    manifest = app / 'vm-manifest.json'
    report = args.report_prefix.resolve()
    env = os.environ.copy()
    env.pop('DEBUG', None)
    env.update(CONFIGS=CONFIG, VORTEX_DRIVER='simx', LD_LIBRARY_PATH=str(libs))

    def save(data):
        text = json.dumps(data, indent=2) + '\n'
        manifest.write_text(text)
        report.with_suffix('.json').write_text(text)
        rows = [{'cohort': run['cohort'], **run['result'],
                 'wall_seconds': run['wall_seconds'], 'log': run['log']}
                for run in data['runs'] if run.get('result')]
        if rows:
            with report.with_suffix('.csv').open('w', newline='') as output:
                writer = csv.DictWriter(output, fieldnames=list(rows[0]))
                writer.writeheader()
                writer.writerows(rows)

    if args.stage in ['build', 'all']:
        if manifest.exists():
            raise RuntimeError('Refusing to replace existing manifest; use a fresh build directory')
        app.mkdir(parents=True, exist_ok=True)
        (app / 'logs').mkdir(exist_ok=True)
        shutil.copy2(SOURCE / 'Makefile', app / 'Makefile')
        required = [libs / name for name in ['libsimx.so', 'libvortex.so', 'libvortex-simx.so']]
        required.append(build / 'sw/kernel/libvortex2.a')
        sources = list(SOURCE.glob('*.cpp')) + list(SOURCE.glob('*.h'))
        sources += [SOURCE / name for name in ['Makefile', 'README.md', 'run_matrix.py']]
        sources += [ROOT / 'sw/common/forza_vm_abi.h', ROOT / 'sw/common/forza_record_abi.h', ROOT / 'sw/common/forza_mmio_abi.h',
                    ROOT / 'sim/simx/forza/forza_vm.h', ROOT / 'sim/simx/forza/forza_vm.cpp',
                    ROOT / 'sim/simx/types.h', ROOT / 'sim/simx/core.cpp', ROOT / 'sim/simx/processor.cpp',
                    ROOT / 'sim/simx/Makefile',
                    ROOT / 'sw/common/forza_abi.h', ROOT / 'sw/common/forza_update_abi.h',
                    ROOT / 'sim/simx/types.h',
                    ROOT / 'sim/simx/forza/forza_zone.cpp', ROOT / 'sim/simx/forza/forza_zone.h',
                    ROOT / 'sim/simx/forza/forza_fabric.cpp', ROOT / 'sim/simx/forza/forza_fabric.h',
                    ROOT / 'sim/simx/forza/forza_types.h', ROOT / 'VX_config.toml']
        data = {'started_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
                'build_dir': str(build), 'configs': CONFIG,
                'environment_overrides': {'CONFIGS': CONFIG, 'VORTEX_DRIVER': 'simx',
                                          'LD_LIBRARY_PATH': str(libs)},
                'source_artifacts': [fingerprint(path) for path in sources],
                'shared_artifacts': [fingerprint(path) for path in required],
                'builds': [], 'artifacts': [], 'runs': []}
        for source in sources:
            snapshot = app / 'source-snapshot' / source.relative_to(ROOT)
            snapshot.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, snapshot)
            data['artifacts'].append(fingerprint(snapshot))
        for mode in MODES:
            command = ['make', '-C', str(app), 'MODE=' + mode[3:], 'PROGRAM=' + mode[1],
                       '-o', str(libs / 'libvortex.so'),
                       '-o', str(build / 'sw/kernel/libvortex2.a')]
            log = app / 'logs' / ('build-' + mode + '.log')
            with log.open('w') as output:
                subprocess.run(command + ['clean'], env=env, stdout=output,
                               stderr=subprocess.STDOUT, check=True)
                subprocess.run(command + ['-j2'], env=env, stdout=output,
                               stderr=subprocess.STDOUT, check=True)
            target = app / 'artifacts' / mode
            target.mkdir(parents=True, exist_ok=True)
            for filename in [project(mode), 'kernel.vxbin', 'kernel.elf', 'kernel.dump']:
                shutil.copy2(app / filename, target / filename)
                data['artifacts'].append(fingerprint(target / filename))
            data['builds'].append({'mode': mode, 'commands': [command + ['clean'], command + ['-j2']],
                                   'log': fingerprint(log)})
            save(data)
            print('BUILT', mode, flush=True)
    else:
        data = json.loads(manifest.read_text())

    def verify():
        for item in data['source_artifacts'] + data['shared_artifacts'] + data['artifacts']:
            if fingerprint(item['path']) != item:
                raise RuntimeError('Frozen artifact changed: ' + item['path'])

    verify()
    if args.stage == 'build':
        return
    cases = [('smoke', mode, 64, count, pattern, args.seed)
             for count, pattern in ([] if args.skip_smoke else [(0, 'random'), (1, 'random'), (128, 'random'), (131, 'hot')])
             for mode in selected]
    if args.stage != 'smoke':
        cases += [('performance', mode, size, args.requests, pattern, args.seed)
                  for size in sizes for pattern in patterns for mode in selected]
    done = {(run['cohort'], run['mode'], run['keys'], run['requests'], run['pattern'], run['seed'])
            for run in data['runs']}
    for cohort, mode, keys, count, pattern, seed in cases:
        if (cohort, mode, keys, count, pattern, seed) in done:
            continue
        verify()
        work = app / 'runs' / f'{cohort}-{mode}-k{keys}-u{count}-{pattern}-seed{seed}-wide'
        work.mkdir(parents=True, exist_ok=False)
        log = work / 'run.log'
        binary = app / 'artifacts' / mode / (project(mode))
        command = [str(binary), '-k', str(binary.parent / 'kernel.vxbin'),
                   '-n', str(keys), '-u', str(count), '-p', pattern, '-d', 'wide',
                   '-s', str(seed), '-t', str(args.timeout)]
        print('START', cohort, mode, keys, count, pattern, seed, flush=True)
        start = time.monotonic()
        timed_out = False
        with log.open('w') as output:
            try:
                returncode = subprocess.run(command, cwd=work, env=env, stdout=output,
                                            stderr=subprocess.STDOUT,
                                            timeout=args.timeout + 10).returncode
            except subprocess.TimeoutExpired:
                returncode = -1
                timed_out = True
        lines = log.read_text().splitlines()
        results = [fields(line) for line in lines if line.startswith('RESULT ')]
        inputs = [fields(line) for line in lines if line.startswith('INPUT ')]
        row = {'cohort': cohort, 'mode': mode, 'keys': keys, 'requests': count,
               'pattern': pattern, 'seed': seed, 'delta_distribution': 'wide',
               'command': command, 'cwd': str(work), 'log': str(log),
               'log_artifact': fingerprint(log), 'returncode': returncode,
               'timed_out': timed_out, 'wall_seconds': time.monotonic() - start,
               'passed': returncode == 0 and bool(results)
                         and any(line.startswith('PASSED:') for line in lines),
               'result': results[-1] if results else None,
               'input': inputs[-1] if inputs else None}
        data['runs'].append(row)
        verify()
        save(data)
        print('PASS' if row['passed'] else 'FAIL', mode,
              'wall', round(row['wall_seconds'], 3), row['result'], flush=True)
        if not row['passed']:
            raise SystemExit('Run failed: ' + str(log))
    data['completed_utc'] = datetime.datetime.now(datetime.timezone.utc).isoformat()
    save(data)
    print('COMPLETE', len(data['runs']), 'cases', flush=True)


if __name__ == '__main__':
    main()
