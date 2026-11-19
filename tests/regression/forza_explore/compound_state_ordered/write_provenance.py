#!/usr/bin/env python3
"""Provenance for the ordered-release transaction rerun and frozen GPU controls."""
import argparse
import hashlib
import json
from pathlib import Path


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--root', type=Path, required=True)
    root = parser.parse_args().root.resolve()
    old_path = root / 'docs/proposals/forza_explore_compound_manifest.json'
    old = json.loads(old_path.read_text())
    source = root / 'tests/regression/forza_explore/compound_state_ordered'
    freeze = json.loads((source / 'source-freeze.json').read_text())
    if not all(sha(root / path) == digest for path, digest in freeze.items()):
        raise RuntimeError('compiled source changed during corrected measurements')
    copied = {name: sha(source / name) == sha(source.parent / 'compound_state' / name)
              for name in ('main.cpp', 'common.h', 'state.h', 'kernel_mp.cpp')}
    assert all(copied.values())
    paths = [p for p in source.iterdir() if p.is_file()]
    paths += list((source.parent / 'runtime_ordered/inc').glob('*.h'))
    paths += [source.parent / 'runtime_ordered/vortex.mk']
    data = {'schema': 1, 'cohort': 'data_dependent_release',
            'source_sha256': {str(p.relative_to(root)): sha(p) for p in sorted(set(paths))},
            'copied_app_sources_identical_to_frozen': copied,
            'frozen_reference_manifest': str(old_path), 'frozen_reference_manifest_sha256': sha(old_path),
            'driver_configuration': old['driver_configuration'], 'builds': {}}
    for cache, build in [('l1_off', 'build64-forza-explore'), ('l1_on', 'build64-forza-explore-l1')]:
        app = root / build / 'tests/regression/forza_explore/compound_state_ordered'
        measured = json.loads((app / 'validation-run-manifest.json').read_text())
        assert measured['artifacts_unchanged']
        assert all(sha(Path(path)) == digest for path, digest in measured['artifacts'].items())
        # The old driver and both native controls must be exactly the measured originals.
        for path, digest in measured['artifacts'].items():
            relative = str(Path(path).relative_to(root))
            if '/compound_state_ordered/' not in relative:
                assert old['builds'][cache]['artifact_sha256'][relative] == digest
        artifacts = [p for p in (app / 'artifacts').rglob('*') if p.is_file()]
        data['builds'][cache] = {'run_manifest': str(app / 'validation-run-manifest.json'),
                                'run_manifest_sha256': sha(app / 'validation-run-manifest.json'),
                                'artifacts': {str(p.relative_to(root)): sha(p) for p in artifacts},
                                'runs': measured['runs'], 'measured_artifacts': measured['artifacts'],
                                'frozen_native_drivers_verified': True}
    target = root / 'docs/proposals/forza_explore_compound_ordered_manifest.json'
    target.write_text(json.dumps(data, indent=2) + '\n')
    print(target)


if __name__ == '__main__':
    main()
