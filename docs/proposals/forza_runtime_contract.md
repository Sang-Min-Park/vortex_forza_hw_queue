# FORZA에서 가져온 기능과 Vortex runtime 연결 계약

상태: 하드웨어 큐는 SimX 프로토타입으로 구현·검증됨. 실제 FORZA runtime에
Vortex backend를 추가해 16 actor의 handler·software DONE·ACK drain까지
실행 검증했다. 이 문서의 팀 간 결정 항목은 **제안**이며, runtime 팀이
승인한 합의사항을 뜻하지 않는다. 전체 HClib task/future 및 일반적인 cyclic
actor graph의 종료는 이번 완료 범위에 포함하지 않는다.

## 무엇을 가져왔는가

FORZA RTL을 Vortex에 복사해서 연결한 것은 아니다. FORZA의 메시지 전달
인터페이스와 큐 관리 방식을 참고해 새 C++ 모델을 작성했다. ZEN·ZQM은
core별 `ForzaZone`이 소유하고, core 사이 연결망은 새 `ForzaFabric`이 담당한다.
현재는 RTL 구현이나 FORZA와 동일한 cycle timing을 주장하지 않는다.

검토한 원본은 다음 revision이다. 두 원본 checkout은 검토 시 clean 상태였다.

- Native RTL: `/export/nethomes/spark3100/forzartl_gt`, commit
  `e8c1cabe604f58d5c0cfa14853252933e4a82529`.
- FORZASim-Micro actor adapter: `/export/nethomes/spark3100/forza/forza-runtime`,
  commit `138df12fbe56d2bc248c596841ac23703217e6d8`.
  원본 `inc/selector.forza.h` SHA-256:
  `09f5f20ba85d54149e5d0e5c6512ed700a6f311878a62bd6adec3e7b47be255c`.

| 항목 | Native FORZA 근거 | Vortex 프로토타입에서 한 일 |
| --- | --- | --- |
| 메시지 조립 | HART별 8-bank message buffer와 enqueue control/data. [조립 버퍼](/export/nethomes/spark3100/forzartl_gt/hdl/zen/zen_msg_spawn_ctrl.sv:357) | actor별 7개 payload word를 쌓고 control write로 commit한다. C++로 새로 구현했다. |
| 기본 메시지 형식 | 64-bit control + 7 × 64-bit payload. [control](/export/nethomes/spark3100/forzartl_gt/hdl/include/zen_types.sv:55), [8-word data set](/export/nethomes/spark3100/forzartl_gt/hdl/include/zxb_intf.sv:520) | 총 64B, payload 최대 56B를 유지한다. |
| 목적지 표현 | precinct·zone·component·logical PE와 mailbox. [destination](/export/nethomes/spark3100/forzartl_gt/hdl/include/zxb_intf.sv:526) | 한 precinct에서 `zone=core`로 쓴다. software가 destination zone과 local PE를 control에 넣는다. |
| 등록표 | CSR에 logical PE와 mailbox mask를 쓰고, 요청의 ZAP/HART identity와 결합한다. [형식](/export/nethomes/spark3100/forzartl_gt/hdl/include/csr_intf.sv:138), [요청 identity](/export/nethomes/spark3100/forzartl_gt/hdl/zqm/zqm_ring_reg_top.sv:279), [표 갱신](/export/nethomes/spark3100/forzartl_gt/hdl/zqm/zqm_mbox_top.sv:226) | CSR 요청의 실제 warp·lane을 사용해 `logical PE → physical actor`를 등록한다. 논리 번호와 물리 번호를 다르게 둔 테스트를 통과했다. |
| 송신 FIFO | local·external request queue 각각 64 entries. [설정](/export/nethomes/spark3100/forzartl_gt/hdl/include/zen_types.sv:37) | 각각 64 entries인 core 공용 메시지 FIFO를 뒀다. |
| ZQM 전달 대기 FIFO | mailbox 번호별 FIFO 깊이 512. [설정·인스턴스](/export/nethomes/spark3100/forzartl_gt/hdl/zqm/zqm_mbox_queue.sv:46) | mailbox 번호별로 깊이 64를 선택했다. 용량·타이밍을 원본과 같게 복제한 것은 아니다. |
| 마지막 수신 슬롯 | HART·mailbox별 두 슬롯의 occupied·filled·current·next 상태. [상태 배열](/export/nethomes/spark3100/forzartl_gt/hdl/zqm/zqm_mbox_top.sv:88) | actor·mailbox별 두 슬롯과 Empty/Filling/Ready, read/write slot을 유지한다. |
| 주소로 수신 | dequeue CSR이 현재 메시지 주소를 제공하고 write로 슬롯을 반환한다. [반환](/export/nethomes/spark3100/forzartl_gt/hdl/zqm/zqm_mbox_top.sv:480), [주소 출력](/export/nethomes/spark3100/forzartl_gt/hdl/zqm/zqm_mbox_top.sv:520) | dequeue 주소를 LSU load로 읽을 수 있게 core 전용 SRAM aperture에 연결했다. |
| outstanding | HART·mailbox별 8-bit counter; ACK로 decrement. [카운터](/export/nethomes/spark3100/forzartl_gt/hdl/zen/zen_msg_counters.sv:113), [ACK 처리](/export/nethomes/spark3100/forzartl_gt/hdl/zen/zen_rsp_intf.sv:122) | actor·destination-mailbox 번호별 최대 255개. transport sequence로 ACK identity를 검사한다. 목적지 actor별 counter는 아니다. |
| zone 입출력 버퍼 | 원본 zone request ingress/egress FIFO는 각각 512 entries이며 ZAP·ZEN·CXL 등의 request를 다룬다. [입구](/export/nethomes/spark3100/forzartl_gt/hdl/zxb/zone_req_xbar.sv:76), [출구](/export/nethomes/spark3100/forzartl_gt/hdl/zxb/zone_req_xbar.sv:106) | 메시지 전용 2-entry 연결 경계·채널 버퍼를 사용한다. 원본의 전체 zone request fabric을 이식하지 않았다. |
| 연결망·역압 | 원본의 request/response crossbar와 ready handshake. [zone 연결](/export/nethomes/spark3100/forzartl_gt/hdl/zone/zone.sv:252) | 새 N×N C++ crossbar, output별 round robin, 독립 data/ACK plane을 구현했다. sender-owned registered credit으로 이동 중인 항목의 공간까지 예약한다. |
| 메시지 저장 위치 | queue용 RZA로 memory-write request를 만든다. zone은 HBM용 RZA에 연결한다. [RZA 목적지](/export/nethomes/spark3100/forzartl_gt/hdl/zqm/zqm_mbox_top.sv:333), [HBM RZA 연결](/export/nethomes/spark3100/forzartl_gt/hdl/zone/zone.sv:444) | mailbox 본문은 `0x70000000`부터의 core-local SRAM 모델에 둔다. RZA/HBM 경로를 사용하지 않는다. |
| ACK 발생 시점 | 최종 슬롯이 가능하고 memory-write request가 수락될 때 ACK FIFO에 넣는다. filled/valid는 나중의 RZA 응답에서 갱신한다. [ACK enqueue](/export/nethomes/spark3100/forzartl_gt/hdl/zqm/zqm_mbox_top.sv:416), [filled 완료](/export/nethomes/spark3100/forzartl_gt/hdl/zqm/zqm_mbox_top.sv:470) | 전체 SRAM 본문이 쓰이고 Ready가 된 뒤 ACK를 낸다. 의도적으로 더 강한 delivery boundary다. |
| NACK·재전송 | retry buffer에 메시지를 보관하고 NACK이면 다시 읽는다. [backup write](/export/nethomes/spark3100/forzartl_gt/hdl/zen/zen_msg_spawn_ctrl.sv:638), [ACK/NACK 분기](/export/nethomes/spark3100/forzartl_gt/hdl/zen/zen_rsp_intf.sv:122) | 구현하지 않았다. 유효한 메시지의 혼잡은 lossless backpressure로 처리한다. 잘못된 목적지는 재시도 NACK가 아닌 terminal error ACK다. |
| 막힌 요청 재순환 | ZQM head가 timeout되면 FIFO 뒤로 넣는 경로가 있다. [재삽입](/export/nethomes/spark3100/forzartl_gt/hdl/zqm/zqm_mbox_queue.sv:129) | 구현하지 않았다. mailbox 번호별 FIFO의 막힌 head를 유지하므로 HOL blocking이 있다. |
| 원자적 비차단 commit | 이번 검토의 native enqueue CSR에는 없는 Vortex 확장 | 새 `ZENTRY` CSR `0x852`를 추가한다. 자리·credit 확인과 commit을 한 동작으로 수행하고 accepted/busy/invalid를 반환한다. 이것은 FORZA에서 가져온 기능이 아니라 SIMT runtime 진행을 위해 추가한 기능이다. |
| 원격 메모리 ISA·thread lifecycle | remote atomic/load/store, migration, spawn, pzop, mcopy 등의 시스템 기능 | 이번 메시지 큐 범위에는 포함하지 않는다. |

## runtime이 바로 사용할 수 있는 하드웨어 계약

메시지 전송의 관찰 가능한 순서는 다음과 같다.

```text
payload CSR write × 최대 7 → control CSR commit → outstanding +1
    → bounded FIFO·연결망 → 목적지 SRAM 기록 → Ready → ACK
    → source outstanding -1

수신: status → dequeue 주소 → 본문 복사 → load fence → release CSR
    → 복사본으로 handler 실행
```

delivery ACK는 handler 실행 완료, 슬롯 반환, actor 종료를 뜻하지 않는다.
runtime이 반환한 메시지 주소는 release까지만 유효하다. handler가 원래
SRAM 슬롯을 계속 가리키면 다음 메시지가 같은 슬롯을 덮어쓸 수 있다.

CSR 번호는 native RTL을 기본으로 하되 Vortex와 충돌하는 alias를 옮겼다.
오래된 Micro 라이브러리의 번호·shift를 그대로 사용해서는 안 된다.

| 기능 | Vortex 주소 |
| --- | --- |
| ZENSTAT / ZQMSTAT / registration | `0x804 / 0x805 / 0x806` |
| mailbox 0..7 dequeue | `0x808..0x80f` |
| enqueue control / data | `0x840 / 0x841` |
| 원자적 try-commit `ZENTRY` | `0x852` — 새 Vortex 확장 |
| 8개 outstanding counter가 packed된 CSR | `0x850` |
| 수신 status / dequeue read aliases | `0x851 / 0x858..0x85f` |

Native의 `0xcc2/0xcc3`는 Vortex에서 CORE_ID/ACTIVE_WARPS다. 이 둘을 FORZA용으로
가로채면 core identity와 실행 제어가 깨진다. 실제 번호는
[forza_abi.h](../../sw/common/forza_abi.h)를 기준으로 한다. native control의
zone shift는 15, mailbox shift는 32다. inspected Micro header는 각각 16과
33을 사용하며, 주석의 native 배치와도 일치하지 않는다.
[Micro 상수·주석](/export/nethomes/spark3100/forza/forzasim-micro/forzarev/test/FORZA/forza_message_lib/forza_message_lib.h:24)

`ZENTRY`는 `CSRRW`로 control을 쓰면서 결과를 읽는다. 결과는 `0=accepted`,
`1=busy`, `2=invalid`다. FIFO나 credit이 부족해도 CSR instruction 자체는
완료한다. busy이면 아직 commit되지 않은 조립본을 비우므로 software는 원래
packet을 보존하고 다음 시도에서 payload부터 다시 써야 한다. 기존 blocking
`ZENEQC`의 의미는 바꾸지 않는다. 이 추가 주소·결과·재시도 규칙은 현재 port의
규격이며 팀 간 ABI review가 필요하다.
원자성의 단위는 lane 하나의 메시지다. 예를 들어 두 lane이 FIFO의 마지막
한 자리를 동시에 요청하면 하나는 accepted, 다른 하나는 busy를 받는다.
warp 전체를 모두 수락하거나 모두 거부하는 transaction은 아니다.

## 실제 actor adapter에서 수정해야 하는 지점

| 기존 adapter의 동작 | Vortex에서 필요한 변경 |
| --- | --- |
| `PE_PER_ZAP=1`, ZAP 단위로 rank를 만든다. | core·warp·lane을 읽고 core=zone, thread=actor topology로 바꾼다. |
| simulator의 ECALL backend로 enqueue·dequeue·topology를 호출한다. | CSR 및 LSU mailbox 접근 backend로 바꾼다. |
| `forza_send()`가 `T` 주소에서 항상 56B를 읽는다. | `sizeof(T)<=56`와 trivially-copyable 조건을 검사하고 56B staging packet을 zero-fill한 뒤 `T`만 복사한다. 작은 `T`의 주소를 그대로 넘기면 객체 밖을 읽을 수 있다. |
| `send()`가 매번 `receiveUntil(0)`까지 진행한다. | bounded actor-private outbox에 수락하는 단계와 실제 CSR commit을 분리한다. 유효한 호출에서 `ok()`가 true인데 `send()`가 false이면 outbox full이다. `try_send`/progress는 원자적 `ZENTRY`를 사용한다. |
| `receive(false)`는 메시지가 없으면 return하지만, `receive(true)`는 DONE까지 기다린다. | 비차단 progress를 기본으로 제공한다. lane마다 다른 blocking loop에서 기다리는 사용 패턴을 지원한다고 가정하지 않는다. |
| 수신 시 56B scratch로 복사한 뒤 `process(pkt,-1)`을 호출한다. | scratch·release 순서를 유지하고, handler context의 actor identity와 payload lifetime을 정의한다. source rank 지원은 별도 계약이 필요하다. |
| `doneReceived`를 세어 `forza_n_pes()`와 비교한다. | DONE은 software 메시지로 유지한다. Vortex port는 source-global-PE와 epoch를 DONE payload에 넣고 중복을 검사한다. pending send·수신 handler·ACK drain까지 완료 조건에 포함해야 한다. |
| `hclib::finish(lambda)`는 이 adapter에서 lambda만 호출한다. | 이 코드를 전체 HClib task/future/finish 구현으로 설명하면 안 된다. 필요한 완료 의미를 명시하고 따로 검증한다. |
| 전역 변수·heap·`std::function`을 CPU/SPMD 코드처럼 쓴다. | lane별 context와 고정 크기 inline callback으로 바꾼다. heap 없이 동작하는 범위와 callback capture 한도를 규격화하고 검증한다. |

원본 증거는 [adapter 초기화](/export/nethomes/spark3100/forza/forza-runtime/inc/selector.forza.h:69),
[send 및 pending drain](/export/nethomes/spark3100/forza/forza-runtime/inc/selector.forza.h:196),
[receive·handler·DONE](/export/nethomes/spark3100/forza/forza-runtime/inc/selector.forza.h:280),
[원본 56B receive copy](/export/nethomes/spark3100/forza/forzasim-micro/forzarev/test/FORZA/forza_message_lib/forza_message_lib.h:473)다.

Source rank에는 특히 주의해야 한다. 원본 adapter는 handler의 두 번째 인자로
`-1`을 전달한다. Native RTL은 received control을 source identity로 바꾸는
중간값을 계산하지만, 실제 data[0]에 대입하는 문장이 주석 처리되어 있다.
따라서 이 checkout에서 source rank 전달이 완성됐다고 가정하면 안 된다.
[계산 및 주석 처리된 대입](/export/nethomes/spark3100/forzartl_gt/hdl/zqm/zqm_mbox_top.sv:249)

## runtime 팀과 우선 합의할 항목

아래는 구현자가 제안하는 초기 계약이다. 팀 간 확정 결정으로 표시하지 않는다.

| 결정할 항목 | 초기 권고 | 추가 기능이 필요한 경우 |
| --- | --- | --- |
| PE 배치와 수명 | core당 persistent CTA 하나, 모든 warp·lane을 actor로 등록한다. `globalPE=zone*PEsPerZone+localPE`를 공유하고 등록 완료 후 송신한다. | 일반 GPU grid, actor 재배치·migration·동일 lane 재사용은 등록 해제·상태 drain과 위치 관리 설계가 필요하다. |
| mailbox의 의미 | mailbox 번호 0..7 중 runtime이 사용하는 수를 명시한다. 각 actor마다 동일 번호가 있어도 수신 공간은 분리된다. 현재 Selector는 번호 0부터 사용하므로 actor당 하나만 active하게 둔다. | 동시 Selector를 지원하려면 mailbox-base allocation과 소유권 규격이 유용하다. mailbox 간 우선순위나 보장된 reserved channel이 필요하면 별도 규격이 필요하다. |
| 메시지 type과 크기 | 최대 56B의 trivially-copyable 값만 전달하고 작은 값은 zero-pad한다. | 큰 메시지, segmentation, 원격 pointer payload는 별도 protocol과 메모리 가시성 규칙이 필요하다. |
| payload 소유권 | dequeue 후 runtime 소유 복사본을 만들고 fence·release한 뒤 handler를 호출한다. handler는 원래 SRAM 주소를 보존하지 않는다. | zero-copy handler는 슬롯을 붙잡는 시간·nested send의 liveness를 함께 설계해야 한다. |
| progress와 send 반환 의미 | `send`는 actor-private outbox 수락, `try_send`는 원자적 hardware commit으로 구분한다. full이면 false를 반환하고 호출자가 progress 후 재시도한다. false 후에는 `ok()/error()`로 혼잡과 오류를 구분한다. | 새 `ZENTRY`가 원자적인 accepted/busy/invalid를 제공한다. native FORZA와 다른 ABI이므로 register 주소와 staging 초기화 규칙을 공동 review해야 한다. |
| SIMT 실행 | 같은 warp의 lane은 독립적으로 스케줄되지 않는다. 새 runtime은 원자적 try-commit과 warp 단위 cooperative progress를 사용한다. 실제 검증은 16개 lane 모두 actor이며 별도 service warp를 예약하지 않는다. | 임의 handler가 blocking send/receive를 호출해도 진행한다는 보장은 현재 없다. 다른 lane이 진입한 collective 때문에 필요한 consumer가 실행되지 않는 경우도 점검해야 한다. |
| handler의 허용 동작 | 현재 port의 handler는 비재진입이며 bounded work만 한다. handler의 `send()` 실패도 확인하고 packet을 잃지 않도록 해야 한다. | handler 안에서 outbox가 빌 때까지 spin하거나 blocking done/receive를 부르면 진행을 보장하지 않는다. 더 많은 reply를 만들려면 handler-private retained work 또는 명시적인 추가 outbox 설계가 필요하다. |
| ACK와 actor 완료 | ACK는 delivery, DONE은 actor가 더 이상 그 stream에 application message를 보내지 않겠다는 software protocol로 구분한다. | ACK=0만으로 finish를 판정하면 안 된다. handler가 만든 추가 work나 아직 software queue에 있는 send도 포함해야 한다. |
| DONE의 범위 | sender별/epoch별 마지막 application message 이후에 DONE을 보낸다. 새 port는 DONE payload의 source-global-PE·epoch와 peer별 중복 검사를 사용한다. 현재는 actor의 한 hardware mailbox에 active Selector 하나만 둔다. | epoch는 DONE에만 있으므로 overlapping epoch나 live Selector의 mailbox 공유는 지원하지 않는다. 이를 지원하려면 application message에도 selector/epoch 식별자가 필요하다. |
| source rank | 첫 API가 `process(pkt,-1)`을 유지하는지 명확히 한다. source가 필요한 handler는 임시로 application payload에 명시할 수 있다. | 본문 source header나 수신 metadata CSR을 추가하면 원래 56B payload를 온전히 유지할 수 있다. 실제 wire format 변경이면 raw CSR 테스트와 runtime을 함께 바꿔야 한다. |
| 순서와 가시성 | 같은 sender→같은 destination mailbox의 FIFO 순서를 사용한다. mailbox 사이 순서는 요구하지 않는다. release 전에 load를 fence한다. | 메시지 이전의 일반 global-memory store를 receiver가 읽어야 한다면 noncoherent L1 문제를 포함한 별도 memory-ordering 계약이 필요하다. |
| 오류·재시도 | 잘못된 주소·등록·payload와 혼잡을 구분한다. 혼잡은 would-block/progress로 처리하고 오류는 명시적으로 반환한다. | 현재 sticky error의 복구와 NACK 재전송은 없다. 자동 복구가 필요하면 reset/clear·중복 제거·정확히 한 번 처리 조건이 추가로 필요하다. |
| 기능 발견과 한도 | RV64, VM off, full-width SFU, single SimX worker, 최대 16 zone 및 SRAM 한도를 초기 조건으로 문서화한다. Runtime은 physical cluster 1개, 기본 `MAX_PES=128`, 전체 Selector outbox depth 8이다. | runtime이 compile-time 상수에 의존하지 않게 하려면 capability CSR 또는 runtime query가 유용하다. |
| compiler와 application build | runtime을 사용하는 모든 device kernel은 runtime의 `vortex.mk` build fragment와 divergence compiler flag를 함께 적용한다. 하드웨어·runtime·application의 설정과 toolchain을 맞춘다. | large inlined kernel에서 SIMT control-flow pass가 조용히 생략되는 문제를 compiler 차원에서 진단·차단하는 개선이 필요하다. |

특히 기존 busy polling을 한 뒤의 `ZENEQC` commit도 blocking일 수 있다는 점은 중요하다.
여러 producer가 한 자리만 남은 FIFO를 동시에 확인하면 한 producer가 먼저
차지할 수 있다. software에서 이름만 `try_send`로 바꾸어서는 이 race가 해결되지
않으므로 이번 runtime port는 새 원자적 `ZENTRY`를 사용한다. 원자적 commit을
제공해도 임의 handler의 blocking loop나 잘못된 collective 진입까지 안전해지는
것은 아니다. 검증한 bounded 실행 패턴과 일반적인 deadlock-free runtime 보장은
구분해야 한다.

실제 수정한 runtime은 clean 원본 revision에서 만든 별도 worktree
`/export/nethomes/spark3100/forza-runtime-vortex`다. `inc/selector.forza.h`에서
Vortex backend를 선택하고 `inc/selector.vortex.h`가 cooperative Selector를
제공하는 구성을 사용한다. 원본 Micro backend는 보존한다. 실제 build·실행
성공 여부는 아래 결과 기록과 구분한다.

## 현재 Vortex Selector의 구체적인 의미

다음은 코드에서 확인한 현재 동작이다. 기존 HClib 전체 API와 같은 의미라는
가정 대신 이 계약을 runtime 팀과 review해야 한다.

| API·상태 | 현재 의미 |
| --- | --- |
| `Selector<N,T>` | actor 한 명이 소유하는 객체다. `N=1..8`, `T`는 trivially-copyable이고 최대 56B다. 객체를 복사·이동해서 actor를 바꾸지 않는다. |
| `send(mailbox,pkt,rank)` | actor-private outbox에 값을 복사한다. true는 software queue 수락이며 hardware delivery 완료가 아니다. false이면 packet이 수락되지 않았으므로 caller가 보존해야 한다. `ok()`가 false이면 오류를 확인한다. |
| software outbox | 기본 8 entries는 **Selector 전체의 합계**다. mailbox마다 8개인 것이 아니다. 이 큐는 보통의 thread-private C++ 상태이므로 spill되면 DRAM에 놓일 수 있다. 공유 global-memory receive mailbox로 되돌아간 것은 아니며, 수신 본문과 슬롯 소유권은 계속 ZQM SRAM이 관리한다. |
| `try_send(mailbox,pkt,rank)` | 앞선 software outbox가 비어 있을 때 원자적 hardware commit을 시도한다. outbox가 남아 있으면 mailbox가 달라도 순서 보존을 위해 false다. |
| `progress()` | mailbox마다 최대 한 메시지를 복사·release·처리하고, outbox head 한 개와 현재 DONE stage를 진행시킨다. handler 재진입을 막으므로 handler 안의 `progress()`는 추가 진행을 하지 않는다. |
| `request_done()` | 새 application send를 mailbox 0부터 순차적으로 닫기 시작한다. 호출 즉시 모든 mailbox가 닫히는 것은 아니다. |
| 단계별 closure | 현재 mailbox의 software pending과 hardware outstanding이 drain된 뒤 모든 PE에 DONE을 보낸다. 모든 peer DONE 및 자신의 ACK drain을 확인하면 다음 mailbox를 닫는다. 예를 들어 mailbox 0 request handler가 mailbox 1 reply를 만드는 DAG를 지원하기 위한 순서다. |
| 허용하지 않는 closure | 이미 닫히는 mailbox로의 새 send는 protocol error다. mailbox 1 handler가 닫힌 mailbox 0으로 다시 보내는 cyclic backedge나 무한 work 생성은 지원하지 않는다. |
| `done()` | actor가 자신의 phase를 끝내는 협력적 진행 루프다. warp 안의 lane들이 완료 시점이 달라도 모든 참여 lane이 끝날 때까지 progress한다. 같은 warp의 참여 lane들은 서로 다른 collective나 무관한 blocking loop로 빠지지 않아야 한다. |
| epoch와 Selector 동시 사용 | epoch는 DONE payload에만 있다. application payload는 epoch/Selector ID를 담지 않으므로 같은 hardware mailbox를 공유하는 live Selector나 겹치는 epoch는 지원하지 않는다. 현재 Selector는 mailbox 번호를 0부터 연속 사용하므로 보통 actor당 하나의 active Selector를 둔다. |
| callback | heap `std::function` 대신 128B inline storage를 사용한다. capture는 trivially-copyable·trivially-destructible이고 alignment 조건을 만족해야 한다. 같은 active warp의 callback target은 같은 함수로 사용하며 capture 값은 actor마다 달라도 된다. application handler의 sender 인자는 계속 `-1`이다. |
| `hclib::finish()` | 원본 adapter처럼 body를 호출하는 scope다. HClib task/future를 추적하는 finish가 아니다. `Selector.done()`의 검증은 해당 actor phase의 메시지·DONE 종료를 뜻한다. |

코드 근거:
[Selector와 outbox](/export/nethomes/spark3100/forza-runtime-vortex/inc/selector.vortex.h:155),
[progress](/export/nethomes/spark3100/forza-runtime-vortex/inc/selector.vortex.h:238),
[closure](/export/nethomes/spark3100/forza-runtime-vortex/inc/selector.vortex.h:385),
[CSR backend](/export/nethomes/spark3100/forza-runtime-vortex/inc/forza_message_lib.vortex.h:122).

서로 다른 lane이 서로 다른 callable type을 설치하는 indirect dispatch는 아직
검증 범위가 아니다. 현재 SimX의 JALR은 active warp에 하나의 target PC를
선택하므로, 그러한 사용은 compiler의 masked indirect-call lowering이나
별도 runtime dispatch 설계·검증이 필요하다.
[현재 JALR target 선택](../../sim/simx/alu_unit.cpp:412)

`done()`으로 종료가 되더라도 모든 형태의 actor graph 종료가 해결됐다는 뜻은
아니다. 현재 closure는 mailbox 순서에 맞춘 유한 DAG와 명시적인 송신 종료를
대상으로 한다. 임의의 cyclic graph, 동적인 task/future, handler 안의 blocking
연산까지 완료를 판정하려면 별도 termination protocol이 필요하다.

## compiler·runtime·application의 필수 build 계약

현재 toolchain에는 큰 device 함수의 제어 흐름을 처리할 때 주의할 조건이 있다.
Vortex divergence pass의 기본 100-basic-block guard를 넘으면
`StructurizeCFG`와 SIMT split/join 처리가 조용히 생략될 수 있다. runtime의
progress·handler가 kernel 안에 inline되면 이 한도를 넘을 수 있다. 그 경우
lane별 조건 분기가 올바르게 mask되지 않아 데이터 처리나 완료 조건이 틀릴 수
있다. 이는 FIFO·CSR 프로토콜 자체를 변경해서 해결할 문제가 아니다.

기존 repository도 같은 문제를 software graphics 경로에서 명시하고 있다.
[기존 header의 설명·guard](../../sw/common/gfx_sw.h:1114),
[기존 build fragment](../../sw/gfx/libgfx_sw.mk:4).
이번 runtime 검증에서도 lane별 분기가 있는 큰 `kernel_main`에 conditional
branch가 존재하지만 SIMT split이 생성되지 않는 증상이 관찰되었다. 작은 helper에
split이 있다는 것만으로 전체 kernel의 변환이 정상임을 판단할 수는 없다.

따라서 현재 port를 사용하는 device kernel에는 다음 flag를 적용한다.

```text
-mllvm -vortex-divergence-max-bbs=512
-DFORZA_VORTEX_DIVERGENCE_OK
```

이 flag 묶음은 runtime worktree의 `vortex.mk`로 제공하고 runtime header가
marker 누락을 compile error로 검출하는 계약을 사용한다. application 팀은
macro만 임의로 정의해서 guard를 우회하면 안 된다. runtime library만 이 flag로
컴파일하는 것으로도 충분하지 않으며, inline된 runtime과 handler를 포함하는
**최종 device kernel**에 적용해야 한다.

512는 현재 build에서 사용하는 상한이며 임의 크기의 kernel을 보장하는 값은
아니다. compiler·optimization 변경이나 더 큰 handler를 도입하면 생성된
kernel의 필요한 SIMT split/join과 실행 결과를 다시 확인해야 한다. 최종
검증에는 같은 source와 compiler flag로 만든 artifact를 사용하고, handler
횟수·checksum·DONE/ACK 조건을 약화하지 않는다. 장기적으로는 pass 생략을
명시적인 compiler 진단으로 바꾸고 toolchain version·flag를 공통 build
설정으로 관리하는 것을 권고한다.

최종 검증은 원래의 lane별 ternary 조건을 복원한 source로 통과했다.
상태 기대값을 완화해서 통과시킨 것이 아니라 build 설정을 바로잡은 결과다.
생성된 `build64/tests/regression/forza_runtime/kernel.dump`에서 `kernel_main`
진입부를 포함한 split/join 명령도 확인했다.

## 기존 application도 확인해야 하는 부분

새 backend를 선택하는 것만으로 모든 upstream application의 동작이 같아지지는
않는다. 특히 기존 bulk send loop가 반환값을 무시하면 bounded outbox가 찼을 때
application packet을 잃는다. 예를 들어 원본 BFS의
[send loop](/export/nethomes/spark3100/forza/forza-runtime/test/bfs/main.cpp:64)는
`tSel.send(...)`의 반환값을 확인하지 않는다. Vortex application은 packet을
보존하고 수락됐을 때만 다음 item으로 이동하며, 그 사이 progress를 실행하도록
수정해야 한다.

```text
warp의 모든 참여 actor가 제출을 끝낼 때까지:
    내 pending item이 있고 send/try_send가 true이면 다음 item으로 이동
    Selector.progress()
    오류를 검사
모든 참여 actor가 제출을 마친 뒤 합의한 DONE 단계 진행
```

완료한 lane도 같은 warp의 다른 lane이 끝날 때까지 cooperative loop에
참여해야 한다. handler 안의 무제한 send loop도 그대로 허용되는 것은 아니다.
handler가 outbox full을 만났을 때 작업을 보존하거나 명시적으로 실패하도록
application 계약을 정해야 한다.

현재 port는 고정 inline callback을 제공하며, application의 `std::vector`,
graph container, 동적 allocator, 원격 pointer와 memory coherence를 통째로
이식하지 않았다. BFS·graph application이 실제로 통과했다는 주장과
이번 Selector transport·handler 검증은 구분한다.

## 검증 기록과 남은 확인

기존 [하드웨어 큐 검증 기록](forza_hwqueue.md)은 raw CSR/LSU 경로에서 16 actor,
최대 24,576 messages, FIFO pressure·ACK drain·오류·255-credit 한도와
cross-warp CSR 진행을 검증한다. 이 결과만으로 실제 Selector handler나
software DONE completion이 실행됐다고 주장하지 않는다.

실제 runtime port는 두 core, core당 두 warp, warp당 네 lane, 총 16 actor로
검증했다. payload `T`는 32B로 56B보다 작다. 모든 actor가 두 mailbox와
세 가지 순차 epoch를 사용한다.

1. global-PE ring으로 다음 actor에게 전달한다.
2. 모든 actor가 actor 0에 보내는 fan-in을 처리한다.
3. mailbox 0 request handler가 mailbox 1 reply를 보내고, 아직 마지막 reply를
   받지 못한 actor도 `done()`에 진입해 진행을 계속한다.

각 실행은 handler 횟수·checksum·목적지·sender별 FIFO·모든 peer의 DONE과
최종 outstanding 0을 검사한다. software DONE은 각 epoch·mailbox에서 각
sender가 모든 16개 peer에게 보내므로, 한 실행의 DONE marker 수는
`16 × 16 × 2 × 3 = 1,536`개다. 이는 application DATA handler 수와 별도다.

| 실제 실행 | 결과 | 기록 |
| --- | --- | --- |
| `forza_runtime -n2` | PASS: DATA handler 192회 + software DONE 1,536개, ACK drain | [validation-n2.log](../../build64/tests/regression/forza_runtime/validation-n2.log) |
| `forza_runtime -n8` | PASS: DATA handler 768회 + software DONE 1,536개. 13 actor가 마지막 reply 처리 전에 `done()`에 진입했으며, `done()` 반환 시에는 reply를 모두 처리한 상태임 | [validation-n8.log](../../build64/tests/regression/forza_runtime/validation-n8.log) |
| `forza_runtime -n32` | PASS: DATA handler 3,072회 + software DONE 1,536개. busy 재시도 7,133회, 마지막 reply 전에 `done()`에 진입한 actor 4개, 최종 peer DONE·ACK 조건 충족 | [validation-n32.log](../../build64/tests/regression/forza_runtime/validation-n32.log) |
| 실제 Selector header + mock transport의 ASan/UBSan fixture | PASS: 8B payload zero-padding, 8-entry outbox full, staged handler reply, duplicate DONE dedup, stale epoch reject | [forza-runtime-host-contract.log](../../build64/forza-runtime-host-contract.log) |
| atomic `ZENTRY`와 credit 한도 fixture | PASS: 마지막 FIFO 공간의 두 lane 경쟁, busy 재시도, 255 outstanding 한도·다른 warp의 CSR 진행 | [forza-zone-test.log](../../build64-default/forza-zone-test.log) |

ASan/UBSan fixture는 실제 `selector.vortex.h`를 사용하되 transport만 mock으로
바꾼 host test다. device의 CSR·SRAM 경로를 대신 검증하는 것으로 설명하지
않는다. 실제 device 경로는 위 SimX kernel 실행으로 확인했다. 세 epoch 사이에는
완료 후 hardware barrier를 두어 겹치지 않게 했다. ring·fan-in 단계는 application
data 처리 후 DONE을 시작하며, request/reply 단계는 reply가 남은 상태에서
`done()`을 시작해 staged closure를 검증한다.

남은 기능과 검증 범위는 다음과 같다.

| 검증 | 필요한 관찰 결과 | 상태 |
| --- | --- | --- |
| mailbox 0 → 1 request/reply DAG | handler-generated reply를 완료하고 DONE·ACK drain | 위 실제 kernel·host fixture에서 PASS |
| arbitrary cyclic actor graph | 이미 닫힌 stream으로의 backedge와 동적인 추가 work까지 종료를 판정 | 미지원; 별도 termination protocol 필요 |
| overlapping epoch·동시 Selector | DATA에도 selector/epoch를 식별해 서로 섞이지 않음 | 미지원; mailbox allocation·wire protocol 합의 필요 |
| 전체 HClib task/future/finish | 동적 task와 future까지 추적해 finish 완료 | 이번 포팅 범위 아님 |
| graph container·BFS 등 실제 application | bulk send 재시도와 allocator·pointer·coherence까지 맞춰 원본 결과 검증 | 아직 포팅·검증하지 않음 |

실제 수정 파일은
[Selector entry](/export/nethomes/spark3100/forza-runtime-vortex/inc/selector.forza.h),
[cooperative Selector](/export/nethomes/spark3100/forza-runtime-vortex/inc/selector.vortex.h),
[CSR backend](/export/nethomes/spark3100/forza-runtime-vortex/inc/forza_message_lib.vortex.h),
[build fragment](/export/nethomes/spark3100/forza-runtime-vortex/vortex.mk),
[sanitizer fixture](/export/nethomes/spark3100/forza-runtime-vortex/test/vortex_contract/main.cpp)다.
실제 device test는 [kernel](../../tests/regression/forza_runtime/kernel.cpp)과
[host 검증](../../tests/regression/forza_runtime/main.cpp)을 참고한다.

동일한 저장 workspace에서 재현하는 명령은 다음과 같다. source Makefile이나
설정이 바뀌면 generated build tree를 먼저 갱신한다.

```bash
cd /export/nethomes/spark3100/vortex-forza-hwqueue/build64
../configure --xlen=64 --tooldir=/export/nethomes/spark3100/tools \
  --osversion=ubuntu/focal --prefix="$PWD/install"
export THIRD_PARTY_DIR=/export/nethomes/spark3100/vortex-forza-step12/third_party
export FORZA_RUNTIME_HOME=/export/nethomes/spark3100/forza-runtime-vortex
export CONFIGS='-DVX_CFG_EXT_FORZA_ENABLE -DVX_CFG_NUM_CORES=2 -DVX_CFG_NUM_WARPS=2 -DVX_CFG_NUM_THREADS=4'
env -u DEBUG make -C sw/runtime stub simx
env -u DEBUG make -C tests/regression/forza_runtime
(
  cd tests/regression/forza_runtime
  export VORTEX_DRIVER=simx
  export LD_LIBRARY_PATH="$PWD/../../../sw/runtime${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
  ./forza_runtime -n32 -p4000
)
env -u DEBUG make -C "$FORZA_RUNTIME_HOME/test/vortex_contract" run
```

긴 device 실행 중 CP의 기존 10초 command-retire 경고가 출력될 수 있다.
위 기록에서는 kernel이 정상 완료되고 최종 검증을 통과했다. busy 재시도 횟수는
관찰된 특정 build·배치의 값이며 성능 보장이나 ABI 고정값이 아니다.
