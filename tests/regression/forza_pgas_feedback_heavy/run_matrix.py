#!/usr/bin/env python3
"""Host-bound-only extension of the validated feedback workload."""
import csv
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import time

SOURCE = Path(__file__).resolve().parent
ROOT = SOURCE.parents[2]
BUILD = ROOT / 'build64-forza-messaging-isa'
APP = BUILD / 'tests/regression/forza_pgas_feedback_heavy_validated'
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
    assert not APP.exists(), 'Use a fresh cohort instead of replacing an existing manifest'
    APP.mkdir(parents=True)
    old_host = ROOT / 'tests/regression/forza_pgas_feedback_packed/main.cpp'
    assert (SOURCE / 'main.cpp').read_text() == old_host.read_text().replace('work > 64', 'work > 512')
    assert old_host.read_text().count('work > 64') == 1
    assert (SOURCE / 'common.h').read_bytes() == (old_host.parent / 'common.h').read_bytes()
    libraries = json.loads((BUILD / 'library-freeze.json').read_text())
    verify(libraries)
    ordinary = BUILD / 'tests/regression/forza_pgas_feedback_sw_ordered_validated'
    packed = BUILD / 'tests/regression/forza_pgas_feedback_packed_validated'
    manifests = {str(path): json.loads((path / 'feedback-manifest.json').read_text()) for path in [ordinary, packed]}
    sources = []
    for name in ['main.cpp', 'common.h', 'run_matrix.py', 'README.md']:
        src = SOURCE / name
        dest = APP / 'source-freeze' / name
        dest.parent.mkdir(exist_ok=True)
        shutil.copy2(src, dest)
        sources += [fingerprint(src), fingerprint(dest)]
    m = dict(started_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
             build_dir=str(BUILD), cohort='heavy_validated', shared_artifacts=libraries,
             source_artifacts=sources, original_host=fingerprint(old_host),
             host_delta='only allowed work upper bound changes64to512; packed host otherwise identical',
             guest_policy='exact frozen validated binaries; no guest or model changes',
             original_manifests=[fingerprint(path / 'feedback-manifest.json') for path in [ordinary, packed]],
             builds=[], artifacts=[], runs=[])
    env = os.environ.copy()
    env.pop('DEBUG', None)
    env.update(VORTEX_DRIVER='simx', LD_LIBRARY_PATH=str(BUILD / 'sw/runtime'))

    def save():
        text = json.dumps(m, indent=2) + '\n'
        (APP / 'feedback-manifest.json').write_text(text)
        REPORT.with_suffix('.json').write_text(text)
        rows = [dict(cohort=r['cohort'], **r['result'], wall_seconds=r['wall_seconds'], log=r['log'])
                for r in m['runs'] if r['result']]
        if rows:
            with REPORT.with_suffix('.csv').open('w', newline='') as f:
                writer = csv.DictWriter(f, fieldnames=list(rows[0]))
                writer.writeheader()
                writer.writerows(rows)

    for mode in MODES:
        origin = packed if mode == 'native_packed' else ordinary
        origin_manifest = manifests[str(origin)]
        dest = APP / 'artifacts' / mode
        dest.mkdir(parents=True)
        for name in ['kernel.vxbin', 'kernel.elf', 'kernel.dump']:
            src = origin / 'artifacts' / mode / name
            original = next(item for item in origin_manifest['artifacts'] if item['path'] == str(src))
            assert fingerprint(src) == original
            shutil.copy2(src, dest / name)
            m['artifacts'].append({**fingerprint(dest / name), 'original': original})
        log = origin / 'logs' / ('build-' + mode + '.log')
        command = next(shlex.split(line) for line in log.read_text().splitlines()
                       if line.startswith('g++ ') and 'main.cpp' in line and ' -o ' in line)
        command = [str(SOURCE / 'main.cpp') if item.endswith('/main.cpp') else item for item in command]
        command = ['-I' + str(SOURCE) if item.startswith('-I') and 'tests/regression/forza_pgas_feedback' in item else item for item in command]
        command[command.index('-o') + 1] = str(dest / ('forza_feedback_heavy_' + mode))
        build_log = APP / ('build-' + mode + '.log')
        with build_log.open('w') as f:
            subprocess.run(command, cwd=APP, env=env, stdout=f, stderr=subprocess.STDOUT, check=True)
        m['builds'].append(dict(mode=mode, command=command, original_log=fingerprint(log), log=fingerprint(build_log)))
        m['artifacts'].append(fingerprint(dest / ('forza_feedback_heavy_' + mode)))
        verify(libraries)
        save()
        print('BUILT HOST ONLY', mode, flush=True)
    frozen = sources + libraries + m['artifacts'] + [row['log'] for row in m['builds']]
    for work in [128, 512]:
        for pattern in ['hot', 'random']:
            for mode in MODES:
                verify(frozen)
                directory = APP / 'runs' / f'update-{mode}-k64-s128-w{work}-{pattern}-seed17'
                directory.mkdir(parents=True)
                log = directory / 'run.log'
                artifact = APP / 'artifacts' / mode
                cmd = [str(artifact / ('forza_feedback_heavy_' + mode)), '-k', str(artifact / 'kernel.vxbin'),
                       '-n', '64', '-s', '128', '-w', str(work), '-o', 'update', '-p', pattern,
                       '-r', '17', '-t', '900']
                print('START', mode, work, pattern, flush=True)
                start = time.monotonic()
                timed_out = False
                with log.open('w') as f:
                    try:
                        code = subprocess.run(cmd, cwd=directory, env=env, stdout=f, stderr=subprocess.STDOUT, timeout=910).returncode
                    except subprocess.TimeoutExpired:
                        code, timed_out = -1, True
                lines = log.read_text().splitlines()
                fields = lambda line: dict(re.findall(r'(\w+)=([^\s]+)', line))
                results = [fields(line) for line in lines if line.startswith('RESULT ')]
                inputs = [fields(line) for line in lines if line.startswith('INPUT ')]
                row = dict(cohort='heavy', mode=mode, kind='update', keys=64, steps=128, work=work,
                           pattern=pattern, seed=17, command=cmd, cwd=str(directory), log=str(log),
                           returncode=code, timed_out=timed_out, wall_seconds=time.monotonic()-start,
                           passed=code == 0 and bool(results) and any(line.startswith('PASSED:') for line in lines),
                           result=results[-1] if results else None, input=inputs[-1] if inputs else None)
                m['runs'].append(row)
                verify(frozen)
                save()
                print('PASS' if row['passed'] else 'FAIL', mode, work, pattern,
                      (row['result'] or {}).get('device_cycles_post_readback_including_flush'), flush=True)
                if not row['passed']:
                    raise RuntimeError(str(log))
    m['completed_utc'] = datetime.datetime.now(datetime.timezone.utc).isoformat()
    save()
    print('COMPLETE', len(m['runs']), flush=True)


if __name__ == '__main__':
    main()
