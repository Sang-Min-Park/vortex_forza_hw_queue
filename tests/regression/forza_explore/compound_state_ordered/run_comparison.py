#!/usr/bin/env python3
"""Run corrected compound MP with immutable original native GPU references."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--cache', choices=['l1_off', 'l1_on'], required=True)
    parser.add_argument('--cases', default='hot,capacity,uniform_smoke,hot_smoke,zero_smoke')
    parser.add_argument('--prefix', default='validation')
    args = parser.parse_args()
    build = args.build.resolve()
    app = build / 'tests/regression/forza_explore/compound_state_ordered'
    old = build / 'tests/regression/forza_explore/compound_state'
    host = app / 'artifacts/mp/forza_compound_ordered_mp'
    kernel = app / 'artifacts/mp/kernel-ordered-mp.vxbin'
    paths = [host, kernel]
    paths += [build / 'sw/runtime' / filename for filename in ('libsimx.so', 'libvortex-simx.so', 'libvortex.so')]
    for mode in ('gpu_lock', 'gpu_owner'):
        paths += [old / 'snapshots' / mode / ('forza_compound_' + mode),
                  old / 'snapshots' / mode / ('kernel-' + mode + '.vxbin')]
    hashes = {str(path): sha(path) for path in paths}
    cases = {'hot': (32, 4096, 'hot'), 'capacity': (32768, 65536, 'uniform'),
             'uniform_smoke': (64, 128, 'uniform'), 'hot_smoke': (64, 128, 'hot'),
             'zero_smoke': (64, 0, 'hot')}
    env = os.environ.copy()
    env['VORTEX_DRIVER'] = 'simx'
    env['LD_LIBRARY_PATH'] = str(build / 'sw/runtime')
    data = {'cohort': 'data_dependent_release', 'cache': args.cache,
            'artifacts': hashes, 'runs': []}
    output = app / (args.prefix + '-run-manifest.json')
    (app / 'logs').mkdir(exist_ok=True)
    for case in args.cases.split(','):
        keys, updates, shape = cases[case]
        log_path = app / 'logs' / f'{args.prefix}-{args.cache}-mp-{case}.log'
        if log_path.exists():
            raise RuntimeError(f'refusing overwrite: {log_path}')
        command = [str(host), '-k', str(kernel), '-n', str(keys), '-u', str(updates),
                   '-g', shape, '-s', '17', '-t', '600']
        start = time.time()
        with log_path.open('w') as log:
            log.write('COMMAND ' + json.dumps(command) + '\n')
            log.flush()
            try:
                result = subprocess.run(command, cwd=app, env=env, stdout=log,
                                        stderr=subprocess.STDOUT, timeout=610)
                status = result.returncode
            except subprocess.TimeoutExpired:
                status = 124
            seconds = time.time() - start
            log.write(f'RUNNER_EXIT={status} WALL_SECONDS={seconds:.6f}\n')
        passed = status == 0 and '\nPASSED:' in log_path.read_text() and '\nRESULT ' in log_path.read_text()
        data['runs'].append({'case': case, 'command': command, 'log': str(log_path),
                             'log_sha256': sha(log_path), 'exit_code': status,
                             'wall_seconds': seconds, 'passed': passed})
        data['artifacts_unchanged'] = all(sha(Path(path)) == digest for path, digest in hashes.items())
        output.write_text(json.dumps(data, indent=2) + '\n')
        print(f'{args.cache} ordered MP {case}: {"PASS" if passed else "FAIL"} {seconds:.2f}s', flush=True)
        if not data['artifacts_unchanged']:
            raise RuntimeError('measured artifact changed')
    if not all(run['passed'] for run in data['runs']):
        raise SystemExit(1)


if __name__ == '__main__':
    main()
