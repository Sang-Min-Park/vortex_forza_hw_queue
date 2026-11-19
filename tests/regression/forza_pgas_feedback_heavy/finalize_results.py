#!/usr/bin/env python3
"""Verify the finished heavy cohort and summarize its strongest controls."""
import datetime
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
REPORT = ROOT / 'docs/proposals/forza_pgas_feedback_heavy_validated_results.json'


def fingerprint(path):
    path = Path(path).resolve()
    return dict(path=str(path), bytes=path.stat().st_size,
                sha256=hashlib.sha256(path.read_bytes()).hexdigest())


def verify(item):
    assert fingerprint(item['path']) == {key: item[key] for key in ['path', 'bytes', 'sha256']}, item['path']


def main():
    data = json.loads(REPORT.read_text())
    assert len(data['runs']) == 20 and all(row['passed'] for row in data['runs'])
    assert data.get('completed_utc')
    app = ROOT / 'build64-forza-messaging-isa/tests/regression/forza_pgas_feedback_heavy_parallel'
    verified = 0
    for item in data['source_artifacts'] + data['shared_artifacts'] + data['original_manifests'] + data['artifacts']:
        verify(item)
        verified += 1
        if 'original' in item:
            verify(item['original'])
            assert item['sha256'] == item['original']['sha256']
    for row in data['builds']:
        verify(row['log'])
        verify(row['original_log'])
    guest_sources = []
    for item in data['original_manifests']:
        original = json.loads(Path(item['path']).read_text())
        for source in original['source_artifacts']:
            snapshot = fingerprint(source['snapshot'])
            assert snapshot['sha256'] == source['sha256']
            guest_sources.append(snapshot)
    verify(data['original_host'])
    source = Path(__file__).resolve().parent
    old_host = Path(data['original_host']['path']).read_text()
    assert (source / 'main.cpp').read_text() == old_host.replace('work > 64', 'work > 512')
    logs = [fingerprint(row['log']) for row in data['runs']]
    for row in data['runs']:
        lines = Path(row['log']).read_text().splitlines()
        assert row['returncode'] == 0 and not row['timed_out']
        assert any(line.startswith('PASSED: all client response chains, causal state histories and final records') for line in lines)
        result = row['result']
        assert int(result['clients']) == 8 and int(result['logical_queries']) == 1024
        assert int(result['causal_nodes_replayed']) == 1024
        assert int(result['logical_handler_iterations']) == 1024 * row['work']
    grouped_report = ROOT / 'docs/proposals/forza_pgas_feedback_grouped_validated_results.json'
    grouped = json.loads(grouped_report.read_text())
    assert len(grouped['runs']) == 13 and all(row['passed'] for row in grouped['runs'])
    summaries = []
    for work in [128, 512]:
        for pattern in ['hot', 'random']:
            rows = [row for row in data['runs'] if row['work'] == work and row['pattern'] == pattern]
            native = next(row for row in grouped['runs'] if row['work'] == work and row['pattern'] == pattern
                          and row['steps'] == 128)
            assert native['passed']
            assert len({row['result']['input_hash'] for row in rows} | {native['result']['input_hash']}) == 1
            cycles = {row['mode']: int(row['result']['device_cycles_post_readback_including_flush']) for row in rows}
            cycles['native_grouped'] = int(native['result']['device_cycles_post_readback_including_flush'])
            best = min(value for mode, value in cycles.items() if mode.startswith('native'))
            message = min(cycles['isa_wait_global'], cycles['isa_wait_local'])
            summaries.append(dict(work=work, pattern=pattern, cycles=cycles, best_native_cycles=best,
                                  best_hardware_message_cycles=message,
                                  hardware_message_over_best_native_cycles=message/best,
                                  hardware_local_over_software_local_cycles=cycles['isa_wait_local']/cycles['gpu_queue_local']))
    freeze = dict(verified_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                  passed=20, run_count=20, verified_artifacts=verified,
                  host_delta_verified='only work upper bound64to512, ABI and validator unchanged',
                  exact_original_guest_artifacts=15, all_4_frozen_libraries_unchanged=True,
                  report=fingerprint(REPORT), grouped_comparison_report=fingerprint(grouped_report),
                  app_manifest=fingerprint(app / 'feedback-manifest.json'),
                  continuation_previous_manifest=data['continuation']['previous_manifest'],
                  original_guest_source_snapshots=guest_sources,
                  run_logs=logs, build_logs=[row['log'] for row in data['builds']],
                  analysis_source=fingerprint(__file__), summaries=summaries,
                  limits='Adaptive update schedules may differ; equal initial input hash and each exact causal history are verified. Simulation cycles are not measured physical GPU speed or PPA.')
    dest = REPORT.with_name(REPORT.stem.replace('_results', '_freeze') + '.json')
    dest.write_text(json.dumps(freeze, indent=2) + '\n')
    print(json.dumps(summaries, indent=2))


if __name__ == '__main__':
    main()
