#!/usr/bin/env python3
"""Run frozen compound transactions and verify benchmark artifacts do not change."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--build', required=True, type=Path)
    parser.add_argument('--cache', required=True, choices=['l1_off', 'l1_on'])
    parser.add_argument('--cases', default='capacity,hot')
    parser.add_argument('--modes', default='mp,gpu_lock,gpu_owner')
    parser.add_argument('--prefix', default='validation')
    args = parser.parse_args()
    build = args.build.resolve()
    app = build / 'tests/regression/forza_explore/compound_state'
    modes = args.modes.split(',')
    cases = {
        'capacity': (32768, 65536, 'uniform'),
        'hot': (32, 4096, 'hot'),
        'uniform_smoke': (64, 128, 'uniform'),
        'hot_smoke': (64, 128, 'hot'),
        'zero_smoke': (64, 0, 'hot'),
    }
    binaries = [build / 'sw/runtime' / name for name in
                ('libsimx.so', 'libvortex-simx.so', 'libvortex.so')]
    for mode in modes:
        binaries += [app / 'snapshots' / mode / ('forza_compound_' + mode),
                     app / 'snapshots' / mode / ('kernel-' + mode + '.vxbin')]
    fingerprints = {str(path): digest(path) for path in binaries}
    manifest = {'cache': args.cache, 'build': str(build), 'artifacts': fingerprints,
                'runs': []}
    manifest_path = app / (args.prefix + '-run-manifest.json')
    env = os.environ.copy()
    env['VORTEX_DRIVER'] = 'simx'
    env['LD_LIBRARY_PATH'] = str(build / 'sw/runtime')
    (app / 'logs').mkdir(exist_ok=True)
    for case in args.cases.split(','):
        keys, updates, shape = cases[case]
        for mode in modes:
            name = f'{args.prefix}-{args.cache}-{mode}-{case}.log'
            log_path = app / 'logs' / name
            if log_path.exists():
                raise RuntimeError(f'refusing to overwrite {log_path}')
            command = [str(app / 'snapshots' / mode / ('forza_compound_' + mode)),
                       '-k', str(app / 'snapshots' / mode / ('kernel-' + mode + '.vxbin')),
                       '-n', str(keys), '-u', str(updates), '-g', shape, '-s', '17', '-t', '600']
            started = time.time()
            with log_path.open('w') as log:
                log.write('COMMAND ' + json.dumps(command) + '\n')
                log.flush()
                try:
                    result = subprocess.run(command, cwd=app, env=env, stdout=log,
                                            stderr=subprocess.STDOUT, timeout=610)
                    status = result.returncode
                except subprocess.TimeoutExpired:
                    status = 124
                seconds = time.time() - started
                log.write(f'RUNNER_EXIT={status} WALL_SECONDS={seconds:.6f}\n')
            text = log_path.read_text()
            passed = status == 0 and '\nPASSED:' in text and '\nRESULT ' in text
            manifest['runs'].append({'case': case, 'mode': mode, 'command': command,
                                     'log': str(log_path), 'log_sha256': digest(log_path),
                                     'exit_code': status, 'passed': passed, 'wall_seconds': seconds})
            unchanged = all(digest(Path(path)) == sha for path, sha in fingerprints.items())
            manifest['artifacts_unchanged'] = unchanged
            manifest_path.write_text(json.dumps(manifest, indent=2) + '\n')
            print(f'{args.cache} {mode} {case}: {"PASS" if passed else "FAIL"} '
                  f'{seconds:.2f}s', flush=True)
            if not unchanged:
                raise RuntimeError('benchmark artifact changed during measurements')
    if not all(run['passed'] for run in manifest['runs']):
        raise SystemExit(1)


if __name__ == '__main__':
    main()
