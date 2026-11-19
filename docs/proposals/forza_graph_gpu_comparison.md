# FORZA actor와 direct GPU graph 실행 비교

**현재 prototype과 이 graph scatter에서는 direct GPU kernel이 훨씬 빠르다.**
Private L1과 shared L2를 켠 matched 비교에서, host 결과 readback/cache flush까지
포함한 device cycles 기준 GPU vertex kernel이 HW actor보다 50.16–486.02배 빠르다.
Software phase cycles 기준으로는 54.06–501.46배다. 이는 아래의 작은 synthetic graph와
SimX 모델에 대한 관측값이며 실제 GPU의 성능 배율을 뜻하지 않는다.

이전의 hardware/software actor queue 비교는 mailbox 구현을 교체하는 실험이었다.
이번에는 **같은 graph 계산을 ordinary GPU kernel로 직접 실행할 때와 비교**했다.
이전 측정 파일은 그대로 보존하고, A extension을 켠 새 build에서 세 mode를 모두
다시 측정했다. 공식 60개 실행과 대표 case의 3개 반복 모두 CPU reference와 완료
검증을 통과했다.

## 비교의 질문과 동일하게 맞출 조건

모든 mode는 한 번의 integer graph scatter를 계산한다.

```text
y[destination] += edge_weight(source, destination) * x[source]
```

Host가 생성한 같은 CSR graph·weight·x, 같은 seed와 graph hash를 사용한다.
모든 output vertex를 CPU reference와 정확히 비교한다. Core/warp/lane 수,
driver DSO, cache 설정은 각 비교 쌍에서 같게 맞춘다. Actor는 owner별 output
slice를 유지하고, GPU는 보통의 `y[destination]` 연속 배열을 쓴다. 동일한 논리
결과를 만드는 algorithm 비교이므로 GPU에 actor용 물리 배치를 강제하지 않는다.
실험 범위는 작은
synthetic graph의 한 scatter step이며, 전체 BFS·PageRank나 실제 GPU 전반의
성능을 뜻하지 않는다.

Graph matrix는 기존 일곱 case에 256 vertices / 4,096 edges의 regular·skew·hub를
더한 열 case다. 같은 열 case를 두 cache 설정과 세 mode에서 실행한다.
이 크기의 graph·output working set은 작으므로, 대형 graph의 DRAM working set과
network 병목까지 검증하는 실험으로 확대하지 않는다.

| Mode | edge를 나누는 방법 | 결과를 갱신하는 방법 | 완료 확인 |
| --- | --- | --- | --- |
| HW actor | source vertex를 thread에 cyclic 배분 | destination owner에게 메시지 전송 후 handler의 일반 load/add/store | 모든 DATA 처리와 runtime DONE 종료 |
| GPU vertex | 같은 source vertex cyclic 배분 | global output에 직접 atomic add | thread들의 계산·atomic 완료와 global barrier |
| GPU edge | global edge를 thread에 striped 배분; source는 CSR row pointer에서 binary search | global output에 직접 atomic add | thread들의 계산·atomic 완료와 global barrier |

GPU edge의 binary search 비용은 측정 구간에 포함한다. Edge-parallel 분배는
높은 source degree 때문에 한 thread만 오래 작업하는 문제를 줄이려는 별도
baseline이다. Vertex mode는 actor와 source 작업 배분을 맞추는 비교다.
둘 중 유리한 결과 하나만 선택하지 않고 각각 보고한다.

GPU kernel에 actor DONE message를 억지로 추가하지 않는다. Actor가 메시지 처리
완료를 알아내는 비용과 GPU가 atomic·barrier로 완료를 보장하는 비용은 각 방식의
실제 동작 차이다. 따라서 이것은 transport 하나만 바꾸는 ablation이 아니라
**같은 계산을 수행하는 두 programming model의 전체 phase 비교**다.

## 새 build를 사용하는 이유

Direct GPU의 64-bit global atomic을 지원하도록 extension A를 명시적으로 켠다.
이전 graph 측정은 A를 켠 동일 build로 측정한 것이 아니므로 그 cycle을 새 GPU
cycle과 직접 나누지 않는다. HW actor도 새 build에서 다시 실행한다.

```text
build64-graph-gpu
-DVX_CFG_EXT_FORZA_ENABLE -DVX_CFG_EXT_A_ENABLE
-DVX_CFG_DCACHE_DISABLE -DVX_CFG_L2_ENABLE
-DVX_CFG_NUM_CORES=2 -DVX_CFG_NUM_WARPS=2 -DVX_CFG_NUM_THREADS=4
```

16개의 lane thread와 두 core를 사용한다. 두 GPU mode와 actor mode 모두
private D-cache를 끄고 shared L2를 사용한다. 이 조건은 private L1이 있는 실제
GPU의 기본 성능과 같다고 주장하지 않는다. Actor의 mailbox 본문은 전용 SRAM
모델에서 읽으며 GPU atomic의 output은 shared global-memory 경로로 접근한다.
이 차이가 비교하려는 mechanism의 일부다.

보통 GPU에 불리한 cache 설정만 고른 결과가 되지 않도록, private D-cache와
shared L2를 모두 켠 `build64-graph-gpu-l1`도 별도 측정한다. 그 경우에도 HW
actor를 같은 설정에서 다시 실행한다. L1 on/off 결과를 섞어서 비율을 계산하지
않으며, 각 조건에서 세 mode를 나란히 보고한다.

## 이 모델에서 atomic을 해석하는 방법

Global atomic은 shared last-level cache bank에서 old value를 읽고 새 값을
반영한 뒤 응답한다. 같은 cache line의 atomic lane을 하나로 합치는 기능은
사용하지 않는다. Lane별 요청이 memory path와 cache bank 중재를 거친다.
따라서 hotspot의 같은 주소 갱신은 요청·bank 처리·응답 경로를 공유한다.
이 설정의 L2가 last-level cache이고, bank당 한 cycle에 최대 하나의 request를
commit한다. Global atomic에 별도의 4-cycle stall을 추가하는 모델은 아니다.
그 수치는 별도 shared/local-memory bank 모델의 규칙과 혼동하면 안 된다.
Cache arbitration·MSHR·pipeline·fill/replay·backpressure 비용은 있지만, multi-cycle
atomic ALU나 word lock의 실제 timing은 따로 모델링하지 않는다.

Atomic의 old value를 application이 사용하지 않더라도 LSU는 그 응답을 기다리는
pending request로 추적한다. 종료 시 `fence`가 pending memory operation을
기다리므로, 단순히 atomic 요청을 내보낸 시각을 계산 완료로 기록하는 비교는 아니다.

근거: [lane별 atomic 요청](../../sim/simx/mem/mem_coalescer.cpp:185),
[LLC atomic commit](../../sim/simx/mem/cache.cpp:1136),
[atomic response 추적](../../sim/simx/lsu_unit.cpp:520),
[fence의 pending 대기](../../sim/simx/lsu_unit.cpp:335).

이는 실제 GPU의 atomic throughput이나 같은 word에 대한 수학적 latency를
보장하는 모델은 아니다. 실제 회로의 atomic ALU pipeline, cache bank/port,
warp-level atomic combining·write merging과 비교한 calibration이 필요하다.
HW queue의 CSR·crossbar·SRAM도 물리 bank conflict와 port 수를 충분히 제한하지
않는 실험 모델이므로, 양쪽 결과를 실제 hardware speedup으로 확대하지 않는다.

## 측정 범위

Primary metric은 `phase_span_cycles = max(end)-min(start)`다. Graph 초기화와
공통 initial barrier 이후부터 각 mode의 software phase 종료까지 측정한다.
GPU는 atomic 완료와 final barrier를 포함하고, actor는 DATA handler와 DONE
종료·마지막 fence를 포함한다. Actor의 plain output store는 posted 요청이므로
이 fence만으로 shared L2·DRAM까지의 완료를 보장하지는 않는다. 따라서 phase
snapshot을 “두 mode 모두 output이 DRAM에 기록된 시각”이라고 해석하지 않는다.
`max_actor_cycles`라는 기존 출력 필드 이름이 남더라도
GPU에서는 thread별 소요 시간의 최대이며 actor 존재를 뜻하지 않는다.

Host 결과 readback 뒤 조회하는 memory request와 instruction counter는 별도
범위다. **Kernel launch부터 결과 readback/cache flush까지** 포함하므로 phase
cycle 구간의 traffic이라고 부르지 않는다. Global atomic이 hot L2에 hit하면
매 atomic이 DRAM request가 되는 것도 아니다. 이 counter만으로 atomic 호출 수나
contention 비용을 추정하지 않는다.

`device_cycles_post_readback_including_flush`는 host readback·cache flush 뒤 core 0의
`MCYCLE`을 조회한 secondary metric이다. 이 모델은 idle core도 매 platform tick에
cycle counter를 증가시키므로 core 0 하나가 elapsed device tick을 나타낸다.
Counter는 kernel마다 초기화되지 않으므로 case마다 새 process/device를 사용한다.
그 값에는 등록·초기화·kernel 종료·posted store drain·host cache flush 등 phase 밖
비용도 포함된다. Core별 값을 더하지 않으며 host wall time과도 구별한다.
Host readback의 모든 vertex 검증은 cache flush 이후 실제 결과까지 확인한다.

근거: [core cycle 증가](../../sim/simx/core.cpp:324),
[MCYCLE 조회](../../sim/simx/csr_unit.cpp:125),
[processor의 in-flight 작업 drain](../../sim/simx/processor.cpp:312).

## 결과

모든 실행에서 output vertex 전체가 CPU reference와 일치했다. GPU는 각 thread의
update 수와 총 `updates = edges`, `handlers = DONE = 0`, global-barrier 완료를
검사했다. Actor는 `sent = received = edges`, actor별 DONE 16개, 최종 outstanding
0을 검사했다. Actor의 완료 protocol을 GPU에 부과하지 않았다.

표의 `A`, `V`, `E`는 각각 HW actor, GPU vertex, GPU edge의 **phase cycles**다.
`A/V`, `A/E`가 클수록 해당 GPU kernel이 actor보다 빠르다. 같은 graph hash·seed와
cache 설정 안에서만 비율을 계산했다.

### Primary: private L1 on, shared L2 on

| Graph | Vertices / edges | Seed | A: HW actor | V: GPU vertex | E: GPU edge | A/V | A/E |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| regular | 32 / 128 | 17 | 312,426 | 2,566 | 10,217 | 121.76× | 30.58× |
| skew | 64 / 256 | 17 | 732,718 | 13,279 | 22,947 | 55.18× | 31.93× |
| hub | 64 / 256 | 17 | 1,303,618 | 4,839 | 22,954 | 269.40× | 56.79× |
| regular | 64 / 1,024 | 17 | 848,155 | 11,312 | 90,287 | 74.98× | 9.39× |
| skew | 64 / 1,024 | 17 | 2,550,310 | 43,496 | 90,256 | 58.63× | 28.26× |
| skew | 64 / 1,024 | 29 | 2,469,297 | 45,677 | 90,261 | 54.06× | 27.36× |
| hub | 64 / 1,024 | 17 | 5,634,383 | 11,309 | 90,269 | 498.22× | 62.42× |
| regular | 256 / 4,096 | 17 | 2,863,783 | 44,001 | 452,982 | 65.08× | 6.32× |
| skew | 256 / 4,096 | 17 | 9,780,976 | 174,859 | 452,757 | 55.94× | 21.60× |
| hub | 256 / 4,096 | 17 | 22,083,392 | 44,038 | 452,903 | 501.46× | 48.76× |

GPU vertex가 이 열 case에서 GPU edge보다 빨랐다. Edge 분배가 균등하더라도
CSR source를 찾는 binary search의 load·branch 비용이 들어간다. 따라서 이 결과를
“edge-parallel GPU 방식은 항상 느리다”로 일반화하지 않는다. 더 큰 graph, 다른
degree 분포, warp 단위 처리나 source index preprocessing은 별도 비교 대상이다.

### Controlled sensitivity: private L1 off, shared L2 on

| Graph | Vertices / edges | Seed | A: HW actor | V: GPU vertex | E: GPU edge | A/V | A/E |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| regular | 32 / 128 | 17 | 308,123 | 2,579 | 10,305 | 119.47× | 29.90× |
| skew | 64 / 256 | 17 | 733,454 | 13,238 | 23,023 | 55.41× | 31.86× |
| hub | 64 / 256 | 17 | 1,305,584 | 4,835 | 23,043 | 270.03× | 56.66× |
| regular | 64 / 1,024 | 17 | 810,103 | 11,269 | 90,660 | 71.89× | 8.94× |
| skew | 64 / 1,024 | 17 | 2,545,584 | 43,066 | 90,619 | 59.11× | 28.09× |
| skew | 64 / 1,024 | 29 | 2,444,813 | 45,148 | 90,647 | 54.15× | 26.97× |
| hub | 64 / 1,024 | 17 | 5,649,105 | 11,269 | 90,671 | 501.30× | 62.30× |
| regular | 256 / 4,096 | 17 | 2,869,626 | 43,784 | 454,670 | 65.54× | 6.31× |
| skew | 256 / 4,096 | 17 | 9,836,319 | 172,710 | 456,481 | 56.95× | 21.55× |
| hub | 256 / 4,096 | 17 | 22,139,466 | 43,781 | 457,629 | 505.69× | 48.38× |

L1을 끈 조건에서도 결론은 같았다. GPU vertex는 54.15–505.69배, GPU edge는
6.31–62.30배 빠르다. L1 on/off 사이의 작은 차이만으로 cache의 독립적 효과를
단정하지 않는다. Kernel layout·instruction scheduling·cache와 memory 순서가 함께
작용하는 전체 실행 결과다.

### 결과 materialization까지 포함한 secondary metric

Host readback/cache flush 뒤 MCYCLE 기준의 비율도 확인했다. 모든 비교에서 GPU가
더 빨랐다. 이는 actor의 posted output store가 phase 뒤에 drain될 수 있다는 측정
범위 차이만으로 위 결론이 생긴 것은 아니라는 확인이다. 이 지표에는 registration,
kernel 시작·종료, verification buffer store와 flush 등 다른 비용도 포함된다.

| Cache | GPU mode | Actor cycles / GPU cycles의 최소–최대 |
| --- | --- | ---: |
| L1 on + L2 | vertex | 50.16–486.02× |
| L1 on + L2 | edge | 6.32–61.70× |
| L1 off + L2 | vertex | 50.40–490.14× |
| L1 off + L2 | edge | 6.30–61.58× |

대표적인 L1-on case의 post-readback device cycles는 다음과 같다. 이 표의 비율은
output의 materialization까지 확인한 동일 구간의 actor cycles / GPU vertex cycles다.

| Graph | Vertices / edges | Seed | HW actor | GPU vertex | 비율 |
| --- | ---: | ---: | ---: | ---: | ---: |
| skew | 64 / 256 | 17 | 736,463 | 14,683 | 50.16× |
| skew | 64 / 1,024 | 17 | 2,554,154 | 44,899 | 56.89× |
| skew | 256 / 4,096 | 17 | 9,784,735 | 176,266 | 55.51× |
| hub | 256 / 4,096 | 17 | 22,087,151 | 45,445 | 486.02× |

예를 들어 L1 on, 256/4,096 hub에서 phase는 actor 22,083,392 cycles, GPU vertex
44,038 cycles였다. Readback/flush 뒤 device cycles는 각각 22,087,151과 45,445로,
비율은 486.02배였다. L1 on, 64/256 skew의 세 mode를 한 번씩 추가 실행했으며,
phase·DATA span·whole-device cycles·retry·memory/instruction counter가 첫 실행과
모두 일치했다. 이는 deterministic simulator 재현 확인이며 graph population에 대한
통계적 신뢰구간은 아니다.

## 이 결과를 어떻게 이해할 것인가

이번 계산에서 edge 하나가 하는 핵심 작업은 값의 곱셈과 한 output에 대한 덧셈이다.
GPU는 graph를 읽어 global atomic add 한 번을 실행한다. Actor는 같은 기여값을
packet으로 만들고 payload CSR 쓰기·commit·전달·polling·본문 복사·handler와 DONE
진행을 수행한다. 현재 검증용 graph handler는 packet의 source/destination/edge와
guard를 검사하고 per-message counter도 갱신한다. GPU는 packet이 없어 이러한
검사가 없고 thread별 update 수를 local 변수로 센다. 이 모두 현재 prototype의
측정 비용이며, 그 전부를 hardware transport 자체의 비용으로 해석하지 않는다.

예를 들어 L1 on, 256/4,096 hub의 전체 instruction counter는 actor 6,753,643,
GPU vertex 15,508이었다. 같은 구간의 off-chip request는 1,047과 725였다.
두 counter 모두 readback/flush까지 포함한다. 이 관측은 단순히 mailbox 본문을
SRAM에 둔 것만으로 이 작은 계산의 runtime·instruction 비용이 사라지지는 않음을
보여준다. 각 비용이 차지하는 비율을 정확히 알려면 별도 instrumentation이나
ablation이 필요하다.

이 결과로 확인한 것은 **현재 actor graph 경로가 direct GPU scatter보다 빠르지
않다는 것**이다. Hardware queue의 기능 구현과 software mailbox 대비 개선은
별도의 결과다. FORZA 전체 설계나 actor model 전반이 다른 graph·handler·runtime에서도
느리다는 결론으로 확대하지 않는다. 현재 실험은 message aggregation, production
runtime 최적화, CSC/pull SpMV, warp atomic combining, 대형 graph dataset을 비교하지 않았다.

## 결과 파일과 재현

- [전체 CSV: 40개의 matched GPU/actor 비교](forza_graph_gpu_measurements.csv)
- [JSON: 63개 raw log와 반복·측정 범위·manifest hash](forza_graph_gpu_measurements.json)
- [source·compiler·config manifest](forza_graph_gpu_manifest.json)
- [분석 스크립트](../../tests/regression/forza_graph/analyze_gpu.py)
- [측정 runner](../../tests/regression/forza_graph/run_gpu_comparison.py)

각 cache build의 app directory에 `gpu-run-manifest.json`이 있다. 이 파일에는
정확한 명령과 flags, 세 host binary·세 kernel vxbin/ELF/dump·공용 DSO의 SHA256,
각 실행의 exit status와 CPU-reference 성공 여부가 기록되어 있다. 공식 runner는
각 cache에서 30개 실행을 통과했고 측정 전후 artifact hash가 같음을 확인했다.
각 mode는 같은 cache build 안의 DSO를 사용하며 case마다 새 process를 실행했다.
별도 세 반복 log도 JSON에 보존했다. Host wall time은 simulator 비용이며 GPU speedup
계산에 사용하지 않았다.

이미 build된 primary L1-on 파일로 한 case를 재실행하는 명령은 다음과 같다.
Actor와 두 GPU mode 모두 해당 build의 library를 사용한다.

```bash
cd /export/nethomes/spark3100/vortex-forza-hwqueue/build64-graph-gpu-l1/tests/regression/forza_graph
export VORTEX_DRIVER=simx
export LD_LIBRARY_PATH=/export/nethomes/spark3100/vortex-forza-hwqueue/build64-graph-gpu-l1/sw/runtime
./forza_graph_hw -k kernel-hw.vxbin -v256 -e4096 -ghub -s17
./forza_graph_gpu -k kernel-gpu.vxbin -v256 -e4096 -ghub -s17
./forza_graph_gpu_edge -k kernel-gpu_edge.vxbin -v256 -e4096 -ghub -s17
```

L1-off 조건은 `build64-graph-gpu`의 host·kernel·library로 모두 교체한다.
각 build의 `validation-direct-*.log`를 다시 분석하는 명령은 다음과 같다.
분석기는 같은 cache group·graph hash만 묶고, 열 graph와 두 GPU mode의 누락,
CPU-reference/완료 검사 실패나 counter 오류를 실패 status로 보고한다.
이전 actor-only 측정 파일은 수정하지 않는다.

```bash
cd /export/nethomes/spark3100/vortex-forza-hwqueue
python3 tests/regression/forza_graph/analyze_gpu.py
```
