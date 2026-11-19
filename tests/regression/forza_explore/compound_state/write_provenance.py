#!/usr/bin/env python3
"""Capture source, model, toolchain, driver, binary, and measurement provenance."""
import argparse
import hashlib
import json
import re
from pathlib import Path
import subprocess


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--root', required=True, type=Path)
    args = parser.parse_args()
    root = args.root.resolve()
    app_source = root / 'tests/regression/forza_explore/compound_state'
    runtime_source = root / 'tests/regression/forza_explore/runtime'
    sources = [path for path in app_source.iterdir() if path.suffix in ('.cpp', '.h', '.py', '.md') or path.name == 'Makefile']
    sources += list((runtime_source / 'inc').glob('*.h')) + [runtime_source / 'vortex.mk']
    sources += [root / name for name in (
        'VX_config.toml', 'sim/simx/constants.h', 'sim/simx/cluster.cpp',
        'sim/simx/core.cpp', 'sim/simx/processor.cpp', 'sim/simx/lsu_unit.cpp',
        'sim/simx/csr_unit.cpp', 'sim/simx/mem/cache.cpp',
        'sim/simx/mem/mem_coalescer.cpp', 'sim/simx/mem/local_mem.cpp',
        'sim/simx/mem/local_mem_switch.cpp', 'sim/simx/forza/forza_zone.cpp',
        'sim/simx/forza/forza_fabric.cpp', 'sim/simx/forza/forza_types.h',
        'sw/runtime/common/device.cpp')]
    missing = [str(path) for path in sources if not path.exists()]
    if missing:
        raise RuntimeError(f'missing provenance dependencies: {missing}')
    data = {'schema': 1, 'repo': str(root),
            'git_head': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip(),
            'git_dirty': bool(subprocess.check_output(['git', 'status', '--porcelain'], cwd=root, text=True)),
            'source_sha256': {str(path.relative_to(root)): digest(path) for path in sorted(set(sources))},
            'driver_configuration': {
                'common': ['-DVX_CFG_EXT_FORZA_ENABLE', '-DVX_CFG_EXT_A_ENABLE',
                           '-DVX_CFG_L2_ENABLE', '-DVX_CFG_NUM_CORES=2',
                           '-DVX_CFG_NUM_WARPS=2', '-DVX_CFG_NUM_THREADS=4'],
                'l1_off_extra': '-DVX_CFG_DCACHE_DISABLE', 'xlen': 64,
                'actual_cpp_constants': {'L2_SIZE': 1048576, 'L2_BANKS': 4,
                                         'LMEM_BANKS': 4, 'L2_REQS': 4, 'L1_PORTS': 2},
                'constants_scope': 'Generated sw/VX_config.h plus sim/simx/constants.h; not standalone host gen_config expansion.'},
            'builds': {}}
    compiler = Path('/export/nethomes/spark3100/tools/llvm-vortex/bin/clang++')
    if compiler.exists():
        data['compiler'] = {'path': str(compiler),
                            'version': subprocess.check_output([str(compiler), '--version'], text=True).strip()}
    for cache, name in [('l1_off', 'build64-forza-explore'), ('l1_on', 'build64-forza-explore-l1')]:
        build = root / name
        app = build / 'tests/regression/forza_explore/compound_state'
        stamp = build / 'sw/runtime/simx_config.stamp'
        paths = [build / 'sw/VX_config.h', stamp]
        paths += [build / 'sw/runtime' / filename for filename in
                  ('libsimx.so', 'libvortex-simx.so', 'libvortex.so')]
        paths += sorted(path for path in (app / 'snapshots').rglob('*') if path.is_file())
        logs = sorted((app / 'logs').glob('*.log'))
        manifests = sorted(app.glob('*run-manifest.json'))
        item = {'build': str(build), 'driver_stamp': stamp.read_text().strip(),
                'artifact_sha256': {str(path.relative_to(root)): digest(path) for path in paths},
                'run_manifests': {str(path.relative_to(root)): digest(path) for path in manifests},
                'log_sha256': {str(path.relative_to(root)): digest(path) for path in logs}}
        data['builds'][cache] = item
    off_app = root / 'build64-forza-explore/tests/regression/forza_explore/compound_state'
    checks = []
    for mode, filename in [('mp', 'probe-hot4096-mp-before-active.log'),
                           ('gpu_lock', 'probe-hot4096-gpu_lock.log'),
                           ('gpu_owner', 'probe-hot4096-gpu_owner.log')]:
        probe = off_app / 'logs' / filename
        official = off_app / 'logs' / ('validation-l1_off-' + mode + '-hot.log')
        def result(path):
            lines = path.read_text().splitlines()
            line = next(line for line in lines if line.startswith('RESULT '))
            return dict(re.findall(r'(\w+)=([^\s]+)', line))
        checks.append({'mode': mode, 'cache': 'l1_off',
                       'probe_log': str(probe.relative_to(root)),
                       'official_log': str(official.relative_to(root)),
                       'all_result_fields_exact': result(probe) == result(official)})
    data['independent_hot_probe_repeat_checks'] = checks
    data['original_mp_device_image_matches_official'] = (
        digest(off_app / 'snapshots/mp/kernel-mp.vxbin') ==
        digest(off_app / 'snapshots/mp_before_active/kernel-mp.vxbin'))
    output = root / 'docs/proposals/forza_explore_compound_manifest.json'
    output.write_text(json.dumps(data, indent=2) + '\n')
    print(output)
    print(f'sources={len(data["source_sha256"])} builds={len(data["builds"])}')


if __name__ == '__main__':
    main()
