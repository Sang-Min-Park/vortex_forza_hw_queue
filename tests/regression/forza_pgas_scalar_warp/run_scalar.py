#!/usr/bin/env python3
"""Frozen scalar/SIMT feedback comparison; this runner never builds a driver."""
import argparse
import concurrent.futures
import csv
import datetime
import fcntl
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
MACHINES = ['simt2w4t', 'scalar8w1t', 'simt2w4t_port1', 'scalar2w1t']
MODES = ['native', 'native_grouped', 'isa_try_global', 'isa_wait_global',
         'isa_wait_local', 'gpu_queue_local']
SHAPES = dict(simt2w4t=(2, 4), scalar8w1t=(8, 1), simt2w4t_port1=(2, 4), scalar2w1t=(2, 1))
RESOURCE_KEYS = ['XLEN', 'NUM_CORES', 'NUM_WARPS', 'NUM_THREADS', 'SIMD_WIDTH', 'ISSUE_WIDTH',
                 'NUM_OPCS', 'NUM_ALU_LANES', 'NUM_SFU_LANES', 'NUM_LSU_LANES',
                 'LSU_LINE_SIZE', 'LSU_PENDING_SIZE', 'LSU_QUEUE_IN_SIZE',
                 'LMEM_LOG_SIZE', 'LMEM_NUM_BANKS', 'L2_SIZE', 'L2_NUM_BANKS',
                 'L2_LATENCY', 'DCACHE_ENABLED', 'AMO_RS_SIZE']
MODEL_SOURCES = ['VX_config.toml', 'ci/gen_config.py', 'sim/simx/types.h',
                 'sim/simx/decode.cpp', 'sim/simx/sfu_unit.cpp', 'sim/simx/core.cpp',
                 'sim/simx/forza/forza_zone.cpp', 'sim/simx/forza/forza_zone.h',
                 'sim/simx/forza/forza_fabric.cpp', 'sim/simx/forza/forza_fabric.h',
                 'sim/simx/forza/forza_types.h', 'sim/simx/mem/local_mem.cpp',
                 'sw/common/forza_abi.h', 'sw/common/forza_mmio_abi.h', 'sw/common/forza_isa_abi.h']


def utc():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def fingerprint(path):
    path = Path(path).resolve()
    return dict(path=str(path), bytes=path.stat().st_size,
                sha256=hashlib.sha256(path.read_bytes()).hexdigest())


def verify_item(item):
    assert fingerprint(item['path']) == {key: item[key] for key in ['path', 'bytes', 'sha256']}, item['path']
    if item.get('snapshot'):
        assert fingerprint(item['snapshot'])['sha256'] == item['sha256'], item['snapshot']


def verify(data):
    for item in data['source_artifacts'] + data['shared_artifacts'] + data['artifacts'] + data['model_metadata']:
        verify_item(item)
    for row in data['builds']:
        verify_item(row['log'])
    for row in data['runs']:
        verify_item(row['log_artifact'])


def resolved(configs):
    output = subprocess.check_output(['python3', str(ROOT / 'ci/gen_config.py'), '--config',
                                      str(ROOT / 'VX_config.toml'), '--cflags', configs + ' -DVX_CFG_XLEN=64'], text=True)
    values = {}
    for token in shlex.split(output):
        if token.startswith('-D'):
            pair = token[2:].split('=', 1)
            values[pair[0]] = pair[1] if len(pair) == 2 else '1'
    return {key: int(values['VX_CFG_' + key], 0) for key in RESOURCE_KEYS}


def shape_audit(name, configs):
    values = resolved(configs)
    warps, threads = SHAPES[name]
    expected = dict(XLEN=64, NUM_CORES=2, NUM_WARPS=warps, NUM_THREADS=threads, SIMD_WIDTH=threads,
                    ISSUE_WIDTH=1, NUM_ALU_LANES=threads, NUM_SFU_LANES=threads,
                    NUM_LSU_LANES=threads, LSU_PENDING_SIZE=8, LMEM_LOG_SIZE=14,
                    LMEM_NUM_BANKS=4, L2_SIZE=1048576, L2_NUM_BANKS=1, L2_LATENCY=4,
                    DCACHE_ENABLED=0)
    for key, value in expected.items():
        assert values[key] == value, (name, key, values[key], value)
    flags = {item.split('=', 1)[0] for item in shlex.split(configs)}
    for flag in ['-DVX_CFG_EXT_FORZA_ENABLE', '-DVX_CFG_EXT_A_ENABLE',
                 '-DVX_CFG_FORZA_MMIO_ENABLE', '-DVX_CFG_FORZA_ISA_ENABLE']:
        assert flag in flags, (name, flag)
    assert not flags.intersection({'-DVX_CFG_FORZA_UPDATE_ENABLE', '-DVX_CFG_FORZA_RECORD_ENABLE',
                                   '-DVX_CFG_FORZA_VM_ENABLE', '-DFORZA_POOL128_ENABLE'})
    vector = '-DVX_CFG_FORZA_MMIO_VECTOR_ENABLE' in flags
    assert vector == (name != 'simt2w4t_port1')
    values.update(physical_actors_per_core=warps * threads,
                  mailbox_data_bytes_per_core=warps * threads * 1024,
                  retained_message_instructions_per_core=2 * warps,
                  message_command_budget_per_core=threads if vector else 1,
                  logical_clients_total=8, logical_clients_per_core=4,
                  physical_workers_per_role_per_core=warps * threads // 2,
                  chains_per_physical_client=8 // (warps * threads))
    return values


def case_key(row):
    return tuple(row[key] for key in ['cohort', 'mode', 'kind', 'steps', 'work', 'pattern', 'seed'])


def cases(stage, modes, seed):
    conditions = [('update', 0, 1, 'random'), ('update', 1, 1, 'hot'), ('update', 7, 1, 'random')]
    if stage == 'primary':
        conditions = [('walk', 128, 1, 'random'), ('update', 128, 1, 'random'),
                      ('update', 128, 1, 'hot'), ('update', 128, 32, 'hot')]
    return [dict(cohort=stage, mode=mode, kind=kind, steps=steps, work=work, pattern=pattern, seed=seed)
            for kind, steps, work, pattern in conditions for mode in modes]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stage', choices=['build', 'smoke', 'primary', 'freeze', 'all'], required=True)
    parser.add_argument('--cohort', default='validated')
    parser.add_argument('--machines', default=','.join(MACHINES))
    parser.add_argument('--modes', default=','.join(MODES))
    parser.add_argument('--seed', type=int, default=17)
    parser.add_argument('--jobs', type=int, default=2)
    parser.add_argument('--timeout', type=int, default=600)
    args = parser.parse_args()
    selected = args.machines.split(',')
    modes = args.modes.split(',')
    assert re.fullmatch(r'[a-z0-9_]+', args.cohort)
    assert set(selected).issubset(MACHINES) and len(set(selected)) == len(selected)
    assert set(modes).issubset(MODES) and len(set(modes)) == len(modes)
    assert 1 <= args.jobs <= 2 and 1 <= args.timeout <= 3600
    prefix = ROOT / 'docs/proposals' / ('forza_pgas_scalar_warp_' + args.cohort)
    lock = prefix.with_suffix('.lock').open('w')
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    data = {}
    building = args.stage in ['build', 'all']

    def app(name):
        return ROOT / ('build64-forza-warp-' + name) / 'tests/regression' / ('forza_pgas_scalar_warp_' + args.cohort)

    def save_machine(name):
        (app(name) / 'scalar-manifest.json').write_text(json.dumps(data[name], indent=2) + '\n')

    def load(name):
        path = app(name) / 'scalar-manifest.json'
        result = json.loads(path.read_text())
        assert result['machine'] == name and result['modes'] == modes and result['seed'] == args.seed
        verify(result)
        return result

    def save_report():
        for name in MACHINES:
            path = app(name) / 'scalar-manifest.json'
            if not building and name not in data and path.exists():
                data[name] = load(name)
        ordered = [data[name] for name in MACHINES if name in data]
        report = dict(updated_utc=utc(), cohort=args.cohort, modes=modes, seed=args.seed,
                      machine_manifests=[fingerprint(app(row['machine']) / 'scalar-manifest.json') for row in ordered],
                      resource_audits={row['machine']: row['resources'] for row in ordered},
                      runs=[dict(machine=row['machine'], **run) for row in ordered for run in row['runs']])
        prefix.with_name(prefix.name + '_results.json').write_text(json.dumps(report, indent=2) + '\n')
        rows = [dict(machine=run['machine'], cohort=run['cohort'], **run['result'], log=run['log'],
                     wall_seconds=run['wall_seconds']) for run in report['runs'] if run['result']]
        if rows:
            keys = list(dict.fromkeys(key for row in rows for key in row))
            with prefix.with_name(prefix.name + '_results.csv').open('w', newline='') as f:
                writer = csv.DictWriter(f, fieldnames=keys)
                writer.writeheader()
                writer.writerows(rows)
        return report

    def build_machine(name):
        build = ROOT / ('build64-forza-warp-' + name)
        metadata = build / 'scalar-machine.json'
        freeze = build / 'library-freeze.json'
        meta = json.loads(metadata.read_text())
        configs = meta['configs']
        assert isinstance(configs, str)
        resources = shape_audit(name, configs)
        libraries = json.loads(freeze.read_text())
        assert isinstance(libraries, list) and len(libraries) == 4
        for item in libraries:
            verify_item(item)
        directory = app(name)
        manifest = directory / 'scalar-manifest.json'
        if manifest.exists():
            result = load(name)
        else:
            directory.mkdir(parents=True)
            (directory / 'logs').mkdir()
            shutil.copy2(SOURCE / 'Makefile', directory / 'Makefile')
            sources = sorted(SOURCE.glob('*.cpp')) + sorted(SOURCE.glob('*.h'))
            sources += [SOURCE / 'Makefile', Path(__file__).resolve()]
            if (SOURCE / 'README.md').exists():
                sources.append(SOURCE / 'README.md')
            if (SOURCE / 'build_models.py').exists():
                sources.append(SOURCE / 'build_models.py')
            sources += [ROOT / name for name in MODEL_SOURCES]
            captured = []
            for src in sources:
                dest = directory / 'source-freeze' / src.relative_to(ROOT)
                dest.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(src, dest)
                captured.append(dict(**fingerprint(src), snapshot=str(dest)))
            result = dict(started_utc=utc(), machine=name, cohort=args.cohort, modes=modes, seed=args.seed,
                          build_dir=str(build), configs=configs, resources=resources,
                          environment=meta.get('environment', {}),
                          source_artifacts=captured, shared_artifacts=libraries,
                          model_metadata=[fingerprint(metadata), fingerprint(freeze)],
                          builds=[], built_modes=[], artifacts=[], runs=[])
        env = os.environ.copy()
        env.update(result['environment'])
        env.pop('DEBUG', None)
        env.update(CONFIGS=configs)
        for mode in modes:
            if mode in result['built_modes']:
                continue
            verify(result)
            command = ['make', '-C', str(directory), 'MODE=' + mode]
            for item in libraries:
                command += ['-o', item['path']]
            attempt = sum(row['mode'] == mode for row in result['builds'])
            log = directory / 'logs' / f'build-{mode}-{attempt}.log'
            with log.open('w') as output:
                code = subprocess.run(command + ['clean'], env=env, stdout=output, stderr=subprocess.STDOUT).returncode
                if code == 0:
                    code = subprocess.run(command + ['-j2'], env=env, stdout=output, stderr=subprocess.STDOUT).returncode
            result['builds'].append(dict(mode=mode, commands=[command + ['clean'], command + ['-j2']],
                                         returncode=code, log=fingerprint(log)))
            if code == 0:
                dest = directory / 'artifacts' / mode
                dest.mkdir(parents=True)
                for filename in ['forza_scalar_' + mode, 'kernel.vxbin', 'kernel.elf', 'kernel.dump']:
                    shutil.copy2(directory / filename, dest / filename)
                    result['artifacts'].append(fingerprint(dest / filename))
                result['built_modes'].append(mode)
            (directory / 'scalar-manifest.json').write_text(json.dumps(result, indent=2) + '\n')
            verify(result)
            assert code == 0, str(log)
            print('BUILT', name, mode, flush=True)
        result['build_completed_utc'] = utc()
        (directory / 'scalar-manifest.json').write_text(json.dumps(result, indent=2) + '\n')
        return name, result

    if args.stage in ['build', 'all']:
        with concurrent.futures.ThreadPoolExecutor(max_workers=min(args.jobs, len(selected))) as pool:
            futures = [pool.submit(build_machine, name) for name in selected]
            for future in concurrent.futures.as_completed(futures):
                name, result = future.result()
                data[name] = result
                save_report()
        building = False
    else:
        data = {name: load(name) for name in selected}

    def smoke_gate():
        for name in MACHINES:
            if name not in data:
                data[name] = load(name)
            required = {case_key(row) for row in cases('smoke', modes, args.seed)}
            passed = {case_key(row) for row in data[name]['runs'] if row['passed']}
            assert required.issubset(passed), 'All four machines must pass every smoke before primary: ' + name

    def run_case(item):
        name, case = item
        result = data[name]
        verify(result)
        base = app(name) / 'runs' / ('-'.join(str(case[key]) for key in
                ['cohort', 'mode', 'kind', 'steps', 'work', 'pattern', 'seed']))
        attempt = 0
        while (base / ('attempt' + str(attempt))).exists():
            attempt += 1
        directory = base / ('attempt' + str(attempt))
        directory.mkdir(parents=True)
        artifact = app(name) / 'artifacts' / case['mode']
        command = [str(artifact / ('forza_scalar_' + case['mode'])), '-k', str(artifact / 'kernel.vxbin'),
                   '-n', '64', '-s', str(case['steps']), '-w', str(case['work']), '-o', case['kind'],
                   '-p', case['pattern'], '-r', str(args.seed), '-t', str(args.timeout)]
        env = os.environ.copy()
        env.update(result['environment'])
        env.pop('DEBUG', None)
        env.update(CONFIGS=result['configs'], VORTEX_DRIVER='simx',
                   LD_LIBRARY_PATH=str(Path(result['build_dir']) / 'sw/runtime'))
        log = directory / 'run.log'
        print('START', name, case, flush=True)
        started_utc = utc()
        start = time.monotonic()
        timeout = False
        with log.open('w') as output:
            try:
                code = subprocess.run(command, cwd=directory, env=env, stdout=output,
                                      stderr=subprocess.STDOUT, timeout=args.timeout + 10).returncode
            except subprocess.TimeoutExpired:
                code, timeout = -1, True
        lines = log.read_text().splitlines()
        fields = lambda line: dict(re.findall(r'(\w+)=([^\s]+)', line))
        outputs = [fields(line) for line in lines if line.startswith('RESULT ')]
        inputs = [fields(line) for line in lines if line.startswith('INPUT ')]
        row = dict(**case, keys=64, command=command, cwd=str(directory), log=str(log),
                   started_utc=started_utc, completed_utc=utc(),
                   log_artifact=fingerprint(log), returncode=code, timed_out=timeout,
                   wall_seconds=time.monotonic()-start, result=outputs[-1] if outputs else None,
                   input=inputs[-1] if inputs else None,
                   passed=code == 0 and bool(outputs) and bool(inputs)
                          and any(line.startswith('PASSED:') for line in lines))
        if row['passed']:
            for where in [row['input'], row['result']]:
                for field in ['warps', 'threads', 'physical_workers_per_role_per_core', 'chains_per_physical_client']:
                    expected = result['resources'].get(field.upper(), result['resources'].get(field))
                    if field == 'warps': expected = result['resources']['NUM_WARPS']
                    if field == 'threads': expected = result['resources']['NUM_THREADS']
                    assert int(where[field]) == expected, (name, field, where)
            assert int(row['result']['clients']) == 8
            assert int(row['result']['logical_queries']) == 8 * case['steps']
            assert int(row['result']['causal_nodes_replayed']) == 8 * case['steps']
        return name, row

    def run_stage(stage):
        pending = []
        for name in selected:
            assert data[name]['built_modes'] == modes
            assert all(row['passed'] for row in data[name]['runs']), 'Failed rows require a fresh cohort'
        for case in cases(stage, modes, args.seed):
            for name in selected:
                done = {case_key(row) for row in data[name]['runs']}
                if case_key(case) not in done:
                    pending.append((name, case))
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
            futures = [pool.submit(run_case, item) for item in pending]
            for future in concurrent.futures.as_completed(futures):
                name, row = future.result()
                data[name]['runs'].append(row)
                save_machine(name)
                save_report()
                print('PASS' if row['passed'] else 'FAIL', name, row['mode'], row['kind'], row['steps'], flush=True)
                assert row['passed'], row['log']

    if args.stage in ['smoke', 'all']:
        run_stage('smoke')
    if args.stage in ['primary', 'all']:
        smoke_gate()
        run_stage('primary')
    report = save_report()
    if args.stage in ['freeze', 'all']:
        smoke_gate()
        expected = {case_key(row) for stage in ['smoke', 'primary'] for row in cases(stage, modes, args.seed)}
        for name in MACHINES:
            verify(data[name])
            assert len(data[name]['runs']) == len(expected)
            assert {case_key(row) for row in data[name]['runs']} == expected
            assert all(row['passed'] for row in data[name]['runs'])
        groups = {}
        for row in report['runs']:
            key = tuple(row[field] for field in ['kind', 'steps', 'work', 'pattern', 'seed'])
            groups.setdefault(key, set()).add(row['result']['input_hash'])
        assert all(len(hashes) == 1 for hashes in groups.values()), 'Initial input differs across machines or modes'
        last_smoke = max(row['completed_utc'] for row in report['runs'] if row['cohort'] == 'smoke')
        first_primary = min(row['started_utc'] for row in report['runs'] if row['cohort'] == 'primary')
        assert last_smoke <= first_primary, 'Primary ran before the global smoke gate'
        artifacts = []
        for result in data.values():
            artifacts += result['source_artifacts'] + result['shared_artifacts'] + result['artifacts'] + result['model_metadata']
            artifacts += [row['log'] for row in result['builds']] + [row['log_artifact'] for row in result['runs']]
        freeze = dict(verified_utc=utc(), run_count=len(report['runs']), passed=len(report['runs']),
                      report=fingerprint(prefix.with_name(prefix.name + '_results.json')),
                      machine_manifests=report['machine_manifests'], artifacts=artifacts,
                      resource_audits=report['resource_audits'], all_smokes_before_primary=True,
                      limitations='Different ALU/LSU widths, operand collectors, warp contexts and mailbox allocation are disclosed; not equal PPA. Eight logical client chains stay fixed. No physical GPU claim.')
        prefix.with_name(prefix.name + '_freeze.json').write_text(json.dumps(freeze, indent=2) + '\n')
        print('FROZEN', len(report['runs']), flush=True)


if __name__ == '__main__':
    main()
