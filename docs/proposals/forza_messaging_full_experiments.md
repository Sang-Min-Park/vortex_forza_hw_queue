# 메시징·PGAS 구현과 실행 실험 결과

실제 SEND/RECV 명령, 대기·깨우기, batching, warp 협력 처리, 공유 수신 풀, owner SRAM, 응답 의존 작업과 프로그램 가능한 실행 엔진을 구현하고 실행했다. 현재 측정한 범용 소프트웨어 handler 메시징은 최적화한 일반 GPU를 이기지 못했다. 프로그램 가능한 엔진은 hot-record 작업에서 빨랐지만, actor mailbox 없이 같은 엔진을 호출해도 같은 이득이 났다. 따라서 메시징 자체의 독점적인 성능 이득은 이번 실험에서 입증되지 않았다.

## 실제로 구현한 것

| 가설 | 구현·실행한 비교 | 결과 |
|---|---|---|
| CSR 호출을 명령으로 줄이면 이기는가 | 실제 custom opcode의 SEND/RECV try/wait, credits, register, status | 동일 batch MMIO 대비 bulk 개선은 0.2% 미만; GPU 역전 없음 |
| polling·retry를 없애면 이기는가 | warp만 재우는 RECV.WAIT.ANY, bounded pending work와 깨우기 | 재시도가 0이 되어도 bulk 개선은 약 0.3%; feedback에서는 MMIO보다 개선되지만 GPU보다 느림 |
| 메시지를 묶고 warp가 처리하면 이기는가 | 8/32 작업 range, 목적지 분류·필터링, 4-lane prefix, 독립 key leader 병렬화 | 모두 실행·검증했지만 GPU 역전 없음 |
| core 공유 큐가 부족한 수신 공간을 해결하는가 | 같은 8 KiB를 endpoint별 2-slot 대신 공용 128-slot 풀로 재할당 | 동일 바이너리에서 개선 0.4% 미만; native 결과는 정확히 동일 |
| owner 가까운 SRAM이면 이기는가 | 실제 16 KiB LocalMem의 owner shard, global/LocalMem 두 경로 | 소프트웨어 queue와 GPU SRAM 복제·bucket 대조군까지 포함하면 역전 없음 |
| 응답을 받아야 다음 일이 정해지면 이기는가 | 8개 지속 client의 key·cookie 피드백, mutable version/value, work 1–512 | 아래의 강한 GPU grouped·packed 대조군과 비교 |
| 범용 실행 엔진이 필요했는가 | 같은 tiny processor에 11/11/17 명령의 서로 다른 3개 프로그램 설치 | hot contention에서 이득; direct 호출도 같거나 더 빠름 |

SEND.WAIT는 송신 큐 수락을 기다리고, handler 완료는 응답·소프트웨어 프로토콜로 확인한다. 전송 credit가 0이어도 프로그램 작업이 끝났다는 의미는 아니다. 새 명령은 전체 GPU store에 대한 release fence를 제공하지 않는다. 실험은 immutable 입력 또는 실제 메모리 ACK에 의존하는 응답을 사용했다.

## 범용 메시징: 최적화한 GPU와 비교

각 조건에서 측정한 가장 빠른 GPU와 메시징 구현을 비교했다. 이 최소값의 조합은 성능 범위이며, 자동으로 최적 알고리즘을 고르는 구현은 아니다. 4,096건, 64 records, seed 17이다. 숫자는 출력 readback/flush 뒤의 core-0 누적 MCYCLE이며 낮을수록 좋다.

| Cores | 입력 분포 | 최선 GPU cycles | 최선 메시징 cycles | 메시징/GPU |
|---:|---|---:|---:|---:|
| 2 | hot | 1,005,670 | 2,358,431 | 2.35배 느림 |
| 2 | hotset | 382,097 | 2,619,747 | 6.86배 느림 |
| 2 | random | 212,264 | 2,838,931 | 13.37배 느림 |
| 4 | hot | 603,756 | 2,356,497 | 3.90배 느림 |
| 4 | hotset | 203,612 | 1,603,073 | 7.87배 느림 |
| 4 | random | 122,621 | 1,842,186 | 15.02배 느림 |

## 피드백 작업과 강한 GPU 대조군

각 client는 앞 응답의 key·cookie를 받아야 다음 요청을 만든다. 모든 연산은 일반 GPU 명령으로 수행한다. 직접 load/lock 외에도 64-bit packed LR/SC, 소프트웨어 owner queue, 읽기 전용 SRAM 복제를 비교했다. 마지막 grouped GPU는 immutable 계산을 잠금 전에 각 lane에서 병렬로 수행하고, 같은 key의 최대 네 갱신을 한 잠금으로 순서대로 실행한다. 값·version에 의존하는 부분은 잠금 안에 남는다.

최종 22개 조건 중 하드웨어 메시징의 GPU 승리는 0개다. 초기 work32 hot의 GPU 4,784,460 cycles보다 grouped GPU가 1,130,910 cycles로 빨라졌다. 같은 메시징은 6,931,600 cycles이므로, 가까워 보이던 결과도 강한 GPU 기준선에서는 약 6.13배 느리다.

| Handler | Steps/client | Work | 분포 | 최선 GPU cycles | HW 메시징 cycles | HW/GPU |
|---|---:|---:|---|---:|---:|---:|
| update | 128 | 1 | hot | 652,742 | 3,217,559 | 4.93배 |
| update | 128 | 1 | random | 198,710 | 895,494 | 4.51배 |
| update | 128 | 8 | hot | 768,678 | 4,056,207 | 5.28배 |
| update | 128 | 8 | random | 386,678 | 1,089,741 | 2.82배 |
| update | 128 | 32 | hot | 1,130,910 | 6,931,600 | 6.13배 |
| update | 128 | 32 | random | 773,167 | 1,904,051 | 2.46배 |
| update | 128 | 128 | hot | 2,579,838 | 18,433,180 | 7.15배 |
| update | 128 | 128 | random | 2,333,406 | 4,956,963 | 2.12배 |
| update | 128 | 512 | hot | 8,375,550 | 64,439,447 | 7.69배 |
| update | 128 | 512 | random | 8,685,470 | 16,766,445 | 1.93배 |
| update | 512 | 1 | hot | 2,595,017 | 12,794,903 | 4.93배 |
| update | 512 | 1 | random | 805,516 | 3,520,716 | 4.37배 |
| update | 512 | 8 | hot | 7,622,607 | 16,149,519 | 2.12배 |
| walk | 128 | 1 | hot | 132,468 | 2,679,867 | 20.23배 |
| walk | 128 | 1 | random | 132,569 | 766,646 | 5.78배 |
| walk | 128 | 8 | hot | 317,940 | 4,170,823 | 13.12배 |
| walk | 128 | 8 | random | 318,017 | 1,133,355 | 3.56배 |
| walk | 128 | 32 | hot | 953,844 | 9,282,624 | 9.73배 |
| walk | 128 | 32 | random | 953,920 | 2,535,351 | 2.66배 |
| walk | 512 | 1 | hot | 522,243 | 10,644,411 | 20.38배 |
| walk | 512 | 1 | random | 522,344 | 2,980,407 | 5.71배 |
| walk | 512 | 8 | hot | 1,264,131 | 16,608,199 | 13.14배 |

원본 feedback에서 MMIO까지 비교한 조건들의 wait 경로는 대응 MMIO보다 1.045–1.138배 빠르다. 이는 유용한 인터페이스 개선이지만 일반 GPU를 이긴 결과는 아니다. 동적 작업은 최대 8개 query가 동시에 살아 있으며, 별도의 포화 bulk·unit 실험이 full queue·credit 한도·backpressure를 검증한다.

## 프로그램 가능한 엔진의 이득은 어디서 왔나

고정 count/sum/xor 회로를 쓰지 않고, LD/ST·정수 ALU·분기를 실행하는 동일 엔진에 세 프로그램을 설치했다. 각 연산은 64 B region 하나를 소유하고 충돌을 직렬화한다. 실제 LocalMem port와 응답을 사용하며 store 완료 전에 결과를 내보내지 않는다.

| 64-key hot 프로그램 | 최선 GPU | 엔진+mailbox | 엔진 direct | Mailbox 속도 향상 | Direct 속도 향상 |
|---|---:|---:|---:|---:|---:|
| branch | 1,334,111 | 438,626 | 432,575 | 3.04배 | 3.08배 |
| minmax | 1,101,501 | 418,023 | 413,087 | 2.64배 | 2.67배 |
| sumxor | 995,071 | 422,279 | 416,739 | 2.36배 | 2.39배 |

Direct는 actor 등록과 mailbox API를 우회하지만 동일한 유한 request/reply fabric과 completion 저장 공간을 사용한다. GPU의 기존 LSU에 엔진을 붙여 register로 직접 반환하는 새 경로까지 구현한 것은 아니다. 이 대조군으로 확인한 것은 actor mailbox가 엔진의 이득에 필수적이지 않다는 점이다. Random/hotset에서는 GPU가 더 빠른 조건도 많다. 전체 18조건 표는 별도 엔진 보고서에 있다.

## 자원과 적용 범위

기본 모델은 core=zone이며 core마다 ZEN/ZQM, local·external TX FIFO 각 64 entries, mailbox index별 RX FIFO 각 64 entries, packet 64 B를 사용한다. 최종 수신 SRAM은 8 physical endpoints × 8 mailboxes × 2 slots × 64 B = 8 KiB/core이다. crossbar는 output별 round-robin 중재와 독립 ACK 경로를 갖고, 등록된 유한 채널이 공간 부족을 앞 단계로 전달한다. 원본 FORZA의 512-entry zone FIFO를 그대로 복제한 구조는 아니다. 손실 없는 backpressure 모델이므로 NACK/retry backup을 추가한 결과도 아니다.

SEND/RECV의 내용은 8 B opaque token이지만 물리 frame은 64 B다. 큰 immutable 입력은 token이 가리키며 GPU가 읽는 비용을 포함한다. 따라서 실제 8 B 가변 길이 네트워크를 평가했다는 주장은 하지 않는다. 공유 수신 풀은 data SRAM을 늘리지 않지만 allocator metadata가 최소 338 B/core 증가한다.

Owner state는 기존 16 KiB/core LocalMem을 사용한다. VM은 별도로 1 KiB program RAM, 4×8×64-bit register contexts, 32-entry request queue, LocalMem client 하나와 제어 상태를 추가한다. 두 호출 방식은 같은 엔진을 사용한다. C++ 객체 크기는 SRAM 면적이 아니며 FIFO의 packet 외 routing metadata와 제어 포트 비용도 존재한다.

모든 수치는 opt-in SimX 모델의 결과다. 두/네 cores, warp당 4 lanes, 작은 resident state와 명시된 cache 설정을 사용했다. 실제 상용 GPU, 대형 graph end-to-end, L2보다 큰 state, arbitrary handler 실행, physical clock·area·power, RTL parity를 증명한 결과로 일반화할 수 없다.

## 검증과 재현 자료

최종 채택한 회귀 실행은 **715/715 PASS**다: bulk 204, pool 30, feedback 200, vm 248, grouped 13, heavy 20. 별도로 ISA release/sanitizer 2 suites, 공유 pool 2 suites, 실제 LocalMem 기반 VM 36 runs가 통과했다. 모든 source snapshot, host/guest binary, library와 log의 fingerprint를 재확인했다. 전체 manifest에는 실행 명령과 정확한 환경이 있다.

모든 이전 값 응답과 최종 상태를 검사한다. 비가환 P2는 per-key ticket 순서로 모든 전이를 재생한다. Feedback update는 client 인과 순서와 key version 순서의 DAG를 재생하므로, 합법적인 실행 순서 차이를 잘못 실패 처리하지 않는다. DONE/STOP, credit drain과 큐 비움까지 검사한다. Compiler divergence와 메모리 publication 문제가 있었던 초기 실행은 진단 자료로 보존하고 최종 수치에서 제외했다. 소프트웨어 ready-bit clear는 실제 AMOAND 반환값에 의존하도록 고친 뒤 재실행했다.

- [전체 검증 manifest](forza_messaging_full_experiments_freeze.json)
- [SEND/RECV 규격](forza_message_isa.md)
- [Bulk·warp·4-core 결과](forza_pgas_warp_message_comparison.md)
- [원본 feedback 200 실행](forza_pgas_feedback_comparison.md)
- [추가 grouped GPU 결과](forza_pgas_feedback_grouped_validated_results.json)
- [Heavy 실행](forza_pgas_feedback_heavy_validated_results.json)
- [프로그램 가능한 엔진 전체 표](forza_pgas_vm_comparison.md)
- [공유 pool 검증](/export/nethomes/spark3100/vortex-forza-pool-experiment/docs/proposals/forza_pgas_pool_fixedguests_freeze.json)

이번 구현에서 유지할 가치가 확인된 것은 유한 큐·credit·backpressure의 정확한 진행 보장과 GPU thread의 polling 명령 감소다. 성능 연구의 긍정적인 결과는 제한된 region 트랜잭션을 프로그램 가능하게 데이터 근처에서 실행한 부분이다. 메시징만으로 최적화한 GPU보다 빨라졌다는 결과는 현재 없다. 같은 가속을 GPU에 허용하면 사라지는 이득을 메시징의 성과로 계산하지 않았다.
