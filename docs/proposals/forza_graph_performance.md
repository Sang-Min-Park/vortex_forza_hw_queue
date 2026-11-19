# FORZA queue의 irregular graph 성능 비교

상태: 18개 실행의 CPU reference·message 수·DONE·drain 검증 완료. 7개 primary
비교와 반복 확인·capacity sensitivity를 아래에 기록했다. 현재 SimX 확장은
RTL과 cycle parity를 검증한 하드웨어가 아니므로,
이 실험은 **정해진 simulator 모델에서 두 mailbox 구현을 비교**하는 실험이다.
실제 GPU 또는 FPGA의 speedup이나 합성 가능한 SRAM의 성능을 증명하지 않는다.

## PGAS와 현재 구현의 구분

현재 Vortex의 일반 global-memory 주소 공간은 모든 core가 접근할 수 있다.
FORZA backend가 thread마다 독립된 DRAM partition을 자동 할당하거나, 다른
thread 소유 데이터에 대한 load/store를 금지하는 기능을 추가한 것은 아니다.

현재 구현이 actor별로 구분하는 것은 메시지 조립 상태, outstanding 카운터,
수신 슬롯과 runtime의 thread-private 상태다. mailbox 본문은 예약 주소 범위
`0x70000000..0x700fffff`를 통해 core의 전용 SRAM 모델로 읽는다. 이 범위는
일반 global DRAM 경로와 다르지만, 그 사실이 application 데이터 전체를 PGAS로
분할한다는 뜻은 아니다. SRAM load도 같은 core의 다른 actor 슬롯에 대한
접근 권한을 검사하지는 않는다.

Graph application은 예를 들어 `owner(v) = v % actor_count`를 정하고,
해당 owner만 vertex `v`의 결과를 갱신하게 만들 수 있다. 이는 공유 global
주소 공간 위에 software ownership을 두는 PGAS 방식이다. 이때 다른 owner의
값을 바꾸려는 작업은 메시지로 전달한다. 이 규칙은 graph workload와 runtime의
계약이며, hardware protection이나 one-sided remote-memory 기능은 아니다.
현재 graph 실험의 결과 배열은 실제로 `y[owner][v / actor_count]`처럼 actor별
연속 영역을 둔다. 이것은 application이 할당한 global buffer의 논리적 분할이며,
thread마다 별도 DRAM controller나 물리 memory가 생기는 것은 아니다.
Thread별 stack·TLS는 원래 별도 위치를 사용하지만, 이것도 임의의 application
객체를 thread마다 자동 PGAS partition으로 나누는 기능과는 구별해야 한다.

근거: [주소 경로 분류](../../sim/simx/types.h:878),
[SRAM load 처리](../../sim/simx/forza/forza_zone.cpp:261),
[actor별 상태](../../sim/simx/forza/forza_zone.h:37),
[thread별 stack·TLS 초기화](../../sw/kernel/src/vx_start.S:94).

## 무엇을 비교할 것인가

두 구현은 같은 simulator checkout, 같은 compiler와 kernel build 규칙,
같은 core/warp/lane 수, 같은 graph·seed·vertex ownership·산술·message payload와
같은 종료 protocol을 사용한다. 비교를 위해 예전 `vortex-actor` simulator의
cycle 수와 새 simulator의 cycle 수를 직접 나누지 않는다.

| 구현 | message transport |
| --- | --- |
| Hardware queue | 동일한 Selector가 CSR를 통해 ZEN/ZQM·전용 SRAM에 접근 |
| Software global mailbox | 동일한 Selector의 transport backend를 global-memory queue로 교체 |

Graph 계산은 CSR adjacency에서 edge를 따라 destination vertex owner에게 값을
전달하고, owner의 handler가 결과를 누적하는 scatter/reduction을 대상으로 한다.
이는 실제 graph의 불규칙 주소와 통신·degree imbalance를 사용하지만, 그 한 번의
scatter를 수행했다고 전체 BFS나 반복 PageRank까지 검증했다고 표현하지 않는다.

각 graph는 CPU reference와 모든 vertex 결과를 비교한다. checksum 하나만 맞추는
것으로 correctness를 대신하지 않는다. message 수·handler 수·최종 queue drain과
runtime 종료도 확인한다. regular, degree-skewed, hub/hotspot 형태를 구분하여
결과를 기록하며, 한 형태에서 얻은 speedup을 모든 irregular graph로 일반화하지
않는다.

Software queue의 크기, SPSC/MPSC 구조, atomic 사용과 cache 정책을 함께 기록한다.
두 transport의 buffering 용량이 다르면 이를 숨기지 않는다. 특히 지금의 private
L1은 일반적인 coherent cache가 아니므로, `volatile`나 `fence`만 붙인 polling
queue가 올바르다고 가정하지 않는다. 잘못된 stale-data baseline을 성능 비교에
사용하지 않으며, cache를 끄는 비교가 필요하면 두 구현 모두 같은 설정으로
비교하고 그 조건을 결과에 적는다.

## 고정한 primary 실험 조건

Primary 비교는 `build64-graph`에서 만든 **같은 SimX driver**를 사용한다.
HW/SW kernel만 transport header가 다르고, driver 설정은 다음과 같다.

```text
-DVX_CFG_EXT_FORZA_ENABLE
-DVX_CFG_DCACHE_DISABLE -DVX_CFG_L2_ENABLE
-DVX_CFG_NUM_CORES=2 -DVX_CFG_NUM_WARPS=2 -DVX_CFG_NUM_THREADS=4
```

한 cluster, 16 actor, core마다 persistent full-core CTA 하나를 사용한다. 두 mode
모두 private D-cache를 끄고 shared L2를 켠다. Software queue가 stale private-L1
copy를 읽는 문제가 없도록 선택한 **특정 cache regime**이며, private L1을 켠
Vortex 기본 설정의 성능으로 표현하지 않는다. Software queue의 hot line은 L2에
남을 수 있으므로 SW mode의 모든 polling이 DRAM에 간다고 가정하지 않는다.

두 kernel은 실제 `Selector<1,Packet>`과 같은 graph loop·handler·software DONE
진행을 사용한다. Kernel은 source vertex의 CSR row를 그 vertex owner에게
배분하고, edge마다 32B packet 하나를 전송한다. 두 backend 모두 이를 56B로
padding한다. 별도의 message aggregation은 사용하지 않는다.

Software baseline은 sender/receiver pair마다 하나의 bounded SPSC ring을 둔다.
한 queue에 producer는 한 actor, consumer도 한 actor이므로 lock·atomic RMW는
사용하지 않는다. 기본 capacity는 **pair당 16 slots**이며, slot은 64B다.
Publication sequence/DONE bit와 56B payload가 같은 shared-L2 line에 있고,
producer가 payload 뒤에 sequence를 publish한다. Consumer는 sequence를 확인하고
payload를 복사한 뒤 해당 slot을 다음 회전에 사용할 수 있도록 반환한다.
수신은 source queue들을 round robin으로 확인한다.

이 구현은 과거 `vortex-actor`의 simulator를 실행해서 얻은 baseline이 아니다.
그 software-mailbox 설계 계열을 현재 model·runtime으로 재구성한, 명시적인
SPSC baseline이다. “최적의 모든 software queue”를 대표한다고 주장하지 않는다.

두 queue의 자원 크기는 같지 않다. 16 actor, capacity 16이면 SW는 256개 pair에
총 4,096 slots를 두며 metadata를 합쳐 `256 × (16 × 64 + 64) = 278,528B`,
즉 272KiB를 할당한다. HW는 actor별 두 final slots와 core 공유 TX/RX queue를
사용한다. 따라서 이는 동일 resource budget의 circuit 비교가 아니다. `-q2`를
추가 측정하더라도 sensitivity 결과로 따로 기록하며 primary `-q16` 결과를
유리한 값으로 교체하지 않는다.

Software backend의 outstanding 값은 항상 0이다. 성공한 send는 receiver가
확인할 수 있는 SPSC slot에 publish된 상태이며, 별도 delivery ACK plane은 없다.
각 pair queue의 FIFO 순서로 DATA 뒤의 DONE이 처리되므로 최종 application
완료는 같은 Selector의 peer-DONE 조건으로 검사한다. HW는 SRAM READY 이후
ACK가 돌아오기까지 outstanding을 유지한다. 이 차이는 숨기지 않으며, per-message
ACK가 없는 SW 구현도 동일한 graph 결과와 DONE drain을 통과해야 한다.

구현: [graph kernel](../../tests/regression/forza_graph/kernel.cpp),
[CPU 검증·계측](../../tests/regression/forza_graph/main.cpp),
[software queue ABI](../../sw/common/forza_software_abi.h),
[software transport](/export/nethomes/spark3100/forza-runtime-vortex/inc/forza_message_lib.software.h).

Software의 publish/fence 규칙은 이 simulator의 shared-L2 처리와 slot reuse
correctness로 검증할 대상이다. 이를 임의 GPU의 hardware memory-model 증명으로
확장하지 않는다. 두 mode는 같은 compiler의 `vortex.mk` 규칙으로 SIMT control
flow를 생성해야 하며, 큰 function의 compiler conversion budget도 함께 맞춘다.

## 현재 SimX queue 모델의 성능 가정

| 부분 | 코드가 모델링하는 동작 | 아직 없는 물리적 제약 |
| --- | --- | --- |
| CSR | core당 한 pending warp CSR 작업을 tick마다 서비스하고, 그 trace의 active lane들을 같은 tick에서 처리한다. 요청·응답·SFU 출력에 registered hop이 있다. | Native CSR ring의 HART별 arbitration·실제 wire bandwidth를 복제하지 않았다. |
| 메시지 송신 | actor는 7 payload CSR와 commit을 사용한다. core는 local/external TX queue에서 합쳐서 한 메시지를 tick마다 꺼낸다. | CSR 조립의 실제 multiport SRAM 비용이나 회로 timing은 아직 정하지 않았다. |
| Fabric | input·output 하나당 tick마다 최대 한 개의 완성된 64B message와 metadata를 전달한다. 서로 다른 출력은 동시에 진행할 수 있다. | flit serialization, link 폭·길이·주파수, physical route 지연을 모델링하지 않는다. |
| ACK | 별도 plane으로 전달하고 SRAM slot을 READY로 만든 뒤 생성한다. | native FORZA의 더 이른 transport ACK 및 NACK/retry 저장·재전송 비용을 복제하지 않았다. |
| ZQM fill | 한 core에서 tick마다 최대 한 slot을 예약하고, 다음 처리에서 64B를 복사해 READY로 만든다. | SRAM bank conflict·fill과 reader 사이의 port contention이 없다. |
| SRAM load | LSU block마다 한 request를 tick마다 처리한다. request의 **각 active lane에 64B line**을 제공하고 1-cycle delay로 응답한다. | SRAM의 실제 bank 수·read port 수·lane 간 충돌을 제한하지 않는다. fill과 load가 같은 tick에 진행 가능하다. |
| response merge | LSU switch는 shared/cache/SRAM response 중 하나를 round robin으로 전달한다. | 전체 SRAM read 처리량이 RTL 구현에서 실현된다고 검증한 것은 아니다. |
| buffering | TX local/external 각 64, mailbox 번호별 ingress 64, actor·mailbox별 slot 2, 각 연결 경계·fabric input의 작은 FIFO와 credit을 사용한다. | queue memory 면적·port 비용과 다른 memory client의 자원 경쟁은 아직 물리적으로 모델링하지 않는다. |

소스: [CSR service](../../sim/simx/forza/forza_zone.cpp:203),
[SRAM request](../../sim/simx/forza/forza_zone.cpp:261),
[slot fill·ACK](../../sim/simx/forza/forza_zone.cpp:297),
[crossbar](../../sim/simx/forza/forza_fabric.cpp:21),
[credit boundary](../../sim/simx/regslice.h:58),
[LSU response arbitration](../../sim/simx/mem/local_mem_switch.cpp:50).

따라서 DRAM message traffic 감소가 관측되더라도 전체 graph cycles가 반드시
감소하는 것은 아니다. 작은 message의 CSR instruction 수, runtime polling,
소프트웨어 상태 spill, hub actor의 순차 처리와 head-of-line blocking이 비용이
될 수 있다. 개선·악화·차이 없음 중 실제 결과를 그대로 보고한다.

## 측정 항목과 범위

주 지표는 simulator가 계산한 device cycle이다. 초기화 및 공통 barrier 뒤에
`vx_rdcycle()`을 읽고, 모든 graph message 처리와 `done()` 종료 뒤에 다시 읽는다.
Primary 값은 `phase_span_cycles = max(end)-min(start)`이며,
`max_actor_cycles = max_actor(end-start)`와 start skew도 함께 기록한다. 마지막
DATA handler가 결과 갱신을 마친 timestamp의 `max(data_end)-min(start)`는
`data_span_cycles`로 기록해 DONE/control 비용과 구별한다. 초기화 전·host upload와
측정 종료 뒤 검증용 CSR 읽기는 이 cycle 구간에 포함하지 않는다. 두 구현에서
같은 정의를 사용하며 lane 수만큼 cycle을 합하지 않는다. Host wall-clock은
simulator 실행 비용으로만 기록하고 GPU 성능 지표로 사용하지 않는다.

함께 기록할 값은 다음과 같다.

- graph vertices/edges, degree/hotspot 통계, seed와 ownership 규칙;
- DATA message와 handler 수, DONE traffic과 종료 조건;
- busy/retry 수, 검증 오류, queue 종류와 용량;
- device cycles와 retired instructions;
- simulator off-chip memory request의 reads/writes/합계.

Host API는 `vx_device_mpm_query(dev, class, csr, core_id, &value)`다.
Memory class `VX_DCR_MPM_CLASS_MEM`의 `MEM_READS`와 `MEM_WRITES`는 machine-global
counter를 각 core 조회에서 동일하게 돌려주므로 **core 0을 한 번만 조회**한다.
`core_id=0xffffffff`로 합산하면 중복 집계된다. `MCYCLE`과 `MINSTRET`는 core별로
읽고 cycle은 최대값 또는 device 구간을, instructions는 core 합계를 사용한다.

이 memory counter는 memory simulator 입구의 request를 세며, graph 데이터 외에
instruction fetch, stack/state spill, queue traffic도 포함한다. 매 kernel reset에서
0이 된다. 이번 host는 결과 buffer들을 읽은 뒤 counter를 조회하므로, 값에는
**kernel launch부터 host 결과 readback에 따른 cache flush까지** 포함된다.
Raw log의 `*_whole_kernel`이라는 이름보다 실제 측정 범위가 넓으며, CSV에서는
`*_post_readback_including_flush`로 명시한다. 위 device phase cycle 구간만의
memory traffic이라고 설명하지 않는다. Retired-instruction counter도 같은
host 조회 시점의 값이며, timed phase 내부 instruction만 분리한 값이 아니다. Atomic RMW는
model의 `is_write()` 분류로 write request에 포함될 수 있으므로 read count 하나만
감소했다고 bandwidth 개선이라고 결론내리지 않는다. 64B를 곱한다면 request
granularity의 bus volume이며 application payload byte 수와 구분한다.

근거: [counter 생성](../../sim/simx/processor.cpp:184),
[counter reset](../../sim/simx/processor.cpp:344),
[public query](../../sw/runtime/common/perf.cpp:196),
[global memory report](../../sw/runtime/common/perf.cpp:588),
[device cycle helper](../../sw/kernel/include/vx_intrinsics.h:235).

## 결과

모든 실행은 vertex별 integer SpMV CPU reference와 정확히 일치했다. 각 실행에서
DATA `sent = received = edge_count`, DONE 256개, 최종 outstanding 0을 확인했다.
Primary는 위에서 고정한 SW capacity 16이며, 같은 graph hash·vertices·edges·seed의
HW/SW만 비교했다. 표의 비율은 **SW phase cycles / HW phase cycles**다. 1보다 크면
해당 SimX 조건에서 HW 경로가 빠르다. 마지막 열은 같은 방식으로 계산한
`SW data_span_cycles / HW data_span_cycles`다. 시뮬레이션 실행의 host 경과 시간은 사용하지 않았다.

| Graph | V / E | Seed | HW phase cycles | SW phase cycles | 비율 | DATA 완료 비율 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| regular | 32 / 128 | 17 | 315,512 | 303,055 | 0.961× | 1.498× |
| skew | 64 / 256 | 17 | 733,676 | 1,341,117 | 1.828× | 1.842× |
| hub | 64 / 256 | 17 | 1,312,316 | 2,597,912 | 1.980× | 2.089× |
| regular | 64 / 1,024 | 17 | 810,174 | 954,935 | 1.179× | 1.324× |
| skew | 64 / 1,024 | 17 | 2,566,812 | 5,227,924 | 2.037× | 2.052× |
| skew | 64 / 1,024 | 29 | 2,459,480 | 5,077,282 | 2.064× | 2.080× |
| hub | 64 / 1,024 | 17 | 5,646,057 | 11,265,668 | 1.995× | 2.015× |

이 실험에서는 skew·hub가 약 1.83–2.06배의 phase 비율을 보였다. 작은 regular
graph는 HW가 4.11% 더 오래 걸렸다. 이 경우 DATA 처리는 HW가 먼저 끝났지만,
DATA 종료부터 전체 phase 종료까지의 관측된 차이가 HW 207,958 cycles,
SW 141,887 cycles였다. 이 tail에는 남은 producer/runtime 진행과 DONE 종료가
들어간다. 별도 ablation 없이 이를 ACK나 특정 단일 장치의 비용으로 단정하지 않는다.

다음은 **host 결과 readback/cache flush까지 포함한** off-chip request 합계다.
위 phase cycle 표와 측정 구간이 다르며, message payload traffic만의 값도 아니다.

| Graph, V / E, seed | HW reads + writes | SW reads + writes | request 합계 감소 |
| --- | ---: | ---: | ---: |
| regular, 32 / 128, 17 | 277 + 176 = 453 | 1,152 + 842 = 1,994 | 77.28% |
| skew, 64 / 256, 17 | 303 + 180 = 483 | 1,342 + 959 = 2,301 | 79.01% |
| hub, 64 / 256, 17 | 303 + 180 = 483 | 1,310 + 968 = 2,278 | 78.80% |
| regular, 64 / 1,024, 17 | 399 + 180 = 579 | 2,178 + 1,742 = 3,920 | 85.23% |
| skew, 64 / 1,024, 17 | 399 + 180 = 579 | 1,982 + 1,518 = 3,500 | 83.46% |
| skew, 64 / 1,024, 29 | 399 + 180 = 579 | 1,961 + 1,493 = 3,454 | 83.24% |
| hub, 64 / 1,024, 17 | 399 + 180 = 579 | 1,519 + 1,045 = 2,564 | 77.42% |

64/256 skew의 두 mode를 각각 한 번 더 실행했다. phase·DATA span·retry·memory
counter·instructions가 첫 실행과 모두 같았다. 이것은 해당 simulator 실행의
재현 확인이며, 여러 graph seed에 대한 통계적 신뢰구간을 뜻하지 않는다.
64/1,024 skew는 별도 seed 29에서도 위와 같이 개선되었지만, graph 분포 전반을
대표하는 sampling이라고 표현하지 않는다.

Capacity sensitivity는 primary와 분리한다. 같은 64/256 graph에서 SW pair당
capacity를 2로 줄이면 skew는 1,811,850 cycles, hub는 2,891,783 cycles였다.
HW는 각각 733,676과 1,312,316 cycles로, 비율은 2.470×와 2.204×다. 이 두 값을
primary의 1.828×와 1.980× 대신 사용하지 않는다. HW의 공유 buffering 구조와
SW pair ring의 topology·총 용량이 다르므로 capacity 2도 동일 면적 비교가 아니다.

측정 대상은 이 SPSC baseline과 동일 Selector를 사용하는 graph scatter다.
최적화한 MPSC queue, direct remote atomic 기반 SpMV, aggregation, 다른 runtime,
전체 BFS·PageRank와의 성능 우위를 검증한 결과가 아니다. 실제 RTL 성능을 예측하려면
SRAM port/bank·link 폭과 latency·CSR arbitration을 정하고 timing 검증이 추가로 필요하다.

## 결과 파일과 재현

- [CSV: primary와 capacity sensitivity](forza_graph_measurements.csv)
- [JSON: 모든 raw log·반복·검증·측정 범위](forza_graph_measurements.json)
- [측정 artifact SHA256와 구성 manifest](forza_graph_manifest.json)
- [분석 스크립트](../../tests/regression/forza_graph/analyze.py)

분석기는 `GRAPH`, `PASSED`, `RESULT`를 모두 요구하고 graph hash와 count를 검사한다.
동일 case 반복은 median으로 묶되 raw 값·반복 횟수·완전 일치 여부를 보존한다.
현재 반복은 완전히 같아서 median이 결과를 바꾸지 않는다. Capacity 2는 따로 표시한다.
잘못된 log나 pair가 없는 graph는 경고와 실패 exit status로 보고한다. 현재 환경에는
matplotlib이 없어 그림을 생성하지 않았으며, 표·CSV·JSON으로 결과를 제공한다.
matplotlib이 설치된 환경에서 같은 스크립트를 실행하면 PNG와 SVG도 생성한다.

측정에 사용한 driver snapshot은 아래 build tree의 `libvortex.so`,
`libvortex-simx.so`, `libsimx.so`다. 두 transport가 같은 DSO를 사용한다.
각 host binary와 `kernel-hw.vxbin`, `kernel-sw.vxbin`을 별도로 보존했고,
파일 hash·compiler·config·수정 상태를 manifest에 기록했다. 아래는 한 primary
pair를 같은 파일로 재실행하는 명령이다. 매번 새 process를 사용한다.

```bash
cd /export/nethomes/spark3100/vortex-forza-hwqueue/build64-graph/tests/regression/forza_graph
export VORTEX_DRIVER=simx
export LD_LIBRARY_PATH=/export/nethomes/spark3100/vortex-forza-hwqueue/build64-graph/sw/runtime
./forza_graph_hw -k kernel-hw.vxbin -v64 -e1024 -gskew -s17
./forza_graph_sw -k kernel-sw.vxbin -v64 -e1024 -gskew -s17 -q16
```

나머지 primary는 표의 `-g`, `-v`, `-e`, `-s`를 같은 쌍에 적용한다.
별도 sensitivity는 64/256 skew·hub에서 SW만 `-q2`로 실행한다.
출력 log는 `build64-graph/tests/regression/forza_graph/validation-*.log`에 보존되어 있다.
저장된 18개 log의 분석을 다시 실행하는 명령은 다음과 같다.

```bash
cd /export/nethomes/spark3100/vortex-forza-hwqueue
python3 tests/regression/forza_graph/analyze.py
```
