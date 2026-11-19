#!/usr/bin/env python3
"""Reverify the completed message/PGAS experiments and generate their synthesis."""

import hashlib
import json
from collections import defaultdict
from datetime import datetime, timezone
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
DOCS = ROOT / "docs/proposals"
POOL = ROOT.parent / "vortex-forza-pool-experiment/docs/proposals"


def fingerprint(path):
    path = Path(path)
    return {"path": str(path), "bytes": path.stat().st_size,
            "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}


def main():
    components = {
        "bulk": DOCS / "forza_pgas_warp_message_final_freeze.json",
        "pool": POOL / "forza_pgas_pool_fixedguests_freeze.json",
        "feedback": DOCS / "forza_pgas_feedback_final_freeze.json",
        "vm": DOCS / "forza_pgas_vm_final_freeze.json",
        "grouped": DOCS / "forza_pgas_feedback_grouped_final_freeze.json",
        "heavy": DOCS / "forza_pgas_feedback_heavy_validated_freeze.json",
    }
    verified = {}

    def verify(value):
        if isinstance(value, dict):
            if {"path", "bytes", "sha256"} <= value.keys():
                key = (value["path"], value["sha256"])
                if key not in verified:
                    actual = fingerprint(value["path"])
                    assert actual == {k: value[k] for k in actual}, actual
                    verified[key] = actual
            for child in value.values():
                verify(child)
        elif isinstance(value, list):
            for child in value:
                verify(child)

    def load(path):
        document = json.loads(Path(path).read_text())
        verify(document)
        return document

    documents = {name: load(path) for name, path in components.items()}
    # Pool raw results contain fingerprints not repeated in its compact freeze.
    load(POOL / "forza_pgas_pool_fixedguests_results.json")
    unit_paths = [
        ROOT / "build64-forza-isa-unit/forza-isa-unit-manifest.json",
        ROOT / "build64-forza-programmable/tests/unittest/forza_vm/forza-vm-unit-manifest.json",
    ]
    units = [load(path) for path in unit_paths]
    assert units[0]["suite_passed"] == units[0]["suite_runs"] == 2
    assert units[1]["passed"] == units[1]["run_count"] == 36
    assert set(documents["pool"]["unit_passes"]) == {"release", "ASAN+UBSAN+leaks"}

    feedback = {}
    for row in documents["feedback"]["summary"]:
        key = (row["handler"], row["steps"], row["work"], row["pattern"], row["seed"])
        feedback[key] = dict(row["cycles"])
    for name in ["forza_pgas_feedback_heavy_validated_results.json",
                 "forza_pgas_feedback_grouped_validated_results.json"]:
        raw = load(DOCS / name)
        for run in raw["runs"]:
            assert run["passed"] and run["returncode"] == 0
            assert "PASSED" in Path(run["log"]).read_text()
            if run["cohort"] not in ("performance", "heavy"):
                continue
            result = run["result"]
            key = (result["handler"], run["steps"], run["work"], run["pattern"], run["seed"])
            feedback.setdefault(key, {})[run["mode"]] = int(
                result["device_cycles_post_readback_including_flush"])

    feedback_summary = []
    for key, cycles in sorted(feedback.items()):
        native = {m: c for m, c in cycles.items() if m.startswith(("native", "gpu_queue"))}
        hardware = {m: c for m, c in cycles.items() if m.startswith(("mmio", "isa_"))}
        best_native = min(native, key=native.get)
        best_hw = min(hardware, key=hardware.get)
        feedback_summary.append({
            "handler": key[0], "steps": key[1], "work": key[2], "pattern": key[3],
            "seed": key[4], "cycles": cycles, "best_native": best_native,
            "best_hardware": best_hw,
            "hardware_over_native_cycles": hardware[best_hw] / native[best_native],
            "hardware_win": hardware[best_hw] < native[best_native],
        })

    bulk_groups = defaultdict(list)
    for name, cores in [("forza_pgas_warp_message_results.json", 2),
                        ("forza_pgas_warp_message_parallel_results.json", 2),
                        ("forza_pgas_warp_message_scale4_results.json", 4)]:
        raw = load(DOCS / name)
        for run in raw["runs"]:
            assert run["passed"] and run["returncode"] == 0
            assert "PASSED" in Path(run["log"]).read_text()
            if run["cohort"] == "performance":
                bulk_groups[(cores, run["pattern"])].append((
                    run["mode"], int(run["result"]["device_cycles_post_readback_including_flush"])))
    bulk_summary = []
    for (cores, pattern), rows in sorted(bulk_groups.items()):
        native = min((r for r in rows if r[0].startswith("gpu_")), key=lambda r: r[1])
        message = min((r for r in rows if not r[0].startswith("gpu_")), key=lambda r: r[1])
        bulk_summary.append({"cores": cores, "pattern": pattern,
                             "native_mode": native[0], "native_cycles": native[1],
                             "message_mode": message[0], "message_cycles": message[1],
                             "message_over_native_cycles": message[1] / native[1]})

    counts = {}
    for name, doc in documents.items():
        count = doc.get("run_count", doc.get("runs"))
        passed = doc["passed"]
        assert isinstance(count, int) and passed == count
        counts[name] = count
    total = sum(counts.values())
    wins = sum(r["hardware_win"] for r in feedback_summary)
    hot_programs = [r for r in documents["vm"]["summary"]
                    if r["keys"] == 64 and r["pattern"] == "hot" and r["seed"] == 17]

    body = [
        "# 메시징·PGAS 구현과 실행 실험 결과", "",
        "실제 SEND/RECV 명령, 대기·깨우기, batching, warp 협력 처리, 공유 수신 풀, "
        "owner SRAM, 응답 의존 작업과 프로그램 가능한 실행 엔진을 구현하고 실행했다. "
        "현재 측정한 범용 소프트웨어 handler 메시징은 최적화한 일반 GPU를 이기지 못했다. "
        "프로그램 가능한 엔진은 hot-record 작업에서 빨랐지만, actor mailbox 없이 같은 "
        "엔진을 호출해도 같은 이득이 났다. 따라서 메시징 자체의 독점적인 성능 이득은 "
        "이번 실험에서 입증되지 않았다.", "",
        "## 실제로 구현한 것", "",
        "| 가설 | 구현·실행한 비교 | 결과 |",
        "|---|---|---|",
        "| CSR 호출을 명령으로 줄이면 이기는가 | 실제 custom opcode의 SEND/RECV try/wait, credits, register, status | 동일 batch MMIO 대비 bulk 개선은 0.2% 미만; GPU 역전 없음 |",
        "| polling·retry를 없애면 이기는가 | warp만 재우는 RECV.WAIT.ANY, bounded pending work와 깨우기 | 재시도가 0이 되어도 bulk 개선은 약 0.3%; feedback에서는 MMIO보다 개선되지만 GPU보다 느림 |",
        "| 메시지를 묶고 warp가 처리하면 이기는가 | 8/32 작업 range, 목적지 분류·필터링, 4-lane prefix, 독립 key leader 병렬화 | 모두 실행·검증했지만 GPU 역전 없음 |",
        "| core 공유 큐가 부족한 수신 공간을 해결하는가 | 같은 8 KiB를 endpoint별 2-slot 대신 공용 128-slot 풀로 재할당 | 동일 바이너리에서 개선 0.4% 미만; native 결과는 정확히 동일 |",
        "| owner 가까운 SRAM이면 이기는가 | 실제 16 KiB LocalMem의 owner shard, global/LocalMem 두 경로 | 소프트웨어 queue와 GPU SRAM 복제·bucket 대조군까지 포함하면 역전 없음 |",
        "| 응답을 받아야 다음 일이 정해지면 이기는가 | 8개 지속 client의 key·cookie 피드백, mutable version/value, work 1–512 | 아래의 강한 GPU grouped·packed 대조군과 비교 |",
        "| 범용 실행 엔진이 필요했는가 | 같은 tiny processor에 11/11/17 명령의 서로 다른 3개 프로그램 설치 | hot contention에서 이득; direct 호출도 같거나 더 빠름 |", "",
        "SEND.WAIT는 송신 큐 수락을 기다리고, handler 완료는 응답·소프트웨어 프로토콜로 확인한다. "
        "전송 credit가 0이어도 프로그램 작업이 끝났다는 의미는 아니다. 새 명령은 전체 GPU "
        "store에 대한 release fence를 제공하지 않는다. 실험은 immutable 입력 또는 실제 "
        "메모리 ACK에 의존하는 응답을 사용했다.", "",
        "## 범용 메시징: 최적화한 GPU와 비교", "",
        "각 조건에서 측정한 가장 빠른 GPU와 메시징 구현을 비교했다. 이 최소값의 조합은 "
        "성능 범위이며, 자동으로 최적 알고리즘을 고르는 구현은 아니다. 4,096건, 64 records, "
        "seed 17이다. 숫자는 출력 readback/flush 뒤의 core-0 누적 MCYCLE이며 낮을수록 좋다.", "",
        "| Cores | 입력 분포 | 최선 GPU cycles | 최선 메시징 cycles | 메시징/GPU |",
        "|---:|---|---:|---:|---:|",
    ]
    for r in bulk_summary:
        body.append(f'| {r["cores"]} | {r["pattern"]} | {r["native_cycles"]:,} | '
                    f'{r["message_cycles"]:,} | {r["message_over_native_cycles"]:.2f}배 느림 |')
    body += [
        "", "## 피드백 작업과 강한 GPU 대조군", "",
        "각 client는 앞 응답의 key·cookie를 받아야 다음 요청을 만든다. 모든 연산은 일반 "
        "GPU 명령으로 수행한다. 직접 load/lock 외에도 64-bit packed LR/SC, 소프트웨어 "
        "owner queue, 읽기 전용 SRAM 복제를 비교했다. 마지막 grouped GPU는 immutable "
        "계산을 잠금 전에 각 lane에서 병렬로 수행하고, 같은 key의 최대 네 갱신을 한 "
        "잠금으로 순서대로 실행한다. 값·version에 의존하는 부분은 잠금 안에 남는다.", "",
        f"최종 {len(feedback_summary)}개 조건 중 하드웨어 메시징의 GPU 승리는 {wins}개다. "
        "초기 work32 hot의 GPU 4,784,460 cycles보다 grouped GPU가 1,130,910 cycles로 "
        "빨라졌다. 같은 메시징은 6,931,600 cycles이므로, 가까워 보이던 결과도 강한 "
        "GPU 기준선에서는 약 6.13배 느리다.", "",
        "| Handler | Steps/client | Work | 분포 | 최선 GPU cycles | HW 메시징 cycles | HW/GPU |",
        "|---|---:|---:|---|---:|---:|---:|",
    ]
    for r in feedback_summary:
        native = r["cycles"][r["best_native"]]
        hw = r["cycles"][r["best_hardware"]]
        body.append(f'| {r["handler"]} | {r["steps"]} | {r["work"]} | {r["pattern"]} | '
                    f'{native:,} | {hw:,} | {r["hardware_over_native_cycles"]:.2f}배 |')
    body += [
        "", "원본 feedback에서 MMIO까지 비교한 조건들의 wait 경로는 대응 MMIO보다 1.045–1.138배 "
        "빠르다. 이는 유용한 인터페이스 개선이지만 일반 GPU를 이긴 결과는 아니다. "
        "동적 작업은 최대 8개 query가 동시에 살아 있으며, 별도의 포화 bulk·unit 실험이 "
        "full queue·credit 한도·backpressure를 검증한다.", "",
        "## 프로그램 가능한 엔진의 이득은 어디서 왔나", "",
        "고정 count/sum/xor 회로를 쓰지 않고, LD/ST·정수 ALU·분기를 실행하는 동일 엔진에 "
        "세 프로그램을 설치했다. 각 연산은 64 B region 하나를 소유하고 충돌을 직렬화한다. "
        "실제 LocalMem port와 응답을 사용하며 store 완료 전에 결과를 내보내지 않는다.", "",
        "| 64-key hot 프로그램 | 최선 GPU | 엔진+mailbox | 엔진 direct | Mailbox 속도 향상 | Direct 속도 향상 |",
        "|---|---:|---:|---:|---:|---:|",
    ]
    for r in hot_programs:
        body.append(f'| {r["program"]} | {r["best_native"]["cycles"]:,} | '
                    f'{r["vm_mailbox"]["cycles"]:,} | {r["vm_direct"]["cycles"]:,} | '
                    f'{r["mailbox_speedup"]:.2f}배 | {r["direct_speedup"]:.2f}배 |')
    body += [
        "", "Direct는 actor 등록과 mailbox API를 우회하지만 동일한 유한 request/reply "
        "fabric과 completion 저장 공간을 사용한다. GPU의 기존 LSU에 엔진을 붙여 register로 "
        "직접 반환하는 새 경로까지 구현한 것은 아니다. 이 대조군으로 확인한 것은 "
        "actor mailbox가 엔진의 이득에 필수적이지 않다는 점이다. Random/hotset에서는 "
        "GPU가 더 빠른 조건도 많다. 전체 18조건 표는 별도 엔진 보고서에 있다.", "",
        "## 자원과 적용 범위", "",
        "기본 모델은 core=zone이며 core마다 ZEN/ZQM, local·external TX FIFO 각 64 entries, "
        "mailbox index별 RX FIFO 각 64 entries, packet 64 B를 사용한다. 최종 수신 SRAM은 "
        "8 physical endpoints × 8 mailboxes × 2 slots × 64 B = 8 KiB/core이다. "
        "crossbar는 output별 round-robin 중재와 독립 ACK 경로를 갖고, 등록된 유한 채널이 "
        "공간 부족을 앞 단계로 전달한다. 원본 FORZA의 512-entry zone FIFO를 그대로 "
        "복제한 구조는 아니다. 손실 없는 backpressure 모델이므로 NACK/retry backup을 "
        "추가한 결과도 아니다.", "",
        "SEND/RECV의 내용은 8 B opaque token이지만 물리 frame은 64 B다. 큰 immutable "
        "입력은 token이 가리키며 GPU가 읽는 비용을 포함한다. 따라서 실제 8 B 가변 길이 "
        "네트워크를 평가했다는 주장은 하지 않는다. 공유 수신 풀은 data SRAM을 늘리지 "
        "않지만 allocator metadata가 최소 338 B/core 증가한다.", "",
        "Owner state는 기존 16 KiB/core LocalMem을 사용한다. VM은 별도로 1 KiB program "
        "RAM, 4×8×64-bit register contexts, 32-entry request queue, LocalMem client 하나와 "
        "제어 상태를 추가한다. 두 호출 방식은 같은 엔진을 사용한다. C++ 객체 크기는 "
        "SRAM 면적이 아니며 FIFO의 packet 외 routing metadata와 제어 포트 비용도 존재한다.", "",
        "모든 수치는 opt-in SimX 모델의 결과다. 두/네 cores, warp당 4 lanes, 작은 resident "
        "state와 명시된 cache 설정을 사용했다. 실제 상용 GPU, 대형 graph end-to-end, "
        "L2보다 큰 state, arbitrary handler 실행, physical clock·area·power, RTL parity를 "
        "증명한 결과로 일반화할 수 없다.", "",
        "## 검증과 재현 자료", "",
        f"최종 채택한 회귀 실행은 **{total}/{total} PASS**다: "
        + ", ".join(f"{name} {count}" for name, count in counts.items()) + ". "
        "별도로 ISA release/sanitizer 2 suites, 공유 pool 2 suites, 실제 LocalMem 기반 "
        "VM 36 runs가 통과했다. 모든 source snapshot, host/guest binary, library와 log의 "
        "fingerprint를 재확인했다. 전체 manifest에는 실행 명령과 정확한 환경이 있다.", "",
        "모든 이전 값 응답과 최종 상태를 검사한다. 비가환 P2는 per-key ticket 순서로 "
        "모든 전이를 재생한다. Feedback update는 client 인과 순서와 key version 순서의 "
        "DAG를 재생하므로, 합법적인 실행 순서 차이를 잘못 실패 처리하지 않는다. "
        "DONE/STOP, credit drain과 큐 비움까지 검사한다. Compiler divergence와 메모리 "
        "publication 문제가 있었던 초기 실행은 진단 자료로 보존하고 최종 수치에서 제외했다. "
        "소프트웨어 ready-bit clear는 실제 AMOAND 반환값에 의존하도록 고친 뒤 재실행했다.", "",
        "- [전체 검증 manifest](forza_messaging_full_experiments_freeze.json)",
        "- [SEND/RECV 규격](forza_message_isa.md)",
        "- [Bulk·warp·4-core 결과](forza_pgas_warp_message_comparison.md)",
        "- [원본 feedback 200 실행](forza_pgas_feedback_comparison.md)",
        "- [추가 grouped GPU 결과](forza_pgas_feedback_grouped_validated_results.json)",
        "- [Heavy 실행](forza_pgas_feedback_heavy_validated_results.json)",
        "- [프로그램 가능한 엔진 전체 표](forza_pgas_vm_comparison.md)",
        f"- [공유 pool 검증]({POOL / 'forza_pgas_pool_fixedguests_freeze.json'})", "",
        "이번 구현에서 유지할 가치가 확인된 것은 유한 큐·credit·backpressure의 정확한 "
        "진행 보장과 GPU thread의 polling 명령 감소다. 성능 연구의 긍정적인 결과는 제한된 region "
        "트랜잭션을 프로그램 가능하게 데이터 근처에서 실행한 부분이다. 메시징만으로 "
        "최적화한 GPU보다 빨라졌다는 결과는 현재 없다. 같은 가속을 GPU에 허용하면 "
        "사라지는 이득을 메시징의 성과로 계산하지 않았다.", "",
    ]
    report = DOCS / "forza_messaging_full_experiments.md"
    report.write_text("\n".join(body))
    output = {
        "verified_utc": datetime.now(timezone.utc).isoformat(),
        "run_count": total, "passed": total, "component_counts": counts,
        "components": {name: fingerprint(path) for name, path in components.items()},
        "unit_manifests": [fingerprint(path) for path in unit_paths],
        "bulk_summary": bulk_summary, "feedback_summary": feedback_summary,
        "feedback_primary_conditions": len(feedback_summary), "feedback_hardware_wins": wins,
        "vm_representative_hot": hot_programs,
        "verified_artifact_count": len(verified), "verified_artifacts": list(verified.values()),
        "report": fingerprint(report), "finalizer": fingerprint(__file__),
        "scope": "Opt-in SimX experiments only; no RTL/PPA or physical GPU claim.",
    }
    (DOCS / "forza_messaging_full_experiments_freeze.json").write_text(
        json.dumps(output, indent=2) + "\n")
    print(json.dumps({k: output[k] for k in ["run_count", "passed", "component_counts",
                     "feedback_primary_conditions", "feedback_hardware_wins", "verified_artifact_count"]}))


if __name__ == "__main__":
    main()
