from pathlib import Path
import hashlib
import json
import re

root = Path(__file__).resolve().parents[2]
experiments = []
artifacts = {}
for name, cores, warps, threads in [('2c2w4t', 2, 2, 4), ('2c2w8t', 2, 2, 8), ('4c2w4t', 4, 2, 4), ('4c2w8t', 4, 2, 8), ('2c4w8t', 2, 4, 8)]:
    build = root / ('build64-forza-csr-generalize-' + name)
    cases = [('forza_queue', 'run-queue' if name == '4c2w8t' else 'forza_queue'),
             ('forza_runtime', 'run-selector' if name == '4c2w8t' else 'forza_runtime')]
    for test, basename in cases:
        log = build / 'diagnostics' / (basename + '.log')
        status = int(log.with_suffix('.exit').read_text().strip())
        text = log.read_text(errors='replace')
        summaries = [s for s in text.splitlines() if s.startswith(('FORZA queues:', 'FORZA runtime:', 'PASSED:', 'FAILED:'))]
        experiments.append(dict(test=test, cores=cores, warps_per_core=warps, lanes_per_warp=threads,
                                physical_actors=cores*warps*threads, exit_code=status, timed_out=('exceeded' in text and 'deadline' in text),
                                passed=status == 0 and any(s.startswith('PASSED:') for s in summaries),
                                summaries=summaries, log=str(log.relative_to(root))))
        for filename in (test, 'kernel.vxbin'):
            p = build / 'tests/regression' / test / filename
            artifacts[str(p.relative_to(root))] = hashlib.sha256(p.read_bytes()).hexdigest()
    p = build / 'sw/runtime/libsimx.so'
    artifacts[str(p.relative_to(root))] = hashlib.sha256(p.read_bytes()).hexdigest()

build = root / 'build64-forza-csr-generalize-2c2w4t'
for waves in (1, 2, 4):
    for mode in ('physical', 'logical'):
        log = build / 'diagnostics' / f'probe-w{waves}-{mode}.log'
        status = int(log.with_suffix('.exit').read_text().strip())
        lines = log.read_text(errors='replace').splitlines()
        summary = next(s for s in lines if s.startswith(('PASSED:', 'FAILED:')))
        counts = {k:int(v) for k,v in re.findall(r'(errors|registration_errors|sent|received|retries)=(\d+)',summary)}
        experiments.append(dict(test='forza_csr_generalize', cores=2, warps_per_core=2, lanes_per_warp=4,
                                physical_actors=16, grid_blocks=2*waves, launched_logical_threads=16*waves,
                                pe_mode=mode, exit_code=status, passed=status==0, **counts,
                                summary=summary, log=str(log.relative_to(root))))
for filename in ('forza_csr_generalize', 'kernel.vxbin'):
    p = build / 'tests/regression/forza_csr_generalize' / filename
    artifacts[str(p.relative_to(root))] = hashlib.sha256(p.read_bytes()).hexdigest()

report = dict(
    scope='RV64 SimX CSR-only correctness experiments; production queue and runtime unchanged',
    feature='VX_CFG_EXT_FORZA_ENABLE; optional FORZA ISA/MMIO/UPDATE/RECORD/VM disabled',
    experiments=experiments,
    interpretation=[
        'Topology results establish correctness only for the observed configurations and bounded patterns.',
        'Runtime tests have all actors sending/receiving; handlers create replies; no dedicated service warp.',
        'Physical PE reuse probes send only to self, drain all messages and ACKs, and retain physical addresses.',
        'Logical PE probes change address identity on a reused endpoint; registration rejection is an actual failed probe, not hidden as success.',
        'Delivery to nonresident blocks, arbitrary handler cycles, general logical-actor virtualization and HW-vs-SW performance were not tested.'
    ],
    source_sha256=json.loads((build/'diagnostics/source-sha256.json').read_text()),
    artifact_sha256=artifacts,
    replay_scripts=[
        'build64-forza-csr-generalize-2c2w4t/diagnostics/run.sh',
        'build64-forza-csr-generalize-2c2w8t/diagnostics/run.sh',
        'build64-forza-csr-generalize-4c2w4t/diagnostics/run.sh',
        'build64-forza-csr-generalize-4c2w8t/diagnostics/run-experiment.sh',
        'build64-forza-csr-generalize-2c2w4t/diagnostics/run-probes.sh'
    ])
diagnostics = []
d = root/'build64-forza-csr-generalize-4c2w8t/diagnostics'
for name, description in [('run-selector-n1', 'Same device kernel, fewer application rounds; original host timeout'),
                          ('run-selector-n1-long', 'Same device kernel, host event wait extended to 300 seconds'),
                          ('run-phase', 'Separate instrumented device diagnostic; no conclusions from this artifact')]:
    p = d/(name+'.log')
    status = int(p.with_suffix('.exit').read_text().strip())
    text = p.read_text(errors='replace')
    diagnostics.append(dict(description=description, exit_code=status,
                            timed_out=('exceeded' in text and 'deadline' in text),
                            summaries=[s for s in text.splitlines() if s.startswith(('FORZA runtime:', 'PASSED:', 'FAILED:'))],
                            log=str(p.relative_to(root))))
report['additional_diagnostics'] = diagnostics
report['long_host_summary'] = json.loads((d/'summary.json').read_text())
report['optional_phase_diagnostic'] = 'Instrumented kernel aborted with exit134 before any device markers; no localization claim is made from it.'
report['replay_scripts'] += ['build64-forza-csr-generalize-2c4w8t/diagnostics/run-experiment.sh',
                             'build64-forza-csr-generalize-4c2w8t/diagnostics/run-long-host.sh',
                             'build64-forza-csr-generalize-4c2w8t/diagnostics/run-phase-probe.sh']
path = root/'docs/proposals/forza_csr_generalization_results.json' 
if path.exists():
    raise RuntimeError(f'Report already exists: {path}')
path.write_text(json.dumps(report,indent=2,ensure_ascii=False)+'\n')
print(path)
for item in experiments:
    print(item['test'], item.get('pe_mode',''), item.get('grid_blocks',''), item['physical_actors'], item['exit_code'])
