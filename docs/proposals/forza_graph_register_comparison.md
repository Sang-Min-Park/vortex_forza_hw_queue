# Register aggregation으로 message 경로의 한계를 다시 비교

**추가 시험에서도 현재 열 graph에서 최선의 측정 GPU를 이기는 message 구현은
나오지 않았다.** Register aggregation으로 atomic 수를 줄였지만 실행은 오히려
느려졌다. 이것은 현재의 작은 cached integer-ADD workload에 대한 결과이며,
message passing의 일반적인 불가능 증명은 아니다.

이번 실험은 “지금 느리다”와 “message passing은 원리적으로 GPU를 이길 수 없다”를
구별하기 위한 제한된 추가 비교다. 기존 message 구현은 edge마다 LMEM atomic을
수행한 뒤 결과를 보냈다. 새 구현은 register에서 반복된 destination을 먼저 합쳐,
atomic 자체를 줄인다. 같은 최적화를 direct GPU에도 적용한다.

## 앞선 결과가 일반적인 불가능 증명은 아닌 이유

현재 graph는 최대 4,096 edges이고 graph 배열은 약 37KiB다. Shared L2는 1MiB다.
따라서 GPU의 global atomic마다 HBM에 접근하는 상황이 아니다. 실제 driver의 L2와
LMEM은 각각 네 bank다. Hot LLC atomic은 bank당 최대 한 request/tick으로
commit하지만 local AMO는 bank를 3 cycles,
같은 word 연속 접근에서는 4 cycles 사용한다. Queue SRAM도 이미 bank conflict 없이
1-cycle 응답을 주는 낙관적인 모델이다. Message는 이 위에 일곱 CSR payload 쓰기와
commit·polling·handler를 수행한다. Bulk/DMA enqueue는 없다.

이전 `msg_warp`의 대표 fold 최대 29,946 cycles가 GPU 전체 22,779보다 길었던 것은
**그 fold를 유지한 구현**의 한계였다. Register aggregation은 fold 알고리즘을
바꾸므로 그 옛 하한을 새 구현에 그대로 적용하지 않는다. 반대로 새 message
알고리즘을 최적화하면서 GPU를 이전 버전으로 고정해 두지도 않는다.

## 무엇을 바꾸는가

세 cache mode는 lane마다 destination key 두 개와 int64 합계 두 개를 named scalar
register 상태로 유지하는 LRU combiner를 사용한다. 같은 key가 다시 나오면 register
합계에 더한다. Entry eviction이나 마지막 flush에서만 atomic update를 수행한다.
논리적인 edge 수, atomic flush 수, 전달 chunk 수가 서로 달라진다.
합계가 0이 된 valid entry도 flush로 센다. `gpu_range`는 edge마다 atomic을 수행한다.
Host는 worker별 cache를 정확히 재생해 flush 수까지 검사한다.

모든 새 mode는 warp마다 연속된 edge 범위를 균등 배분한다. 각 lane은 시작 위치의
CSR source를 처음 한 번 binary search하고, 이후에는 row end를 따라 전진하며
빈 row를 건너뛴다. Host가 source index 배열을 미리 만들어 주는 비용은 숨기지 않는다.
Descriptor와 row metadata도 cache한다.

| Mode | Register cache | Flush target과 전달 |
| --- | --- | --- |
| `msg_cache` | 같은 2-entry LRU | core scratchpad `partial[destination]`에 atomic flush; local bypass 후 remote message |
| `sw_cache` | 같은 2-entry LRU | 같은 core partial과 bypass; remote global partial/tree/pull |
| `gpu_cache` | 같은 2-entry LRU | global `y[destination]`에 직접 atomic flush |
| `gpu_range` | 없음 | 같은 balanced edge mapping에서 edge마다 direct global atomic |

Message/SW의 contiguous partial은 per-edge owner/index 변환을 없앤다. Owner vector를
보낼 때 `owner + bucket × actor_count`로 gather한다. 따라서 이 비교는 register cache,
partial layout과 edge scheduling을 함께 바꾼 algorithm 비교다. 같은 mapping의
`gpu_cache`와 `gpu_range`는 GPU에서 cache를 추가한 차이를 확인하는 control이다.

저장한 disassembly에서 cache 세 mode의 fold helper instruction byte가 같고,
streaming loop에 stack memory access가 없음을 확인했다. Callee register의 save/
restore 13개는 함수 진입·종료에서 한 번 수행하며 timed fold에 포함된다. Source의
named scalar만 보고 cache가 register에 있다고 가정한 것은 아니다. Static loop
footprint의 instruction 수를 매 edge의 dynamic 실행 수나 cycle로 환산하지 않는다.

## 공정하게 비교할 범위

같은 열 graph·weight·x·hash와 정확한 int64 CPU reference를 사용한다. 기존 driver와
runtime·hardware model은 바꾸지 않는다. 설정은 A/FORZA-enabled 2 cores × 2 warps ×
4 lanes, private L1 off + shared L2다. 기존 `msg_local`, `msg_warp`, `sw_warp`,
GPU vertex와 GPU warp의 frozen reference도 포함한다.

새 edge mapping은 source 작업을 재배치한다. Graph hash와 degree는 같아야 하지만
cross-core edge 수는 mapping마다 달라질 수 있다. 이를 input 차이로 오인하지 않고
같은 mapping group에서만 비교한다. Cache 세 mode의 per-worker 입력 순서와 LRU
규칙이 같으므로 atomic flush 수의 일치도 검사한다.

Primary metric은 새 process/device에서 host 결과 readback/cache flush 뒤의 device
cycles다. 모든 output vertex와 worker별 logical edge·flush·vector·chunk·완료 조건을
확인한다. Stage 최대값은 worker가 다를 수 있으므로 더하지 않는다. 최선 GPU를
고를 때는 기존 vertex/warp와 새 range/cache 네 가지를 모두 포함하고, 사후 선택을
자동 선택 정책으로 표현하지 않는다.

## 결과

새 official 40개 실행과 대표 반복 4개가 모두 CPU reference·worker별 edge·flush·
vector·chunk·완료 검증을 통과했다. 기존 reference 50개를 더해 **94개 log → 열
graph × 아홉 mode의 90개 측정값**으로 정리했다. Cache 세 mode의 atomic flush 수가
같았고 CPU replay와 일치했다. 네 반복은 cycle과 counter가 처음과 같았다.

### Atomic을 크게 줄여도 느려진 실제 case

256 vertices / 4,096 edges, hub, seed 17의 결과다. 같은 balanced edge mapping의
GPU range/cache는 register cache 유무를 비교한다. 숫자는 모두 readback/flush 뒤
device cycles다.

| Mode | Logical edges | Atomic flushes | Device cycles |
| --- | ---: | ---: | ---: |
| `gpu_range`: 같은 mapping, cache 없음 | 4,096 | 4,096 | 73,367 |
| `gpu_cache`: 같은 mapping, 2-entry LRU | 4,096 | 432 | 176,757 |
| `sw_cache`: 같은 LRU + core partial + global 전달 | 4,096 | 432 | 192,545 |
| `msg_cache`: 같은 LRU + core partial + HW message | 4,096 | 432 | 213,572 |

Atomic 수는 약 9.48배 줄었지만 GPU cache 실행은 range보다 **2.41배 오래 걸렸다.**
이 구간의 retired-instruction counter도 24,260에서 50,525로 늘었다. Counter는
readback/flush까지 포함하며 phase 내부 instruction만 센 것은 아니다. LRU의 key
비교·교체·SIMT control과 source tracking 비용을 함께 지불하므로 atomic 수만으로
성능을 설명할 수 없다. 정확한 stall별 비중까지 측정한 결과로 표현하지 않는다.

같은 case의 fold 최대값은 cache message 175,087, SW 175,270, GPU 175,224 cycles로
가까웠다. Register streaming loop에 stack spill이 없고 세 helper의 instruction byte도
같았으므로, 이 cache 알고리즘 자체의 처리 비용이 큰 상태다. Target이 scratchpad인지
global Y인지 바꾸는 것만으로 그 비용이 사라지지는 않았다.

강한 기존 GPU reference까지 포함하면 이 case의 최선은 vertex kernel의 **45,178
cycles**였다. 기존 최선 message는 `msg_local`의 101,351이었다. 새 알고리즘을
약한 새 GPU control 하나에만 비교하여 승리를 주장하지 않았다.

### 전체 열 case의 판단

- 새 `msg_cache`는 이전의 최선 message보다 모든 case에서 **1.33–2.64배 느렸다.**
- GPU의 register cache도 같은 mapping의 uncombined GPU range보다 모든 case에서
  **1.69–3.16배 느렸다.** Atomic 감소가 항상 이득이라는 가정은 이 구현에서 맞지 않았다.
- 기존·새 구현을 모두 포함한 최선 message와 최선 GPU를 graph마다 짝지어도
  message는 **1.78–5.00배 오래 걸렸고 GPU를 이긴 case는 0/10**이었다.
- 새 cache/range 중 어느 것도 이 열 case에서 최선의 측정 GPU가 되지 않았다.
  최선 message도 기존 `msg_local` 또는 `msg_warp`로 유지되었다.

예를 들어 64/1,024 skew, seed 17에서도 새 message 88,821은 기존 최선 message
47,081보다 느렸다. 새 GPU cache 72,873은 같은 mapping의 range 30,596보다
느렸고, 네 GPU 중 최선은 기존 warp kernel의 22,779였다. 모든 mode의 실제 값과
사후 최선 선택을 CSV/JSON에 보존했다. 이 선택을 자동 mode 선택 정책이나 모든
가능한 GPU 구현의 최적값으로 부르지 않는다.

## 그러면 현재 workload에서 불가능한가

현재까지 **이길 수 있다는 측정 근거는 없다.** 구현을 계속 조금씩 바꾸면 반드시
이긴다고 약속하기도 어렵다. 현재 계산은 cache-sized output의 단순 덧셈이며,
direct global AMO는 이미 LLC의 hardware operation이다. 반면 message는 GPU thread가
추가로 packet·CSR·polling·handler를 실행한다. 두 방식 모두 Vortex GPU에서 실행한다.

또한 여기서 actor의 논리적 owner를 정했다고 application 데이터가 그 core의
물리적으로 가까운 memory에 배치되지는 않는다. 일반 데이터는 같은 shared L2와
memory 경로를 사용한다. 현재 prototype에는 FORZA식으로 분리된 RZA/메모리 home의
locality 이득이 없다. Queue 자체와 본문 SRAM을 구현한 것만으로 생기는 성질이 아니다.

이 판단은 현재 workload·model·구현들의 범위다. 복잡한 owner 상태 변경을 여러
CAS/retry 대신 처리하거나, 실제 memory home/NUMA·cache-miss 비용을 줄이거나,
bulk descriptor/DMA enqueue로 CPU의 payload CSR 비용을 줄이는 설계에서는 tradeoff가
달라질 수 있다. 이들은 **아직 측정하지 않은 다른 설계 조건**이며 이길 것이라는
예측은 아니다. 적절한 topology·memory·port·bandwidth를 모델링하고 동일하게 최적화한
GPU control과 다시 비교해야 한다. Latency를 임의로 불리하게 바꿔 승리를 만드는
방식은 사용하지 않았다.

## 결과 파일과 재현

- [CSV: 열 graph × 아홉 mode](forza_graph_register_measurements.csv)
- [JSON: 원본 94개 log·atomic 수·mapping·반복·최선 선택](forza_graph_register_measurements.json)
- [Source/config/model/compiler provenance](forza_graph_register_manifest.json)
- [실제 register combiner](../../tests/regression/forza_graph_register/fold.h)
- [분석 스크립트](../../tests/regression/forza_graph_register/analyze.py)
- [저장한 compiled-fold 증거](/export/nethomes/spark3100/vortex-forza-hwqueue/build64-graph-gpu/tests/regression/forza_graph_register/compiled-fold-proof.txt)

새 app의 `register-run-manifest.json`은 official 실행의 명령·exit status와
artifact fingerprint를 보존한다. 원본 source·runtime·driver·model과 이전 측정은
변경하지 않았고 측정 전후 artifact가 같은 것을 확인했다. 실제 model의 L2/LMEM
bank 수와 compiler, helper byte proof와 분석기 의존성도 source manifest에 기록했다.

이미 build된 같은 파일로 hub case를 재실행하는 명령은 다음과 같다.

```bash
cd /export/nethomes/spark3100/vortex-forza-hwqueue/build64-graph-gpu/tests/regression/forza_graph_register
export VORTEX_DRIVER=simx
export LD_LIBRARY_PATH=/export/nethomes/spark3100/vortex-forza-hwqueue/build64-graph-gpu/sw/runtime
./forza_graph_msg_cache -k kernel-msg_cache.vxbin -v256 -e4096 -ghub -s17
./forza_graph_sw_cache -k kernel-sw_cache.vxbin -v256 -e4096 -ghub -s17
./forza_graph_gpu_cache -k kernel-gpu_cache.vxbin -v256 -e4096 -ghub -s17
./forza_graph_gpu_range -k kernel-gpu_range.vxbin -v256 -e4096 -ghub -s17
```

전체 export를 다시 만드는 명령이다. 분석기는 graph/mapping·정확한 flush 수·같은
cache algorithm과 아홉 mode의 누락을 검사하고 실패 시 오류 status를 반환한다.

```bash
cd /export/nethomes/spark3100/vortex-forza-hwqueue
python3 tests/regression/forza_graph_register/analyze.py
```
