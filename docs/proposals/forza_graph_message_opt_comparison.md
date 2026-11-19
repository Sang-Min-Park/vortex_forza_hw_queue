# Message passing 병목과 pipeline 최적화 비교

이 실험은 기존 측정에서 느렸던 부분을 실제 코드에서 확인하고, message 경로와
앞단 graph 처리의 비용을 줄이는 새 pipeline을 비교한다. 기존 결과·source·driver는
그대로 보존한다. 개선 여부는 같은 graph와 같은 SimX driver로 다시 측정한다.

**실제 개선은 있었다.** 대표적인 64/1,024 skew에서 기존 fixed message 경로의
104,725 cycles를 `msg_warp`의 47,081 cycles로 줄였다. 2.22배 개선이다.
다만 같은 최적화를 준 SW는 41,889, GPU는 22,779 cycles로 여전히 빨랐다.
아래 값은 모두 host 결과 readback/cache flush까지 포함한 device cycles다.

## 어디를 먼저 봐야 하는가

현재 실행은 다음 단계를 지난다.

```text
graph load + core combining
    → packet 구성 → CSR payload 쓰기 → ZEN/ZQM 전달
    → CSR polling + SRAM payload 읽기 → handler → 완료 확인
```

연결망만 빨라져도 앞단의 combining 비용은 남는다. 기존 64/1,024 skew, seed 17에서
`core_hw_fixed`의 `max_fold_cycles`는 79,696이었다. 반면 GPU direct atomic의
readback/flush 포함 전체 device cycles는 44,451이었다. 이 dense-vector 알고리즘은
각 core의 fold를 마쳐야 partial을 보내고 모든 owner가 두 core의 결과를 받아야 한다.
따라서 **fold를 그대로 두면 communication을 무료로 만들어도 이 GPU 실행을
넘을 수 없다.** 이는 단계별 최대값을 더한 추정이 아니라 필수 prefix 하나의 하한이다.

## 코드에서 확인한 비용과 아직 분리하지 않은 비용

기존 fold loop의 disassembly에는 static 25 instructions와 6 loads/edge가 있고,
GPU 쪽 대응 loop에는 12 instructions와 2 loads가 있었다. Inline AMO assembly의
memory clobber 때문에 compiler가 immutable argument와 row end를 다시 읽는 부분이
있었다. 새 pipeline은 scalar·pointer와 row end를 loop 밖으로 옮긴다. Static 개수를
곧바로 cycle 비용으로 환산하지 않는다. 이 모델의 DIV는 2-cycle operation이므로
일반적인 “나눗셈이 수십 cycle이라 병목”이라는 설명도 적용하지 않는다.

새로 저장한 disassembly에서 확인한 hot-loop 개수는 다음과 같다. 여전히 message
fold의 owner/index 주소 계산이 direct GPU보다 많다. 이 표는 static 코드 관찰이며
실행 횟수·stall·memory latency까지 반영한 profile 비율은 아니다.

| Fold loop | Static instructions | Loads |
| --- | ---: | ---: |
| 기존 `core_hw_fixed` | 25 | 6 |
| 새 `msg_local` | 21 | 2 |
| 새 `msg_private` | 22 | 2 |
| 새 `msg_warp`, `sw_warp` | 23 | 2 |
| 새 `gpu_warp` | 14 | 2 |

현재 runtime의 `progress()`는 `message_available()`을 확인하고, 호출한
`message_receive()` 안에서 같은 상태를 한 번 더 확인한다. 수신에는 SRAM payload
읽기, temporary `Payload`, typed packet과 handler 인자가 있고 송신에는 typed packet,
`Payload`, 일곱 번의 `zeneqd` 쓰기가 있다. 다만 C++의 `memcpy`나 zero initialization이
항상 실제 memory copy로 남는 것은 아니다. Compiler가 없애거나 register로 처리할
수 있으므로 source의 copy 층 수를 그대로 dynamic traffic으로 세지 않는다.

근거: [active Vortex selector](/export/nethomes/spark3100/forza-runtime-vortex/inc/selector.vortex.h:245),
[receive 상태 재확인](/export/nethomes/spark3100/forza-runtime-vortex/inc/forza_message_lib.vortex.h:109),
[CSR 송신](/export/nethomes/spark3100/forza-runtime-vortex/inc/forza_message_lib.vortex.h:133).

## 새 비교가 바꾸는 것

Scalar와 row-end caching, own-core result의 직접 적용, 하나의 cooperative 완료 loop를
함께 적용한다. Same-core partial은 CSR mailbox를 왕복하지 않고 owner가 직접 읽는다.
두 core에서 전체 DATA chunk 수는 절반이지만, 빠진 것은 local delivery다. Cross-core
DATA 개수까지 절반으로 줄인 것으로 설명하지 않는다.

Warp-cooperative row 처리는 한 row의 edge들을 lane에 나누고 core/warp assignment를
interleave한다. 이 변경은 message transport 이전의 작업 배분도 바꾼다. 대응하는
SW combining과 GPU direct-atomic control에도 같은 최적화와 분배를 적용해, HW
경로에만 유리한 algorithm을 제공하지 않는다. SW control에도 own-core local 적용을
넣고 remote partial만 global flush·tree·pull로 처리한다.

개별 mode가 여러 변경을 함께 사용하므로 한 mode의 전체 speedup을 local bypass
하나나 memcpy 하나의 독립 효과로 단정하지 않는다. Private partial의 저장 위치와
reset/reduction 비용도 실제 구현과 측정값에 맞춰 별도로 기록한다.

| 새 mode | 적용한 pipeline |
| --- | --- |
| `msg_local` | scalar/row-end caching, own-core bypass, 하나의 fixed-count 송수신 loop |
| `msg_private` | 같은 message 경로와 caching; sender별 private LMEM row에 AMO 합산 후 row들을 reduce |
| `msg_warp` | shared core table, caching/bypass와 warp-cooperative·core-interleaved row 배분 |
| `sw_warp` | `msg_warp`와 같은 fold·배분·local bypass; remote partial만 global flush/tree/pull |
| `gpu_warp` | 같은 warp row 배분과 caching; global output에 직접 atomic add |

Private 방식도 plain load/add/store로 ordering을 가정하지 않고 acknowledged AMO를
유지한다. Core당 row 수는 1에서 8로 늘며, V=256에서 scratchpad를 2KiB 대신
16KiB 사용한다. 초기화와 row reduction 비용도 측정에 포함된다. 더 큰 설정이
scratchpad capacity를 넘으면 host가 거부한다.
논리적인 sender별 vector는 물리적으로 `[output_word][sender]`에 interleave해
합산 단계의 hot word를 분산한다. 그러나 최종 합산값은 sender column 0에 남긴다.
네 bank 설정에서 그 값의 gather·local read가 bank 0에 몰릴 수 있어 bank conflict를
모두 없앤 구현이나 최상의 private-reduction 알고리즘으로 표현하지 않는다.

이번 다섯 mode는 기존 runtime의 receive memcpy 층을 바꾸는 실험이 아니다.
In-place typed receive를 추가한다면 nonblocking handler, view를 보관하지 않는
수명 규칙과 모든 payload load 뒤 slot release를 지켜야 한다. 이는 별도 검증이
필요한 후보이며 이번 결과의 개선 요인으로 세지 않는다.

## 측정 조건과 해석

기존의 열 graph, weighted int64 CSR scatter와 CPU reference를 사용한다. 같은 hash,
vertices·edges·seed를 짝짓는다. Driver는 기존 A/FORZA-enabled 2-core, 2-warp,
4-lane, private L1 off + shared L2다. Model·runtime 원본을 수정하지 않는다.

Warp 배분은 source edge를 담당하는 core/lane을 바꾸므로 `cross_core_edges`도
바뀔 수 있다. 이는 graph가 달라졌다는 뜻이 아니다. 분석기는 hash·degree·CPU
결과는 전체 mode에서 맞추고, cross-core count는 같은 work-mapping group 안에서만
같은지 확인한다. 해당 mapping과 count를 CSV/JSON에 각각 보존한다.

Primary 값은 host 결과 readback/cache flush 뒤의 device cycles다. Software phase와
stage별 worker 최대값도 보존하지만, 서로 다른 worker의 최대를 더하지 않는다.
알려진 수신 개수에 의한 one-shot 완료는 일반 동적 actor 종료와 구별한다.
실제 GPU/FPGA latency에 맞춘 calibration이나 대형 graph 전체에 대한 결론은 아니다.

## 결과

새 official 50개 실행과 다섯 대표 반복이 모두 CPU reference·worker별 edge·vector·
chunk·완료 검증을 통과했다. 기존 reference 30개를 더해 **85개 log → 열 graph ×
여덟 mode의 80개 측정값**으로 정리했다. 다섯 반복은 cycle과 counter가 처음과 같았다.

### 대표 graph에서 달라진 부분

64 vertices / 1,024 edges, skew, seed 17의 결과다.

| 실행 경로 | Device cycles |
| --- | ---: |
| 기존 `core_hw_fixed` | 104,725 |
| `msg_local`: caching + local bypass + 단일 완료 loop | 83,983 |
| `msg_private`: private LMEM partial 추가 | 96,886 |
| `msg_warp`: warp row 배분 추가 | 47,081 |
| 기존 SW `agg_comb` | 87,771 |
| 같은 warp/bypass를 준 `sw_warp` | 41,889 |
| 기존 GPU vertex | 44,451 |
| 같은 warp 배분의 `gpu_warp` | 22,779 |

Warp 적용을 HW 경로에만 주면 기존 SW나 기존 GPU와의 격차가 과장된다. 같은
배분을 적용한 control도 개선되었다. 이 case에서는 HW message가 기존 SW보다
빨라졌지만, 개선된 SW보다 12.4%, 개선된 GPU보다 2.07배 오래 걸렸다.

해당 case의 fold 최대 구간은 기존 79,696에서 `msg_local` 67,673,
`msg_warp` 29,946 cycles로 줄었다. Matched `sw_warp`는 29,870,
`gpu_warp`는 21,354 cycles였다. 이는 kernel 앞단에도 개선이 필요했다는 관측과
맞는다. `msg_warp`의 fold 하나만도 GPU 전체 22,779 cycles보다 길어, 이 새 fold를
그대로 둔 채 message 비용만 없애는 것으로는 이 GPU 결과를 넘을 수 없다.

### 열 case의 전체 경향

- `msg_local`은 기존 fixed 경로보다 열 case 모두에서 **1.24–1.42배** 빨랐다.
- `msg_warp`는 기존보다 **1.18–2.34배** 빨랐다. Source-degree skew가 있는 네
  case에서 세 message 구현 중 가장 빨랐고, regular·hub 여섯 case에서는 `msg_local`이 빨랐다.
- `msg_private`는 `msg_local`보다 모든 case에서 느렸고 최선인 case가 없었다.
  Private hot-word 분산의 이득만으로 증가한 초기화·row reduction·gather 비용을 상쇄하지 못했다.
- 같은 warp mapping의 `msg_warp`와 `sw_warp`를 비교하면 message 쪽이 **1.12–1.33배**
  오래 걸렸다. 두 경로는 같은 fold와 local bypass를 사용하지만 remote delivery와
  completion이 다르며 compiler 배치까지 동일한 binary 실험은 아니다.

Graph마다 측정된 message 세 가지 중 최선과 GPU 두 가지 중 최선을 선택해 비교해도
message는 **1.78–5.00배** 오래 걸렸다. 최선 SW 두 가지와 비교하면 **1.11–1.41배**다.
이 선택은 결과를 해석하기 위한 사후 비교이며 graph를 보고 자동으로 mode를 선택하는
정책을 구현한 것은 아니다. Warp mapping은 source-skew에 유리했지만 balanced-source
case의 GPU에서는 기존 vertex mapping이 더 빨랐다. 전체 graph에 warp가 항상 낫다고
표현하지 않는다. 각 mode의 모든 값과 mapping을 CSV/JSON에 보존했다.

## 남은 병목과 다음 후보

Private mode는 V=64에서 4KiB/core, V=256에서 16KiB/core를 사용한다. Shared 방식의
512B와 2KiB보다 8배 크다. 대표 case의 initialization 최대값은 3,164 cycles로
`msg_local`의 922보다 컸다. `max_reduce_cycles`는 63,342와 53,666이었지만 이 값은
private row 합산뿐 아니라 다른 lane의 fold를 기다리는 CTA barrier와 local output
적용도 포함한다. 이 차이를 순수 private-reduction 비용으로 부르거나 단계 최대를
더하지 않는다.

현재는 fold hot loop의 owner/index 계산, local bank 처리와 이후 메시지 처리 비용이
남는다. 다음으로 검토할 수 있는 source 변경은 scratchpad를 `partial[destination]`
형식으로 두어 owner/index 변환을 edge loop 밖의 vector gather로 옮기는 것이다.
현재 owner-major table은 V=64의 L=4에서 여러 owner의 첫 word가 같은 local bank로
몰릴 수도 있다. Contiguous layout은 **아직 측정하지 않은 후보**이며 speedup을
약속하는 결과가 아니다.

Message 경로에서는 확인된 중복 status polling과 typed payload의 temporary/lifetime
처리를 검토할 수 있다. Copy layers가 어느 정도 실제 instruction·spill로 남는지는
추가 profile이 필요하다. 이번 실험은 원본 runtime·hardware timing을 수정하지 않았고
zero-copy receive를 구현했다고 주장하지 않는다. 여기서 얻은 개선을 실제 GPU/FPGA의
성능 배율이나 generic dynamic actor의 완료 성능으로 확대하지 않는다.

## 결과 파일과 재현

- [CSV: 열 graph × 여덟 mode](forza_graph_message_opt_measurements.csv)
- [JSON: 원본 85개 log·mapping·반복·stage·footprint](forza_graph_message_opt_measurements.json)
- [Source/config/model/compiler provenance](forza_graph_message_opt_manifest.json)
- [새 구현과 정확한 stage 정의](../../tests/regression/forza_graph_message_opt/README.md)
- [분석 스크립트](../../tests/regression/forza_graph_message_opt/analyze.py)
- [저장한 compiled-fold 증거](/export/nethomes/spark3100/vortex-forza-hwqueue/build64-graph-gpu/tests/regression/forza_graph_message_opt/compiled-fold-proof.txt)

새 app의 `message-opt-run-manifest.json`에 official 실행의 명령·exit status와
artifact fingerprint를 기록했다. Driver·runtime·기존 source와 reference는 그대로
유지했고 측정 중 파일 hash도 바뀌지 않았다. Case마다 새 process/device를 사용했다.
Host wall time은 speedup 계산에 사용하지 않는다.

이미 build된 같은 파일로 대표 case를 재실행하려면 다음과 같다.

```bash
cd /export/nethomes/spark3100/vortex-forza-hwqueue/build64-graph-gpu/tests/regression/forza_graph_message_opt
export VORTEX_DRIVER=simx
export LD_LIBRARY_PATH=/export/nethomes/spark3100/vortex-forza-hwqueue/build64-graph-gpu/sw/runtime
./forza_graph_msg_local -k kernel-msg_local.vxbin -v64 -e1024 -gskew -s17
./forza_graph_msg_private -k kernel-msg_private.vxbin -v64 -e1024 -gskew -s17
./forza_graph_msg_warp -k kernel-msg_warp.vxbin -v64 -e1024 -gskew -s17
./forza_graph_sw_warp -k kernel-sw_warp.vxbin -v64 -e1024 -gskew -s17
./forza_graph_gpu_warp -k kernel-gpu_warp.vxbin -v64 -e1024 -gskew -s17
```

전체 export를 다시 만드는 명령은 다음과 같다. 분석기는 같은 input hash와
work-mapping group을 검사하고 불완전한 mode matrix나 잘못된 log를 실패 status로 보고한다.

```bash
cd /export/nethomes/spark3100/vortex-forza-hwqueue
python3 tests/regression/forza_graph_message_opt/analyze.py
```
