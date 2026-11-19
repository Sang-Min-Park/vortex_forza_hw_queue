# CSR 대신 메시지 장치를 사용하는 PGAS 실험

FORZA의 메시지용 CSR을 사용하지 않고도 송수신할 수 있도록 SimX 경로를
구현했다. `core = zone`, `hardware thread = actor`이며, actor의 실제 상태는
Vortex의 core-local scratchpad SRAM에 둘 수 있다. 이 기능은 기본 비활성화된
SimX 프로토타입이다.

## 프로그램에서 일어나는 일

PGAS 주소는 논리적인 `{owner, offset}`이다. 예를 들어 actor 0이 actor 9의
7번째 값을 바꾸고 싶으면, actor 9에게 `{offset=7, delta=3}` 메시지를 보낸다.
actor 9의 수신 루프가 메시지를 꺼내고, 자기 SRAM의 해당 값을 수정한다.
읽기 요청은 값을 답장으로 돌려준다. 다른 core의 SRAM에 일반 `ld`로 바로
접근하는 기능이나 actor 사이의 하드웨어 메모리 보호는 추가하지 않았다.

```text
보내는 actor
    │ AMOSWAP.D: 목적지 주소 + payload 8B
    ▼
LSU → 주소 분류 → 해당 core의 메시지 장치
    │ ACCEPTED / BUSY / INVALID를 레지스터로 반환
    ▼
ZEN local/external TX FIFO
    ▼
기존 메시지 crossbar
    ▼
수신 core의 ZQM 대기 큐 → mailbox SRAM의 READY 슬롯
                                  │
                           수신 actor가 상태 LD
                                  │
                           AMOSWAP.D로 payload POP
                                  │ 값은 응답에 복사되고 슬롯 반환
                                  ▼
                           actor의 handler 실행
                                  ▼
                           실제 local SRAM 상태 수정
```

주소 범위 `0x71000000..0x713fffff`는 일반 global RAM이 아니라 장치 명령 창구다.
LSU가 이 주소를 분류해 전용 채널로 보낸다. 이 명령 자체가 L1/L2/DRAM에
메시지 큐를 만들지는 않는다. 상태 데이터의 local 접근은 별도의 기존
`__local_mem()` 경로를 사용한다.

송신은 `amoswap.d` 한 번에 64비트 payload를 값으로 전달한다. 주소에 목적지
zone, local PE, mailbox가 들어간다. 이 주소의 AMO는 플랫폼에 추가한 장치
명령이며 일반 RAM의 atomic swap과 의미가 다르다. 큐가 차면 BUSY를 즉시
반환한다. 호출자는 수신 작업도 진행하면서 나중에 재시도한다.

수신은 자기 actor의 상태 주소를 읽고, READY면 POP 명령으로 payload를
레지스터에 받는다. 장치는 응답 공간을 확보하고 값을 복사한 뒤 슬롯을
반환한다. 따라서 프로그램이 payload 주소를 받은 뒤 별도 release를 보내는
과정이 없다. actor 하나에 mailbox별 consumer가 하나라는 조건이다.

등록·상태·POP 주소에는 물리 actor 번호도 넣었다. 그래야 GPU 컴파일러가
lane마다 결과가 다른 접근임을 알 수 있다. 동일한 상수 주소에서 lane별로
다른 상태를 돌려주던 첫 버전은 uniform load 최적화와 충돌했고, 이 주소
설계로 수정했다. [원인과 수정](../../../docs/proposals/forza_mmio_uniformity.md).

메시지당 유효 payload는 8B다. 기존 fabric의 물리 프레임과 mailbox 슬롯은
여전히 64B다. 일반 Vortex 시작·식별·barrier CSR은 사용한다. FORZA/HClib의
56B 메시지 전체 ABI를 이식한 것은 아니다.

## 실제로 가까운 메모리는 무엇인가

여기서 local은 GPU의 global-backed per-thread stack을 의미하지 않는다.
core마다 있는 실제 16KiB scratchpad를 actor들이 소프트웨어로 나눠 쓴다.
8 actors/core 설정에서 actor당 int64 256개, 즉 2KiB를 보유할 수 있다.

```text
local_word_address = __local_mem() + 8 * (offset * actors_per_core + local_actor)
```

전체 두 core에서 live state 32KiB가 SRAM에 들어간다. 더 큰 배열은 actor별
256개까지만 resident로 두고 나머지는 명시적으로 global 메모리에 spill한다.
최종 검증을 위한 global snapshot/backing도 별도로 존재한다. mailbox SRAM은
이 state SRAM과 별개다. 현재 구성의 mailbox 본문은 8KiB/core이며, 이 수치는
송신·수신 FIFO와 기타 상태를 포함한 전체 하드웨어 면적이 아니다.

## 검증 결과와 비교 범위

실제 RV64 guest에서 16 actors의 반복 송수신, full64 payload의 0·상위 비트,
FIFO 순서, 큐 포화에 따른 BUSY, 슬롯 재사용, ACK 반환을 확인했다. 이어
다른 core의 actor에 GET/UPDATE를 보내고 실제 local SRAM 값을 답장받았다.
모든 actor의 최종 상태를 CPU 참조값과 비교해 통과했다.

scalar 장치는 cycle당 한 lane 명령을 처리한다. 별도 vector 옵션은 한 LSU
벡터의 최대 4 lane을 처리하며 추가 command/state/FIFO 포트를 가정한다.
두 모델에서 동일한 guest 바이너리가 통과했다. unit test는 두 옵션 각각
일반 실행과 ASAN/UBSAN을 통과했다. vector 옵션이 같은 면적이라는 주장은 없다.

상태 배치와 송수신 방법을 나눈 42개 scalar 실행, 같은 guest를 사용한 42개
vector 실행, owner를 분산한 28개 실행도 모두 정확한 결과를 냈다.
측정 workload는 signed irregular scatter의 유한한 한 단계다.

| 4,096 updates, 인기 키를 owner들에 분산 | SimX cycles |
| --- | ---: |
| 일반 GPU, global atomic | 50,594 |
| 일반 GPU, local SRAM + remote partials | 183,599 |
| CSR 메시지 + local SRAM | 746,689 |
| MMIO 메시지 + local SRAM, vector option | 863,831 |
| 소프트웨어 SPSC 메시지 + local SRAM | 1,320,772 |

MMIO/local은 해당 software owner 방식보다 약 1.53배 빨랐지만, 일반 GPU
global atomic보다 약 17.1배 느렸다. 같은 skew 입력의 MMIO/local은 owner가
한 곳에 몰린 경우 6,629,703 cycles에서 분산 후 863,831로 줄었다.
동시에 메시지 수는 3,841에서 3,851로 거의 같았다. 이 개선은 owner 배치와
메모리 배치 효과이며 CSR 제거 효과가 아니다.

32KiB state는 설정된 1MiB L2 안에 들어간다. 이 결과로 L2보다 큰 workload나
복잡한 여러 필드 handler의 승패를 판단할 수 없다. 이 모델의 단순 덧셈에서는
GPU atomic 한 번이 이미 같은 일을 끝낸다. 메시지 처리에는 enqueue, 전달,
상태 확인, POP, handler 실행이 추가된다. SRAM을 사용한다는 사실만으로
그 비용이 상쇄되지는 않았다. 실제 GPU 또는 RTL 성능·면적 측정은 아니다.

ACK는 수신 슬롯의 준비 완료이며 handler 완료가 아니다. 이번 테스트는
예상 수신 개수와 barrier로 종료한다. 일반 `finish`와 오류 복구까지 구현한
완성 runtime은 아니다. actor는 커널 실행 동안 상주하는 고정 thread다.
block 재배치나 물리 thread보다 많은 가상 actor의 scheduling은 검증하지 않았다.

## 다른 인터페이스 선택지

아래 세 가지는 이 실험에서 새로 구현·검증한 기능이 아니라 후속 설계 선택지다.

| 선택지 | 필요한 변화와 목적 |
| --- | --- |
| 전용 `msg.send` / `msg.recv` 명령 | 레지스터와 메시지 장치를 직접 연결해 LSU 명령 창구 비용을 줄인다. decode·issue·writeback과 SIMT 처리를 구현해야 한다. |
| descriptor/batch enqueue | 여러 메시지를 모아 한 번에 제출하고 장치가 읽어 간다. descriptor 접근 비용을 여러 메시지에 나누지만, 작은 메시지 하나만 보내면 그 비용이 추가될 수 있다. |
| sender/core coalescing | 합칠 수 있는 연산의 같은 owner·key 업데이트를 먼저 결합한다. 인터페이스 횟수와 실제 메시지 수를 함께 줄인다. 순서가 중요한 handler에는 그대로 적용할 수 없다. |

이번 결과는 인터페이스 처리 폭을 늘리는 것만으로는 큰 개선이 없고, owner
분산이 더 중요했음을 보여준다. 추가 연구에서는 전송할 메시지 수와
메시지 하나가 대체하는 작업량을 명시적으로 바꾸는 비교가 필요하다.

## 위치와 재현

- [공유 MMIO ABI](../../../sw/common/forza_mmio_abi.h)
- [송수신 wrapper](../forza_pgas_explore/transport.h)
- [메시지 장치](../../../sim/simx/forza/forza_zone.cpp)
- [실제 GET/UPDATE guest](kernel.cpp)
- [컴파일된 guest와 로그 검증](../../../docs/proposals/forza_mmio_guest_verification.json)
- [처리 폭·owner 분산 최종 비교](../../../docs/proposals/forza_pgas_width_mapping.md)
- [최초 scalar 비교 및 재현 자료](../../../docs/proposals/forza_pgas_results.md)

현재 만들어 둔 vector build의 smoke를 실행하는 명령:

```sh
cd /export/nethomes/spark3100/vortex-forza-hwqueue/build64-forza-pgas-vector/tests/regression/forza_mmio
env VORTEX_DRIVER=simx \
  LD_LIBRARY_PATH=/export/nethomes/spark3100/vortex-forza-hwqueue/build64-forza-pgas-vector/sw/runtime \
  ./forza_mmio_smoke_mmio kernel-mmio.vxbin 1000
```

새 build는 RV64와 `VX_CFG_EXT_FORZA_ENABLE`, `VX_CFG_EXT_A_ENABLE`,
`VX_CFG_FORZA_MMIO_ENABLE`을 함께 켜야 한다. vector 비교에는 추가로
`VX_CFG_FORZA_MMIO_VECTOR_ENABLE`을 켠다. 이번 검증은 2 cores, 2 warps/core,
4 threads/warp, D-cache 비활성화, L2 활성화 설정이다. app과 simulator의
CONFIGS가 같아야 한다. 전체 행렬의 정확한 명령·해시·로그는 각 결과 JSON과
각 build의 `pgas-manifest.json`에 기록되어 있다.
