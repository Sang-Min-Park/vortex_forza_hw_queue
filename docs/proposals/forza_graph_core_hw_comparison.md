# Core combining 뒤에 FORZA hardware queue를 연결한 비교

이번 변경은 Mitul-derived `agg_comb`의 **core 내부 합산은 유지하고, 합산 결과를
다른 owner에게 전달하는 경로를 바꾸는 실험**이다. 두 mode `core_hw`와
`core_hw_fixed`는 같은 DATA 경로를 쓰고 완료를 확인하는 방법을 달리한다.
실제로 연결해 실행한 결과, 두 mode 모두 열 graph에서 기존 SW `agg_comb`보다
느렸다. DONE 없는 fixed mode도 readback/flush 포함 cycles가 **1.17–2.00배**였다.
Global partial table을 없애는 것만으로 성능이 자동 개선되지는 않았다.

## 이전 두 방식과 무엇이 다른가

`agg_comb`는 core의 모든 thread가 scratchpad의 하나의 table에 기여값을 더한다.
그 뒤 core별 partial table을 global memory로 flush하고 tree barrier를 통과한다.
각 output owner는 여러 core의 partial을 global load로 읽어 최종값을 더한다.

`core_hw`도 같은 scratchpad zeroing과 shared atomic fold를 수행한다. 그 뒤 global
partial table을 만들지 않고, scratchpad의 owner vector를 직접 CSR payload로 만들어
FORZA queue에 보낸다. 수신 owner의 handler가 그 값을 더한다.

```text
공통: source edges → core scratchpad에서 합산

agg_comb: scratchpad → global partial table → tree barrier → owner가 global load
core_hw : scratchpad → CSR / ZEN / ZQM → owner handler → runtime DONE
```

따라서 hardware queue가 combining stage 자체를 빠르게 해주는 변경은 아니다.
Global partial의 flush·읽기를 message 전송·수신으로 바꾼다. `agg_comb`의 tree
barrier와 `core_hw`의 Selector/DONE 종료도 다르므로 같은 종료 protocol의 순수한
transport 교체 실험이라고 표현하지 않는다.

이전 `hw_agg`는 **sender별** global vector에 먼저 합산했다. `core_hw`는 core의
여러 sender가 **한 scratchpad table**에 합산한다. 두 mode는 저장 위치·합산 범위와
전달 단위 수가 다르다.

## 전달하는 데이터의 수

Core 두 개, owner 열여섯 개에서 core마다 owner vector 열여섯 개를 보낸다.
0으로만 채워진 vector도 포함하는 dense 방식이므로 총 vector 수는 **32개**다.
Local actor들에게 owner를 나누어 보내게 하며 receiver progress도 함께 진행한다.

메시지 payload는 56B이며 `begin32/source_core16/count16` 8B header와 int64 값
최대 여섯 개가 들어간다. Source core와 chunk의 bitmap으로 중복 수신도 검사한다.
64 vertices에서는 owner당 값이 네 개라 vector당 한 chunk, DATA chunk는 32개다.
256 vertices에서는 owner당 값이 열여섯 개라 세 chunk씩, DATA chunk는 96개다.
`core_hw`는 별도로 actor runtime의 256개 DONE과 outstanding ACK 0을 확인한다.
`core_hw_fixed`는 각 owner가 받을 chunk 수 `cores × ceil(values_per_owner/6)`를
미리 안다. 자신의 송신과 모든 unique chunk의 수신, outstanding ACK 0을 확인하고
warp vote와 최종 global barrier를 통과한다. 이 mode는 DONE message를 보내지 않는다.
DATA vector 수와 전체 message 수를 구별한다.

| Mode | DATA 전달 | 완료 확인 |
| --- | --- | --- |
| `core_hw` | core scratchpad → CSR queue → owner handler | Selector의 256 DONE과 ACK drain |
| `core_hw_fixed` | 같은 core vector와 같은 chunk 형식 | 알려진 chunk 수·중복 bitmap·ACK drain·global barrier |

Fixed mode는 dense vector 개수를 아는 one-shot collective 전용이다. 동적으로
메시지가 추가되는 일반 actor의 종료나 `Selector.is_done()`을 구현한 것으로
표현하지 않는다. Zero vector를 보내지 않도록 바꾸면 현재의 정해진 수신 개수
계약도 함께 바꿔야 한다.

## 비교 조건

모든 mode는 같은 weighted integer CSR graph scatter를 계산한다.

```text
y[destination] += edge_weight(source, destination) * x[source]
```

같은 graph hash·vertices·edges·seed·16 lane actors를 짝짓고 모든 output vertex를
CPU reference와 정확히 비교한다. 다음의 변경 없는 driver와 설정을 쓴다.

```text
build64-graph-gpu
-DVX_CFG_EXT_FORZA_ENABLE -DVX_CFG_EXT_A_ENABLE
-DVX_CFG_DCACHE_DISABLE -DVX_CFG_L2_ENABLE
-DVX_CFG_NUM_CORES=2 -DVX_CFG_NUM_WARPS=2 -DVX_CFG_NUM_THREADS=4
```

`agg_comb`, sender-level `hw_agg`, per-edge HW actor, GPU vertex의 기존 official
L1-off 결과를 reference로 사용한다. L1-on 결과나 이전 A-disabled 측정은 섞지 않는다.
기존 source·binary·측정 파일은 수정하지 않는다. 새 구현은 Mitul 원본 histogram의
graph adaptation에 hardware 전달을 추가한 것이며 원본 binary를 실행한 것은 아니다.

Primary metric은 host output readback/cache flush 뒤 core 0에서 조회한
`device_cycles_post_readback_including_flush`다. Case마다 새 process/device를 사용한다.
`phase_span_cycles`는 software phase의 별도 지표이며 plain output store의 최종 DRAM
기록 시각을 보장하지 않는다. Host wall time은 GPU speedup에 사용하지 않는다.
Stage breakdown이 worker별 최대값이면 서로 더하지 않는다.
새 두 mode의 `publish_sync`에는 송신 중 진행한 receiver handler와 fold 뒤의
block 동기화도 들어간다. `reduce_cycles = 0`은 reduction이 없다는 뜻이 아니라
별도 최종 reduce pass 없이 그 진행 중 처리했다는 뜻이다.

Scratchpad zeroing은 phase에 포함하고 aggregate 입력·output 초기화는 one-shot
launch 전 조건을 따른다. 이 실험이 반복 superstep reset이나 큰 graph의 steady-state
성능을 검증하는 것은 아니다. Global/local AMO, CSR·SRAM·fabric은 같은 SimX 모델을
사용하며 실제 GPU·FPGA throughput에 대한 calibration은 없다.
Global partial table을 없앴어도 input·output·runtime state와 mailbox SRAM 등 다른
저장 공간은 남아 있으므로 global-memory 사용 전체가 0이 되었다고 설명하지 않는다.

## 결과

새 공식 실행 20개와 대표 반복 2개가 모두 CPU reference·완료 검증을 통과했다.
각 owner의 core/chunk 중복 검사, 32개 vector와 DATA 32/96개, outstanding 0도
확인했다. 고정된 reference 40개를 더해 **62개 log → 60개 graph/mode 측정값**이다.
두 대표 반복은 cycle과 memory/instruction counter가 모두 첫 실행과 같았다.

64 vertices / 1,024 edges, skew, seed 17의 결과를 먼저 보면 다음과 같다.
Primary 값은 모두 **host readback/cache flush 뒤 device cycles**다.

| Core combining 뒤의 전달 방식 | Device cycles | SW 대비 소요 시간 |
| --- | ---: | ---: |
| SW `agg_comb`: global partial + tree + owner load | 87,771 | 1.00× |
| `core_hw`: hardware queue + 256 DONE | 474,770 | 5.41× |
| `core_hw_fixed`: hardware queue + known chunk 수 | 104,725 | 1.19× |

이전 sender-level `hw_agg`는 같은 graph에서 600,054 cycles였다. 새 core-level
결합과 fixed completion은 그보다 개선되었지만, SW core combining보다 빠르지는
않았다. 같은 설정의 direct GPU atomic은 44,451 cycles였다.

열 graph 전체에서 얻은 비율은 다음과 같다. 각 graph의 같은 hash와 seed 안에서
계산한 최소–최대이며, 다른 크기의 cycle을 서로 나눈 값이 아니다.

| 새 mode | SW `agg_comb` 대비 소요 시간 | SW보다 빠른 case | GPU 대비 소요 시간 |
| --- | ---: | ---: | ---: |
| `core_hw` | 2.25–31.64× | 0 / 10 | 4.20–112.04× |
| `core_hw_fixed` | 1.17–2.00× | 0 / 10 | 2.19–7.09× |

Fixed mode는 normal mode보다 모든 case에서 1.91–15.80배 빨랐다. Generic DONE
진행을 줄이는 선택이 현재 workload에서 큰 차이를 만들지만, fixed mode는 수신할
partial 개수를 아는 one-shot 계산에만 적용한다. 이 차이를 모든 actor workload에서
제거 가능한 비용으로 보지 않는다.

Fixed mode는 기존 per-edge HW보다 11.11–157.11배, sender-level `hw_agg`보다
5.48–15.44배 빨랐다. 즉 이전 HW actor 경로를 개선한 것과 이미 최적화된 SW core
combining을 넘어선 것은 서로 다른 결과다.

## 결과의 의미와 범위

두 새 mode는 SW와 같은 scratchpad zero/shared-AMO combining 알고리즘을 유지한다.
그렇다고 fold 구간의 binary·cycle까지 완전히 같은 것은 아니다. 위 대표 case의
`max_fold_cycles`는 SW 76,006, normal 79,604, fixed 79,696이었다. Runtime 통합,
추가 수신 metadata와 compiler의 register/control-flow 배치도 달라질 수 있으므로
전체 시간 차이를 모두 transport나 DONE 하나의 독립 비용으로 단정하지 않는다.
Stage별 최대값을 더해 차이를 분해하는 것도 올바르지 않다.

Global partial buffer는 V=64에서 1,024B, V=256에서 4,096B를 없앴다.
Core당 scratchpad는 SW와 같은 512B와 2,048B다. 새 mode의 tree flags는 0B다.
대신 CSR 전송·수신·metadata·handler와 해당 완료 절차를 수행한다. 이는 전체
메모리 사용·실제 network byte·chip 면적이 그만큼 줄었다는 측정은 아니다.

현재 작은 graph와 동일 SimX 모델에서는 global partial을 직접 flush/reduce하는
SW 방식이 더 빨랐다. Physical GPU/FPGA 성능, 큰 DRAM working set, 일반 동적
actor 종료나 반복 superstep까지의 결론으로 확대하지 않는다.

## 결과 파일과 재현

- [CSV: 열 graph와 여섯 mode의 전체 결과](forza_graph_core_hw_measurements.csv)
- [JSON: 원본 62개 log·반복·completion·footprint](forza_graph_core_hw_measurements.json)
- [Source/config/model provenance](forza_graph_core_hw_manifest.json)
- [새 kernel](../../tests/regression/forza_graph_core_hw/kernel.cpp)
- [분석 스크립트](../../tests/regression/forza_graph_core_hw/analyze.py)

새 app의 `core-hw-run-manifest.json`은 official 20개 실행의 명령과 결과, binary/
library fingerprint를 보존한다. 측정 전후 artifact가 바뀌지 않았고 이전 reference와
같은 driver를 사용했다. Source manifest는 분석기의 read-only reference parser
의존성도 기록한다. 기존 보고서와 측정 파일은 변경하지 않았다.

이미 build된 같은 파일로 대표 case를 재실행하려면 다음과 같다.

```bash
cd /export/nethomes/spark3100/vortex-forza-hwqueue/build64-graph-gpu/tests/regression/forza_graph_core_hw
export VORTEX_DRIVER=simx
export LD_LIBRARY_PATH=/export/nethomes/spark3100/vortex-forza-hwqueue/build64-graph-gpu/sw/runtime
./forza_graph_core_hw -k kernel-core_hw.vxbin -v64 -e1024 -gskew -s17
./forza_graph_core_hw_fixed -k kernel-core_hw_fixed.vxbin -v64 -e1024 -gskew -s17
```

전체 export를 다시 만드는 명령은 다음과 같다. 분석기는 같은 graph hash의 여섯
mode가 모두 있는지 검사하고, 잘못된 log나 누락 case를 실패 status로 보고한다.

```bash
cd /export/nethomes/spark3100/vortex-forza-hwqueue
python3 tests/regression/forza_graph_core_hw/analyze.py
```
