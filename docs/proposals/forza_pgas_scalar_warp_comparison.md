# Warp당 thread 1개: 실제 SimX 실행 결과

물리 warp 폭을 1로 설정한 별도 모델을 빌드해 실제 GPU 명령과 메시지 전달을 실행했다. 기존 4-lane warp의 한 lane만 활성화한 실험이 아니다. 모든 경우 8개의 논리 client가 응답으로 다음 key와 cookie를 결정하며, client당 128회인 주요 실험에서는 1,024개 query와 모든 결과를 검증했다.

## 비교한 자원

| 구성 | Warps/core | Threads/warp | 물리 threads/core | 명령 서비스/cycle | Mailbox SRAM/core |
|---|---:|---:|---:|---:|---:|
| simt2w4t | 2 | 4 | 8 | 4 | 8,192 B |
| scalar8w1t | 8 | 1 | 8 | 1 | 8,192 B |
| simt2w4t_port1 | 2 | 4 | 8 | 1 | 8,192 B |
| scalar2w1t | 2 | 1 | 2 | 1 | 2,048 B |

공통: cores 2개, RV64, global L1D off, shared L2 1 MiB/1 bank/기본 latency 4, LocalMem 16 KiB/core와 4 banks/core, LSU pending 8. 64 B frame, 기존 TX/RX FIFO 깊이와 mailbox당 2 slots를 유지한다. UPDATE/RECORD/VM 연산 엔진은 없다.

주 비교는 2×4와 8×1로 물리 thread 수를 유지한다. Port1 SIMT는 메시지 장치의 명령 처리 폭 감소를 별도로 확인한다. 2×1은 물리 thread와 mailbox SRAM이 줄어드는 보조 비교다. 이 경우 physical client 하나가 논리 client 4개를 round-robin으로 관리하고, 각 논리 chain의 outstanding query는 최대 하나다.

SIMD 폭을 줄이면 ALU/LSU/SFU lane도 4→1로 줄고 LSU line은 32→8 B가 된다. 8 scalar warps에서는 operand collector가 1→2, retained message instruction capacity가 4→16으로 달라지고 warp PC·제어 상태도 늘어난다. 메모리 bank를 명시적으로 고정해 기본값 4→1 축소는 방지했다. 동일 면적·전력 비교나 독립 PC 하나만의 효과를 분리한 실험은 아니다. AMO reservation 크기 매크로는 현재 모델의 강제 용량 제한으로 해석하지 않는다.

## 전체 주요 결과

각 조건에서 native packed LR/SC와 grouped/seed-hoisted native 중 최선을 선택한다. HW는 ISA try/global, wait/global, wait/local 중 최선이다. SW owner는 같은 LocalMem handler를 ordinary global-memory queue로 실행한다. 최소값은 측정한 성능 범위이며 자동 backend 선택기를 구현한 결과는 아니다. 낮은 cycles가 좋다.

| 작업 | Work | 분포 | 구성 | 최선 native | 최선 HW 메시징 | SW owner/local | HW/native |
|---|---:|---|---|---:|---:|---:|---:|
| update | 1 | hot | simt2w4t | 691,403 | 3,287,853 | 3,827,642 | 4.76x |
| update | 1 | hot | scalar8w1t | 648,698 | 3,271,996 | 3,767,702 | 5.04x |
| update | 1 | hot | simt2w4t_port1 | 691,403 | 3,291,974 | 3,827,642 | 4.76x |
| update | 1 | hot | scalar2w1t | 1,272,122 | 3,136,250 | 4,068,743 | 2.47x |
| update | 1 | random | simt2w4t | 296,225 | 1,035,780 | 1,353,854 | 3.50x |
| update | 1 | random | scalar8w1t | 259,265 | 752,934 | 1,144,974 | 2.90x |
| update | 1 | random | simt2w4t_port1 | 296,225 | 1,057,678 | 1,353,854 | 3.57x |
| update | 1 | random | scalar2w1t | 998,999 | 1,888,746 | 2,526,268 | 1.89x |
| update | 32 | hot | simt2w4t | 1,155,539 | 7,001,896 | 7,542,311 | 6.06x |
| update | 32 | hot | scalar8w1t | 1,151,071 | 7,017,962 | 7,525,792 | 6.10x |
| update | 32 | hot | simt2w4t_port1 | 1,155,539 | 7,006,024 | 7,542,311 | 6.06x |
| update | 32 | hot | scalar2w1t | 3,129,146 | 6,850,297 | 7,655,343 | 2.19x |
| walk | 1 | random | simt2w4t | 109,216 | 924,954 | 1,180,829 | 8.47x |
| walk | 1 | random | scalar8w1t | 101,140 | 657,826 | 1,005,123 | 6.50x |
| walk | 1 | random | simt2w4t_port1 | 109,216 | 911,609 | 1,180,829 | 8.35x |
| walk | 1 | random | scalar2w1t | 555,601 | 1,675,909 | 2,336,460 | 3.02x |

## Thread 수를 유지한 2×4 대 8×1

아래 메시징 개선 배수가 1보다 크면 scalar messaging이 SIMT messaging보다 빠르다. 마지막 열은 scalar messaging을 원래 SIMT native와 비교한 cycle 배수다.

| 작업 | Work | 분포 | SIMT HW cycles | Scalar HW cycles | 메시징 개선 | Scalar HW / SIMT native |
|---|---:|---|---:|---:|---:|---:|
| update | 1 | hot | 3,287,853 | 3,271,996 | 1.00x | 4.73x |
| update | 1 | random | 1,035,780 | 752,934 | 1.38x | 2.54x |
| update | 32 | hot | 7,001,896 | 7,017,962 | 1.00x | 6.07x |
| walk | 1 | random | 924,954 | 657,826 | 1.41x | 6.02x |

8×1에서 최선 HW 메시징이 같은 구성의 최선 native를 이긴 조건: **0/4**. Scalar HW 메시징이 원래 2×4 native를 이긴 조건: **0/4**.

명령별 정확한 대응 비율과 local-HW/local-SW 비교는 analysis JSON에 있다. 독립적인 warp가 늘어나면 대기와 분기 경로를 독립적으로 진행시킬 수 있지만, SIMD 계산·coalescing·같은-key grouping 능력과 명령 처리 폭도 바뀐다. 관측한 차이를 전부 하나의 원인에 귀속하는 cycle 분해는 하지 않았다.

Port1/port4 SIMT의 6개 guest vxbin은 byte-identical이고, 영향을 받지 않는 native/grouped/SW의 12개 primary RESULT 전체가 정확히 동일하다. 이 대조군에서 메시지 명령 서비스 폭만 바꾼 효과와 scalar 전체 구성의 효과를 구분할 수 있다.

## 정답 검증과 측정 범위

168/168 실행 PASS: 72개의 zero/one/seven-step smokes가 모든 구성에서 완료된 뒤 96 primary를 실행했다. 모든 primary는 동일한 초기 입력, 논리 client 8개, 1,024 query, seed 17을 사용한다. Lookup은 모든 구성의 trace와 state checksum도 동일하다. Mutable update는 실행 순서에 따라 다른 합법적 결과가 가능하므로, client 인과 관계와 key version 순서의 DAG를 재생해 모든 응답·전이·최종 값을 확인한다.

8개의 client DONE 뒤 physical server 전체에 STOP을 보내며 coordinator 자신은 마지막이다. 모든 message credit과 소프트웨어 slot/ready bit를 비운다. SRAM/global 상태 store의 실제 AMO 응답과 SW ready-clear 응답에 의존하는 publication 순서를 유지했다. Native packing/unpacking, LocalMem 초기화/export, 종료와 readback/flush를 포함한 core-zero MCYCLE을 비교한다. Host wall time은 성능 지표가 아니다.

라이브러리 16개, source snapshot, 생성 설정, host/guest binary, build/run log를 별도로 검증했다. 기존 715-run 결과와 이전 빌드는 수정하지 않았다. 새 기능은 standalone benchmark이며 전체 HClib Selector runtime의 scalar 포팅이 아니다. 실제 GPU나 RTL/PPA를 측정한 결과는 아니다. 기존 atomic timing 모델의 [검증 한계](forza_atomic_timing_scope.md)도 그대로 적용된다.

- [168회 원자료](forza_pgas_scalar_warp_validated_results.json)
- [실행 freeze](forza_pgas_scalar_warp_validated_freeze.json)
- [독립 분석·전체 대응 비율](forza_pgas_scalar_warp_analysis.json)
- Source: `tests/regression/forza_pgas_scalar_warp/`
