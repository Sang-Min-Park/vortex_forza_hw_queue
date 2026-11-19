# Mitul의 aggregation 방식과 FORZA queue 비교

**세 Mitul-derived 방식 모두 이번 열 graph에서 one-message SPSC와 per-edge HW
actor보다 빨랐다.** Core combining인 `agg_comb`가 세 방식 중 가장 빨랐으며,
SPSC 대비 22.10–495.37배, per-edge HW 대비 22.17–242.23배 빨랐다.
같은 설정의 direct GPU atomic kernel은 `agg_comb`보다도 1.87–3.54배 빨랐다.
모두 host 결과 readback/cache flush까지 포함한 SimX device cycle 비교다.

이번 실험은 Mitul의 세 aggregation 방식을 같은 weighted graph 계산에 적용해,
기존 HW actor와 direct GPU 실행에 비교한다. 같은 sender aggregation을 HW queue로
전달하는 `hw_agg` control과, 같은 새 설정으로 재실행한 one-message SPSC도 포함한다.
이전 software-mailbox 측정은
**메시지마다 SPSC slot 하나를 사용하는 별도 backend**였다. 아래의 aggregation
코드를 실행해 얻은 결과가 아니므로, 이전 비율을 이 비교의 결과로 사용하지 않는다.
기존 측정 파일은 그대로 보존한다.

## 먼저 무엇을 합치는가

한 sender가 같은 owner의 output에 여러 기여값을 보낸다고 하자.
기본 actor 경로는 edge마다 packet 하나를 만들고 owner가 하나씩 더한다.
Sender aggregation은 owner별 vector에 먼저 더한 뒤 합산된 vector를 전달한다.
따라서 입력 edge 수와 최종 전달 단위 수가 달라진다.

```text
기본 actor:     contribution 3 → owner
               contribution 5 → owner
               contribution 2 → owner

합산한 방식:   sender에서 3+5+2=10
               합산 결과 10 → owner
```

이는 여러 packet을 하나의 memory transaction으로 옮기는 단순한 batching보다
강한 변환이다. 덧셈의 결합 법칙을 사용해 반복된 갱신 자체를 줄인다. 이번 integer
graph scatter는 같은 결과를 얻을 수 있지만 임의의 actor handler를 항상 이렇게
합칠 수 있는 것은 아니다.

## 원본과 세 방식의 차이

원본은 `/export/nethomes/spark3100/vortex-actor`의 commit
`7ef9d3acf9d34c0df1c8a257fe3bf67759081472`에 있는 histogram 구현이다.
Git의 author는 `tandonmitul27`이며 세 variant가 해당 commit에 들어왔다.
이번 프로그램은 그 알고리즘을 graph scatter로 옮긴 **Mitul-derived adaptation**이다.
원본 binary나 generic message-coalescing runtime을 그대로 실행한 것은 아니다.

| 새 mode | 원본 | 합산 공간과 진행 |
| --- | --- | --- |
| `agg_amo` | `actor_histo_agg_amo` | sender·owner별 global vector에 먼저 합산하고, 완료를 publish한 뒤 owner가 한 번씩 drain |
| `agg_lmem` | `actor_histo_agg_amo_lmem` | 같은 core의 sender→owner vector는 scratchpad, 다른 core는 global vector로 구분 |
| `agg_comb` | `actor_histo_agg_comb` | core의 모든 thread가 하나의 scratchpad table로 합산; core별 결과를 global로 flush하고 owner가 core별 값을 reduce |

추가 `hw_agg` control은 `agg_amo`와 같은 sender-private global AMO fold를 수행한다.
그 뒤 nonempty owner vector를 CSR 메시지로 나누어 보낸다. 56B payload 중 8B는
`begin/count` header이고 최대 여섯 개의 int64 값이 들어간다. Owner당 값이 네 개인
64-vertex case는 vector당 한 chunk, 열여섯 개인 256-vertex case는 세 chunk가 필요하다.
송신 중에도 receiver의 progress를 진행하며, handler가 합산하고 256개의 DONE
protocol로 종료한다. SW `agg_amo`는 global vector를 직접 읽고 tree barrier로
완료를 확인하므로 이 쌍도 termination까지 같은 pure-transport ablation은 아니다.

`agg_comb`는 원본 README도 “No messages”로 구분한다. 모든 최적화를 하나의
mailbox 기능으로 묶어 부르지 않는다. `agg_amo`의 원본 comment 일부는 no-atomics라고
쓰여 있지만 실제 kernel은 single-writer vector에도 `__atomic_fetch_add`를 사용한다.
원본은 이를 load/add/store 대신 한 instruction을 사용하는 선택으로 설명한다.
비교는 comment의 요약보다 실제 실행 코드를 따른다.

원본: [README](/export/nethomes/spark3100/vortex-actor/tests/actor/README.md),
[sender aggregation](/export/nethomes/spark3100/vortex-actor/tests/actor/actor_histo_agg_amo/kernel.cpp),
[two-level aggregation](/export/nethomes/spark3100/vortex-actor/tests/actor/actor_histo_agg_amo_lmem/kernel.cpp),
[core combining](/export/nethomes/spark3100/vortex-actor/tests/actor/actor_histo_agg_comb/kernel.cpp).

## 동일하게 맞출 계산과 실행 조건

계산은 앞선 graph 실험과 같은 한 번의 weighted integer scatter다.

```text
y[destination] += weight(source, destination) * x[source]
```

같은 CSR graph·weight·x·seed·graph hash를 사용하고 모든 output vertex를 CPU
reference와 정확히 비교한다. Regular·skew·hub의 기존 열 case를 사용한다.
GPU는 연속 `y[destination]` 배열을, actor·aggregation mode는 owner별 output
slice를 사용한다. 논리적인 결과는 같고 GPU에 actor용 물리 배열 배치를 강제하지 않는다.
Contribution과 누적값은 64-bit 정수다. Input edge 수는 그대로지만 aggregation
vector, publish와 flush의 수는 별도 계측하며 edge 수와 message 수를 같다고 세지 않는다.
`agg_amo`와 `hw_agg`는 합산 후 nonzero인 vector만 published로 센다. `agg_lmem`는
빈 vector를 포함한 N²개의 dense sender→owner vector를, `agg_comb`는 core×N개의
owner slice를 센다. 따라서 세 counter를 같은 의미의 network packet 수로 나누지 않는다.

Matching build는 `build64-graph-gpu`이며 다음 설정의 **동일한 기존 DSO**를 쓴다.

```text
-DVX_CFG_EXT_FORZA_ENABLE -DVX_CFG_EXT_A_ENABLE
-DVX_CFG_DCACHE_DISABLE -DVX_CFG_L2_ENABLE
-DVX_CFG_NUM_CORES=2 -DVX_CFG_NUM_WARPS=2 -DVX_CFG_NUM_THREADS=4
```

기존 official L1-off HW actor와 GPU vertex의 같은 graph 결과만 재사용한다.
One-message SPSC는 A-enabled 설정에서 열 case를 새로 실행한다. 이전 A-disabled
SW 측정이나 L1-on GPU의 값을 끼워 넣지 않는다. 전체 비교는 열 graph × 일곱 mode다.
기존 HW/GPU reference가 20개, 새 실행은 50개다.

## 원본에서 바뀐 부분

이번 graph port는 원본의 hash-generated histogram input을 앞선 실험의 weighted
CSR graph로 바꾼다. `agg_amo` 원본은 actor마다 block 하나와 thread 하나를 띄우지만,
이번 실행은 core마다 persistent block을 두고 lane thread 하나를 actor로 대응시킨다.
Launch mapping과 memory coalescing이 달라질 수 있으므로 원본 성능을 재현했다고
표현하지 않는다.

원본의 tree-barrier flag에 대한 plain volatile polling은 현재 private-L1 coherence
기능을 가정할 수 없다. 이번에는 L1을 끄고 shared L2를 사용하며, flag를
`amoswap.w`로 publish하고 fence로 그 응답을 확인한다. Core-combining의 global
flush와 scratchpad zeroing에는 `amoswap.d`를 사용해 완료 응답을 추적한다.
Queued memory model에서 plain store 뒤 fence만으로 downstream write 완료를
가정하지 않기 위한 port 변경이다. 이 추가 operation의 비용도 새 측정에 포함된다.

Global aggregate buffer는 원본처럼 host가 launch 전에 zero-initialize하는 one-shot
조건이다. Scratchpad 초기화는 kernel phase에 포함한다. 반복 superstep에서 buffer를
다시 0으로 만드는 steady-state 비용까지 자동으로 검증한 것은 아니다.

`global_vectors_bytes`는 할당한 aggregate buffer 크기다.
`global_active_vectors_bytes`는 알고리즘에서 사용할 수 있는 **eligible address 영역**이며,
실제로 값이 기록되었거나 network로 전송된 byte 수가 아니다. `agg_amo`에는 빈 CAP1
pair도 포함하고, `agg_lmem`에서만 사용하지 않는 same-core global pair를 제외한다.
Scratchpad 크기는 core당 값으로, 전체 device 합계와 구별한다. Flags는 128B를
모든 새 mode에 할당하지만 `hw_agg`의 tree flags는 사용하지 않는다.

## 측정 범위

`phase_span_cycles`는 초기 barrier 뒤부터 각 알고리즘의 drain/reduction과 종료까지의
software phase를 잰다. Mode마다 완료 protocol이 다르므로 이 값만으로 모든 plain
store가 DRAM에 기록된 시각을 뜻하지 않는다.

최종 비교에는 host output readback/cache flush 뒤 core 0의 `MCYCLE`, 즉
`device_cycles_post_readback_including_flush`도 함께 사용한다. 이 구간은 allocation과
host wall time을 뜻하지 않으며, device 모델이 진행한 전체 tick을 센다. Case마다
새 process/device를 실행하고 core cycle을 합산하지 않는다. Memory request와
instruction counter도 readback/flush 뒤 값이므로 phase 내부 traffic이라고 해석하지 않는다.

`max_local_init_cycles`, `max_fold_cycles`, `max_publish_sync_cycles`, `max_reduce_cycles`는
각 영역의 **worker별 최대값**이다. 서로 다른 worker의 최대일 수 있고 phase 경계도
worker마다 다르므로 이 네 값을 더해 전체 phase나 비용 비율을 만들지 않는다.
원본을 잇는 breakdown 정보로 CSV/JSON에 보존한다.

AMO의 global 경로는 shared LLC bank에서 요청을 처리하고 lane별 atomic을 합치지
않는다. Local/shared-memory AMO에는 별도의 bank occupancy와 같은 word 충돌 규칙이
있다. 따라서 global과 scratchpad가 모두 무한히 빠른 memory라고 가정하지 않는다.
이 모델을 physical GPU의 atomic throughput·SRAM port 수에 맞춘 calibration은 없다.
이전 HW queue의 CSR·SRAM·fabric 역시 실험적 timing 모델이라는 제한이 유지된다.

## 결과

공식 새 실행 50개와 대표 case의 반복 5개가 모두 통과했다. 기존에 고정한 HW/GPU
reference 20개를 더해 **75개 log → 열 graph × 일곱 mode의 70개 측정값**으로 묶었다.
모든 output vertex가 CPU reference와 일치했고 worker별 edge·vector·chunk·완료
조건을 검사했다. 같은 case의 다섯 새 mode를 반복했을 때 cycle·counter가 모두 같았다.
반복은 median으로 묶되 원본·횟수·일치 여부를 JSON에 보존했다.

### 한 graph로 먼저 보기

64 vertices / 1,024 edges, skew, seed 17의 결과다. 숫자가 작을수록 빠르다.
이 표는 phase snapshot이 아닌 **readback/flush 뒤 device cycles**다.

| 실행 방식 | Device cycles | 실제로 수행하는 통신·합산 |
| --- | ---: | --- |
| One-message SPSC | 5,235,933 | edge마다 global-memory queue packet |
| Per-edge HW actor | 2,549,534 | edge마다 FORZA queue message |
| `agg_amo` | 117,668 | sender별 global vector에 합산하고 직접 drain |
| `agg_lmem` | 155,850 | 같은 core vector는 scratchpad, 다른 core는 global |
| `agg_comb` | 87,771 | core별 scratchpad 합산 후 flush/reduce |
| `hw_agg` | 600,054 | sender aggregation 후 FORZA vector chunk 전송 |
| Direct GPU atomic | 44,451 | global output에 직접 atomic add |

이 graph에서 `agg_comb`는 SPSC보다 59.65배, per-edge HW보다 29.05배 빠르다.
같은 sender AMO fold를 쓰는 `agg_amo`도 `hw_agg`보다 5.10배 빠르다. GPU는
`agg_comb`보다 1.97배 빠르다. 이전의 단순 software-queue 대비 약 2배 개선만으로
최적화된 software aggregation보다 빠르다고 말할 수 없었던 이유가 여기 드러난다.

### 열 case의 전체 경향

열 graph를 각각 짝지어 계산한 최소–최대 비율은 다음과 같다. Graph 크기가 다른
cycle을 합치거나 raw log 중 유리한 것만 선택한 값이 아니다.
전체 열 graph × 일곱 mode의 실제 cycle은 [CSV](forza_graph_coalesced_measurements.csv)에 있다.

| 방식 | SPSC보다 빠른 배율 | Per-edge HW보다 빠른 배율 | 이 방식보다 GPU가 빠른 배율 |
| --- | ---: | ---: | ---: |
| `agg_amo` | 9.80–304.46× | 9.86–148.87× | 2.36–7.99× |
| `agg_lmem` | 13.97–272.39× | 12.87–133.19× | 3.41–5.60× |
| `agg_comb` | 22.10–495.37× | 22.17–242.23× | 1.87–3.54× |

`hw_agg`는 per-edge HW보다 모든 case에서 1.09–15.84배 빨랐다. HW queue도
software pre-aggregation의 효과를 얻는다. 그러나 같은 sender fold를 쓰는 SW
`agg_amo`가 `hw_agg`보다 5.08–14.61배 빨랐다. CSR 전송·본문 복사·handler와
256 DONE의 비용을 포함한 **현재 구현과 완료 protocol 전체의 결과**다. 이를 동일
termination의 순수 transport 성능이나 실제 hardware port의 latency로 해석하지 않는다.

Per-edge HW의 SPSC 대비 비율은 0.98–2.11배였다. 두 regular case에서는 HW가
조금 느렸다. 반면 세 software aggregation 방식은 모든 case에서 per-edge HW를
앞섰다. 이 범위에서는 transport 교체보다 aggregation algorithm의 차이가 컸다.

## 실제로 줄어든 전달 단위와 저장 공간

`agg_amo`와 `hw_agg`가 만든 nonzero owner vector 수는 같았다. 예를 들어 64/1,024
skew의 1,024개 입력 edge가 188개 vector가 되고, `hw_agg`는 188개 DATA chunk를
전송한다. 256/4,096 skew는 253개 vector이지만 vector가 커져 DATA chunk는 759개다.
그 외에 HW runtime의 DONE은 두 경우 모두 256개다. Message 수가 항상 input
edge 수와 같은 것도, vector 수와 같은 것도 아니다.

다음 byte 수는 aggregate buffer만의 footprint다. Graph input·output·stack·측정
buffer나 HW queue 전체의 면적을 합한 값이 아니다. 괄호 안은 eligible global 영역이며
실제 memory traffic이 아니다. Scratchpad는 core당 값이다.

| Mode | V=64 global allocated (eligible) | V=64 local/core | V=256 global allocated (eligible) | V=256 local/core |
| --- | ---: | ---: | ---: | ---: |
| `agg_amo`, `hw_agg` | 12,288 (12,288) | 0 | 36,864 (36,864) | 0 |
| `agg_lmem` | 8,192 (4,096) | 2,048 | 32,768 (16,384) | 8,192 |
| `agg_comb` | 1,024 (1,024) | 512 | 4,096 (4,096) | 2,048 |

세 SW mode는 이 case들에서 tree flags 128B를 사용한다. 같은 128B를 `hw_agg`에도
할당하지만 그 tree flags는 사용하지 않는다. SPSC는 16 actor, pair당 16 slots에서
metadata를 포함해 272KiB의 global queue를 할당한다. 자원 크기와 topology가 다른
algorithm 비교이며 동일 면적·동일 buffer 용량 비교는 아니다.

## 판단과 한계

이 결과는 Mitul-derived aggregation을 사용하면 기존 단순 SPSC보다 성능이 크게
개선됨을 확인한다. 세 방식 중 core combining이 가장 빨랐고, 현재 HW queue를
사용한 두 actor 경로보다 빨랐다. 이때 `agg_comb`는 generic mailbox 자체가 아니라
scratchpad를 이용한 bulk combine/flush/reduce 알고리즘이다.

한편 GPU direct atomic은 열 case에서 일곱 mode 중 가장 빨랐다. 이 workload는
작은 synthetic graph의 associative integer sum이며, 대형 graph의 DRAM working set,
non-associative handler, 동적인 task 생성이나 장기적인 streaming actor 처리를
대표하지 않는다. 원본 histogram binary의 성능이나 실제 GPU·FPGA의 speedup으로
확대하지 않는다. 다음 성능 분석은 source-private fold·dense scan·CSR 전송·polling·
종료 protocol을 분리한 ablation이 필요하며, stage별 worker 최대값의 합으로 대신할 수 없다.

## 결과 파일과 재현

- [CSV: 열 graph × 일곱 mode](forza_graph_coalesced_measurements.csv)
- [JSON: 75개 raw log·반복·footprint·manifest hash](forza_graph_coalesced_measurements.json)
- [원본·port·model·runtime provenance](forza_graph_coalesced_manifest.json)
- [분석 스크립트](../../tests/regression/forza_graph_coalesced/analyze.py)
- [새 benchmark 소스](../../tests/regression/forza_graph_coalesced/kernel.cpp)
- [HW pre-aggregation control](../../tests/regression/forza_graph_coalesced/kernel_hw_agg.cpp)

새 app의 `coalesced-run-manifest.json`은 official 실행의 command·exit status·CPU
reference 검증과 23개 binary/library fingerprint를 보존한다. 기존 HW/GPU reference의
`gpu-run-manifest.json`도 함께 참조한다. 원본 source와 새 source의 hash를 별도
manifest에 기록했고, runner는 측정 중 artifact가 바뀌지 않았음을 확인했다.

예를 들어 이미 build된 같은 파일로 64/1,024 skew를 다시 실행하려면 다음과 같다.

```bash
cd /export/nethomes/spark3100/vortex-forza-hwqueue/build64-graph-gpu/tests/regression/forza_graph_coalesced
export VORTEX_DRIVER=simx
export LD_LIBRARY_PATH=/export/nethomes/spark3100/vortex-forza-hwqueue/build64-graph-gpu/sw/runtime
./forza_graph_agg_amo -k kernel-agg_amo.vxbin -v64 -e1024 -gskew -s17
./forza_graph_agg_lmem -k kernel-agg_lmem.vxbin -v64 -e1024 -gskew -s17
./forza_graph_agg_comb -k kernel-agg_comb.vxbin -v64 -e1024 -gskew -s17
./forza_graph_hw_agg -k kernel-hw_agg.vxbin -v64 -e1024 -gskew -s17
```

One-message SPSC와 기존 reference는 같은 build의 `forza_graph` app directory에
보존되어 있다. SPSC도 이번 A-enabled driver와 새 kernel snapshot을 사용한다.

```bash
cd /export/nethomes/spark3100/vortex-forza-hwqueue/build64-graph-gpu/tests/regression/forza_graph
./forza_graph_sw -k kernel-sw.vxbin -v64 -e1024 -gskew -s17 -q16
./forza_graph_hw -k kernel-hw.vxbin -v64 -e1024 -gskew -s17
./forza_graph_gpu -k kernel-gpu.vxbin -v64 -e1024 -gskew -s17
```

분석기는 frozen official reference와 새 validation log만 읽고 이전 측정 파일을
수정하지 않는다. 전체 export를 재생성하는 명령은 다음과 같다.

```bash
cd /export/nethomes/spark3100/vortex-forza-hwqueue
python3 tests/regression/forza_graph_coalesced/analyze.py
```
