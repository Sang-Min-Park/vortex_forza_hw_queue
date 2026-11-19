#!/usr/bin/env python3
"""Independently verify frozen warp-width runs and generate paired comparisons."""

import hashlib
import json
from collections import defaultdict
from datetime import datetime, timezone
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
DOCS = ROOT / "docs/proposals"
PREFIX = DOCS / "forza_pgas_scalar_warp_validated"
MACHINES = ["simt2w4t", "scalar8w1t", "simt2w4t_port1", "scalar2w1t"]


def fingerprint(path):
    path = Path(path)
    return {"path": str(path), "bytes": path.stat().st_size,
            "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}


def main():
    verified = {}

    def visit(value):
        if isinstance(value, dict):
            if {"path", "bytes", "sha256"} <= value.keys():
                key = (value["path"], value["sha256"])
                if key not in verified:
                    actual = fingerprint(value["path"])
                    assert actual == {k: value[k] for k in actual}, actual
                    verified[key] = actual
                if value.get("snapshot"):
                    actual = fingerprint(value["snapshot"])
                    assert actual["sha256"] == value["sha256"]
                    verified[(actual["path"], actual["sha256"])] = actual
            for child in value.values():
                visit(child)
        elif isinstance(value, list):
            for child in value:
                visit(child)

    raw_path = PREFIX.with_name(PREFIX.name + "_results.json")
    freeze_path = PREFIX.with_name(PREFIX.name + "_freeze.json")
    raw, freeze = (json.loads(p.read_text()) for p in [raw_path, freeze_path])
    visit(freeze)
    visit(raw)
    for item in raw["machine_manifests"]:
        visit(json.loads(Path(item["path"]).read_text()))
    for machine in MACHINES:
        build = ROOT / ("build64-forza-warp-" + machine)
        visit(json.loads((build / "scalar-machine.json").read_text()))
        visit(json.loads((build / "library-freeze.json").read_text()))
    runs = raw["runs"]
    assert len(runs) == freeze["run_count"] == freeze["passed"] == 168
    assert sum(r["cohort"] == "smoke" for r in runs) == 72
    assert sum(r["cohort"] == "primary" for r in runs) == 96
    assert freeze["all_smokes_before_primary"]
    grouped = defaultdict(dict)
    conditions = defaultdict(list)
    for row in runs:
        assert row["passed"] and row["returncode"] == 0 and not row["timed_out"]
        assert "PASSED:" in Path(row["log"]).read_text()
        result = row["result"]
        assert int(result["clients"]) == 8
        assert int(result["logical_queries"]) == int(result["causal_nodes_replayed"]) == row["steps"] * 8
        assert int(result["logical_handler_iterations"]) == row["steps"] * 8 * row["work"]
        key = (row["kind"], row["steps"], row["work"], row["pattern"], row["seed"])
        conditions[key].append(row)
        if row["cohort"] == "primary":
            grouped[(row["machine"], key)][row["mode"]] = row
    for key, rows in conditions.items():
        assert len({row["result"]["input_hash"] for row in rows}) == 1
        if key[0] == "walk":
            assert len({row["result"]["trace_checksum"] for row in rows}) == 1
            assert len({row["result"]["state_checksum"] for row in rows}) == 1

    primary_keys = sorted({key for _, key in grouped})
    assert len(primary_keys) == 4
    port_control = {"identical_guest_vxbins": [], "identical_unaffected_primary_results": 0}
    example_key = primary_keys[0]
    for mode in grouped[("simt2w4t", example_key)]:
        original = grouped[("simt2w4t", example_key)][mode]
        port1 = grouped[("simt2w4t_port1", example_key)][mode]
        a, b = (fingerprint(row["command"][2]) for row in [original, port1])
        assert a["sha256"] == b["sha256"]
        port_control["identical_guest_vxbins"].append({"mode": mode, "port4": a, "port1": b})
    for key in primary_keys:
        for mode in ["native", "native_grouped", "gpu_queue_local"]:
            assert grouped[("simt2w4t", key)][mode]["result"] == grouped[("simt2w4t_port1", key)][mode]["result"]
            port_control["identical_unaffected_primary_results"] += 1
    summaries = []
    for key in primary_keys:
        for machine in MACHINES:
            rows = grouped[(machine, key)]
            cycles = {mode: int(row["result"]["device_cycles_post_readback_including_flush"])
                      for mode, row in rows.items()}
            native = min(["native", "native_grouped"], key=cycles.get)
            hardware = min(["isa_try_global", "isa_wait_global", "isa_wait_local"], key=cycles.get)
            summaries.append({
                "machine": machine, "handler": key[0], "steps": key[1], "work": key[2],
                "pattern": key[3], "seed": key[4], "cycles": cycles,
                "best_native": native, "best_hardware": hardware,
                "hardware_over_native_cycles": cycles[hardware] / cycles[native],
                "hardware_win": cycles[hardware] < cycles[native],
                "matched_local_hardware_speedup_vs_software": cycles["gpu_queue_local"] / cycles["isa_wait_local"],
                "retries": {mode: int(row["result"]["retries"]) for mode, row in rows.items()},
                "l2_reads": {mode: int(row["result"]["l2_reads"]) for mode, row in rows.items()},
                "l2_writes": {mode: int(row["result"]["l2_writes"]) for mode, row in rows.items()},
            })
    by_key = {(row["machine"], row["handler"], row["work"], row["pattern"]): row for row in summaries}
    paired = []
    for key in primary_keys:
        simt = by_key[("simt2w4t", key[0], key[2], key[3])]
        scalar = by_key[("scalar8w1t", key[0], key[2], key[3])]
        port1 = by_key[("simt2w4t_port1", key[0], key[2], key[3])]
        paired.append({
            "handler": key[0], "work": key[2], "pattern": key[3],
            "simt_native_cycles": simt["cycles"][simt["best_native"]],
            "simt_message_cycles": simt["cycles"][simt["best_hardware"]],
            "scalar_native_cycles": scalar["cycles"][scalar["best_native"]],
            "scalar_message_cycles": scalar["cycles"][scalar["best_hardware"]],
            "scalar_message_speedup_vs_simt_message": simt["cycles"][simt["best_hardware"]] / scalar["cycles"][scalar["best_hardware"]],
            "scalar_message_over_simt_native": scalar["cycles"][scalar["best_hardware"]] / simt["cycles"][simt["best_native"]],
            "scalar_message_beats_simt_native": scalar["cycles"][scalar["best_hardware"]] < simt["cycles"][simt["best_native"]],
            "matched_mode_scalar_speedups_vs_simt": {mode: simt["cycles"][mode] / scalar["cycles"][mode] for mode in simt["cycles"]},
            "matched_mode_scalar_speedups_vs_port1_simt": {mode: port1["cycles"][mode] / scalar["cycles"][mode] for mode in simt["cycles"]},
        })

    body = [
        "# Warp당 thread 1개: 실제 SimX 실행 결과", "",
        "물리 warp 폭을 1로 설정한 별도 모델을 빌드해 실제 GPU 명령과 메시지 전달을 "
        "실행했다. 기존 4-lane warp의 한 lane만 활성화한 실험이 아니다. 모든 경우 "
        "8개의 논리 client가 응답으로 다음 key와 cookie를 결정하며, client당 128회인 "
        "주요 실험에서는 1,024개 query와 모든 결과를 검증했다.", "",
        "## 비교한 자원", "",
        "| 구성 | Warps/core | Threads/warp | 물리 threads/core | 명령 서비스/cycle | Mailbox SRAM/core |",
        "|---|---:|---:|---:|---:|---:|",
    ]
    for machine in MACHINES:
        r = raw["resource_audits"][machine]
        body.append(f'| {machine} | {r["NUM_WARPS"]} | {r["NUM_THREADS"]} | '
                    f'{r["physical_actors_per_core"]} | {r["message_command_budget_per_core"]} | '
                    f'{r["mailbox_data_bytes_per_core"]:,} B |')
    body += [
        "", "공통: cores 2개, RV64, global L1D off, shared L2 1 MiB/1 bank/기본 latency 4, "
        "LocalMem 16 KiB/core와 4 banks/core, LSU pending 8. 64 B frame, 기존 TX/RX FIFO "
        "깊이와 mailbox당 2 slots를 유지한다. UPDATE/RECORD/VM 연산 엔진은 없다.", "",
        "주 비교는 2×4와 8×1로 물리 thread 수를 유지한다. Port1 SIMT는 메시지 장치의 "
        "명령 처리 폭 감소를 별도로 확인한다. 2×1은 물리 thread와 mailbox SRAM이 줄어드는 "
        "보조 비교다. 이 경우 physical client 하나가 논리 client 4개를 round-robin으로 "
        "관리하고, 각 논리 chain의 outstanding query는 최대 하나다.", "",
        "SIMD 폭을 줄이면 ALU/LSU/SFU lane도 4→1로 줄고 LSU line은 32→8 B가 된다. "
        "8 scalar warps에서는 operand collector가 1→2, retained message instruction "
        "capacity가 4→16으로 달라지고 warp PC·제어 상태도 늘어난다. 메모리 bank를 "
        "명시적으로 고정해 기본값 4→1 축소는 방지했다. 동일 면적·전력 비교나 독립 PC "
        "하나만의 효과를 분리한 실험은 아니다. AMO reservation 크기 매크로는 현재 "
        "모델의 강제 용량 제한으로 해석하지 않는다.", "",
        "## 전체 주요 결과", "",
        "각 조건에서 native packed LR/SC와 grouped/seed-hoisted native 중 최선을 선택한다. "
        "HW는 ISA try/global, wait/global, wait/local 중 최선이다. SW owner는 같은 "
        "LocalMem handler를 ordinary global-memory queue로 실행한다. 최소값은 측정한 "
        "성능 범위이며 자동 backend 선택기를 구현한 결과는 아니다. 낮은 cycles가 좋다.", "",
        "| 작업 | Work | 분포 | 구성 | 최선 native | 최선 HW 메시징 | SW owner/local | HW/native |",
        "|---|---:|---|---|---:|---:|---:|---:|",
    ]
    for row in summaries:
        c = row["cycles"]
        body.append(f'| {row["handler"]} | {row["work"]} | {row["pattern"]} | {row["machine"]} | '
                    f'{c[row["best_native"]]:,} | {c[row["best_hardware"]]:,} | '
                    f'{c["gpu_queue_local"]:,} | {row["hardware_over_native_cycles"]:.2f}x |')
    body += [
        "", "## Thread 수를 유지한 2×4 대 8×1", "",
        "아래 메시징 개선 배수가 1보다 크면 scalar messaging이 SIMT messaging보다 빠르다. "
        "마지막 열은 scalar messaging을 원래 SIMT native와 비교한 cycle 배수다.", "",
        "| 작업 | Work | 분포 | SIMT HW cycles | Scalar HW cycles | 메시징 개선 | Scalar HW / SIMT native |",
        "|---|---:|---|---:|---:|---:|---:|",
    ]
    for row in paired:
        body.append(f'| {row["handler"]} | {row["work"]} | {row["pattern"]} | '
                    f'{row["simt_message_cycles"]:,} | {row["scalar_message_cycles"]:,} | '
                    f'{row["scalar_message_speedup_vs_simt_message"]:.2f}x | '
                    f'{row["scalar_message_over_simt_native"]:.2f}x |')
    scalar_wins = [r for r in summaries if r["machine"] == "scalar8w1t" and r["hardware_win"]]
    cross_wins = [r for r in paired if r["scalar_message_beats_simt_native"]]
    body += [
        "", f'8×1에서 최선 HW 메시징이 같은 구성의 최선 native를 이긴 조건: **{len(scalar_wins)}/4**. '
        f'Scalar HW 메시징이 원래 2×4 native를 이긴 조건: **{len(cross_wins)}/4**.', "",
        "명령별 정확한 대응 비율과 local-HW/local-SW 비교는 analysis JSON에 있다. "
        "독립적인 warp가 늘어나면 대기와 분기 경로를 독립적으로 진행시킬 수 있지만, "
        "SIMD 계산·coalescing·같은-key grouping 능력과 명령 처리 폭도 바뀐다. "
        "관측한 차이를 전부 하나의 원인에 귀속하는 cycle 분해는 하지 않았다.", "",
        "Port1/port4 SIMT의 6개 guest vxbin은 byte-identical이고, 영향을 받지 않는 "
        "native/grouped/SW의 12개 primary RESULT 전체가 정확히 동일하다. 이 대조군에서 "
        "메시지 명령 서비스 폭만 바꾼 효과와 scalar 전체 구성의 효과를 구분할 수 있다.", "",
        "## 정답 검증과 측정 범위", "",
        "168/168 실행 PASS: 72개의 zero/one/seven-step smokes가 모든 구성에서 완료된 뒤 "
        "96 primary를 실행했다. 모든 primary는 동일한 초기 입력, 논리 client 8개, "
        "1,024 query, seed 17을 사용한다. Lookup은 모든 구성의 trace와 state checksum도 "
        "동일하다. Mutable update는 실행 순서에 따라 다른 합법적 결과가 가능하므로, "
        "client 인과 관계와 key version 순서의 DAG를 재생해 모든 응답·전이·최종 값을 확인한다.", "",
        "8개의 client DONE 뒤 physical server 전체에 STOP을 보내며 coordinator 자신은 "
        "마지막이다. 모든 message credit과 소프트웨어 slot/ready bit를 비운다. SRAM/global "
        "상태 store의 실제 AMO 응답과 SW ready-clear 응답에 의존하는 publication 순서를 "
        "유지했다. Native packing/unpacking, LocalMem 초기화/export, 종료와 readback/flush를 "
        "포함한 core-zero MCYCLE을 비교한다. Host wall time은 성능 지표가 아니다.", "",
        "라이브러리 16개, source snapshot, 생성 설정, host/guest binary, build/run log를 "
        "별도로 검증했다. 기존 715-run 결과와 이전 빌드는 수정하지 않았다. 새 기능은 "
        "standalone benchmark이며 전체 HClib Selector runtime의 scalar 포팅이 아니다. "
        "실제 GPU나 RTL/PPA를 측정한 결과는 아니다. 기존 atomic timing 모델의 "
        "[검증 한계](forza_atomic_timing_scope.md)도 그대로 적용된다.", "",
        "- [168회 원자료](forza_pgas_scalar_warp_validated_results.json)",
        "- [실행 freeze](forza_pgas_scalar_warp_validated_freeze.json)",
        "- [독립 분석·전체 대응 비율](forza_pgas_scalar_warp_analysis.json)",
        "- Source: `tests/regression/forza_pgas_scalar_warp/`", "",
    ]
    report = DOCS / "forza_pgas_scalar_warp_comparison.md"
    report.write_text("\n".join(body))
    analysis = {
        "verified_utc": datetime.now(timezone.utc).isoformat(),
        "run_count": 168, "passed": 168, "smokes": 72, "primary": 96,
        "scalar_hardware_wins_vs_scalar_native": len(scalar_wins),
        "scalar_hardware_wins_vs_simt_native": len(cross_wins),
        "summary": summaries, "paired_same_context_count": paired, "port_control": port_control,
        "resources": raw["resource_audits"],
        "verified_artifact_count": len(verified), "verified_artifacts": list(verified.values()),
        "raw": fingerprint(raw_path), "freeze": fingerprint(freeze_path),
        "report": fingerprint(report), "finalizer": fingerprint(__file__),
    }
    (DOCS / "forza_pgas_scalar_warp_analysis.json").write_text(json.dumps(analysis, indent=2) + "\n")
    print(json.dumps({k: analysis[k] for k in ["run_count", "passed", "scalar_hardware_wins_vs_scalar_native",
          "scalar_hardware_wins_vs_simt_native", "verified_artifact_count"]}))


if __name__ == "__main__":
    main()
