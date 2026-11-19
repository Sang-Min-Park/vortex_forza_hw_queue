#!/usr/bin/env python3
"""Continue the same20 cases with three isolated simulator processes."""
import concurrent.futures
import csv
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time

ROOT = Path(__file__).resolve().parents[3]
BUILD = ROOT / 'build64-forza-messaging-isa'
PREVIOUS = BUILD / 'tests/regression/forza_pgas_feedback_heavy_validated'
APP = BUILD / 'tests/regression/forza_pgas_feedback_heavy_parallel'
REPORT = ROOT / 'docs/proposals/forza_pgas_feedback_heavy_validated_results'
MODES = ['native', 'native_packed', 'isa_wait_global', 'isa_wait_local', 'gpu_queue_local']


def fingerprint(path):
    path = Path(path).resolve()
    return dict(path=str(path), bytes=path.stat().st_size,
                sha256=hashlib.sha256(path.read_bytes()).hexdigest())


def verify(items):
    for item in items:
        assert fingerprint(item['path']) == {key: item[key] for key in ['path', 'bytes', 'sha256']}, item['path']


def main():
    assert not APP.exists()
    APP.mkdir(parents=True)
    previous = PREVIOUS / 'feedback-manifest.json'
    data = json.loads(previous.read_text())
    assert all(row['passed'] for row in data['runs'])
    shutil.copy2(REPORT.with_suffix('.json'), REPORT.with_name('forza_pgas_feedback_heavy_sequential_partial_results.json'))
    data['cohort'] = 'heavy_parallel'
    data['continuation'] = dict(previous_manifest=fingerprint(previous),
                                retained_completed_rows=len(data['runs']), process_limit=3,
                                reason='Wall-time scheduling only; identical hosts, guests and libraries')
    copied = []
    for item in data['artifacts']:
        src = Path(item['path'])
        assert fingerprint(src) == {key: item[key] for key in ['path', 'bytes', 'sha256']}
        dest = APP / 'artifacts' / src.parent.name / src.name
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dest)
        copied.append(dict(**fingerprint(dest), original=item.get('original', fingerprint(src))))
    data['artifacts'] = copied
    runner = Path(__file__).resolve()
    snapshot = APP / 'source-freeze' / runner.name
    snapshot.parent.mkdir(exist_ok=True)
    shutil.copy2(runner, snapshot)
    data['source_artifacts'] += [fingerprint(runner), fingerprint(snapshot)]
    frozen = data['source_artifacts'] + data['shared_artifacts'] + data['artifacts']
    env = os.environ.copy()
    env.pop('DEBUG', None)
    env.update(VORTEX_DRIVER='simx', LD_LIBRARY_PATH=str(BUILD / 'sw/runtime'))

    def save():
        text = json.dumps(data, indent=2) + '\n'
        (APP / 'feedback-manifest.json').write_text(text)
        REPORT.with_suffix('.json').write_text(text)
        rows = [dict(cohort=row['cohort'], **row['result'], wall_seconds=row['wall_seconds'], log=row['log'])
                for row in data['runs'] if row['result']]
        with REPORT.with_suffix('.csv').open('w', newline='') as f:
            writer = csv.DictWriter(f, fieldnames=list(rows[0]))
            writer.writeheader()
            writer.writerows(rows)

    def run(case):
        work, pattern, mode = case
        verify(frozen)
        directory = APP / 'runs' / f'update-{mode}-k64-s128-w{work}-{pattern}-seed17'
        directory.mkdir(parents=True)
        log = directory / 'run.log'
        artifact = APP / 'artifacts' / mode
        command = [str(artifact / ('forza_feedback_heavy_' + mode)), '-k', str(artifact / 'kernel.vxbin'),
                   '-n', '64', '-s', '128', '-w', str(work), '-o', 'update', '-p', pattern,
                   '-r', '17', '-t', '900']
        print('START', mode, work, pattern, flush=True)
        start = time.monotonic()
        timed_out = False
        with log.open('w') as f:
            try:
                code = subprocess.run(command, cwd=directory, env=env, stdout=f, stderr=subprocess.STDOUT, timeout=910).returncode
            except subprocess.TimeoutExpired:
                code, timed_out = -1, True
        lines = log.read_text().splitlines()
        fields = lambda line: dict(re.findall(r'(\w+)=([^\s]+)', line))
        results = [fields(line) for line in lines if line.startswith('RESULT ')]
        inputs = [fields(line) for line in lines if line.startswith('INPUT ')]
        verify(frozen)
        return dict(cohort='heavy', mode=mode, kind='update', keys=64, steps=128, work=work,
                    pattern=pattern, seed=17, command=command, cwd=str(directory), log=str(log),
                    returncode=code, timed_out=timed_out, wall_seconds=time.monotonic()-start,
                    passed=code == 0 and bool(results) and any(line.startswith('PASSED:') for line in lines),
                    result=results[-1] if results else None, input=inputs[-1] if inputs else None)

    done = {(row['work'], row['pattern'], row['mode']) for row in data['runs']}
    cases = [(work, pattern, mode) for work in [128, 512] for pattern in ['hot', 'random']
             for mode in MODES if (work, pattern, mode) not in done]
    save()
    with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
        futures = [pool.submit(run, case) for case in cases]
        for future in concurrent.futures.as_completed(futures):
            row = future.result()
            data['runs'].append(row)
            save()
            print('PASS' if row['passed'] else 'FAIL', row['mode'], row['work'], row['pattern'],
                  (row['result'] or {}).get('device_cycles_post_readback_including_flush'), flush=True)
            assert row['passed'], row['log']
    assert len(data['runs']) == 20
    data['completed_utc'] = datetime.datetime.now(datetime.timezone.utc).isoformat()
    save()
    print('COMPLETE', len(data['runs']), flush=True)


if __name__ == '__main__':
    main()
