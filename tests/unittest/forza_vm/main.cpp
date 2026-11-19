#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
#include "forza/forza_vm.h"
#include "mem/local_mem.h"

using namespace vortex;

namespace {
void check(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << std::endl;
    std::exit(1);
  }
}

uint64_t unpack(const std::shared_ptr<mem_block_t>& block, uint32_t offset) {
  check(bool(block), "missing memory data");
  uint64_t value=0;
  for(uint32_t b=0;b<8;++b)value|=uint64_t(block->at(offset+b))<<(b*8);
  return value;
}

struct Control { uint32_t response_mode=1; bool acknowledge_results=true; };
struct MemoryEvent { uint64_t cycle; MemReq request; };
struct ResultEvent { uint64_t cycle; ForzaVmResult result; };
struct CompletionEvent { uint64_t cycle; ForzaAck ack; };

class Probe : public SimObject<Probe> {
public:
  Probe(const SimContext& ctx,const char* name)
    : SimObject<Probe>(ctx,name), commands(this), results(this), reply_ack(this),
      completion(this), configuration(this), configured(this), control(this),
      memory_in(this), memory_out(this), memory_responses(this), memory_forward(this),
      cpu_out(this),cpu_in(this) {}
  SimChannel<ForzaMessage> commands;
  SimChannel<ForzaVmResult> results;
  SimChannel<ForzaAck> reply_ack;
  SimChannel<ForzaAck> completion;
  SimChannel<ForzaVmConfig> configuration;
  SimChannel<uint64_t> configured;
  SimChannel<Control> control;
  SimChannel<MemReq> memory_in;
  SimChannel<MemReq> memory_out;
  SimChannel<MemRsp> memory_responses;
  SimChannel<MemRsp> memory_forward;
  SimChannel<MemReq> cpu_out;
  SimChannel<MemRsp> cpu_in;
  std::vector<MemoryEvent> accesses;
  std::vector<ResultEvent> values;
  std::vector<CompletionEvent> completions;
  std::map<uint64_t,uint64_t> response_cycles;
protected:
  void on_reset() {
    mode_=Control{};
    acknowledgments_.clear();accesses.clear();values.clear();completions.clear();
    response_cycles.clear();
  }
  void on_tick() {
    if(!control.empty()) {mode_=control.peek();control.pop();}
    if(!memory_in.empty()&&!memory_out.full()) {
      accesses.push_back({SimPlatform::instance().cycles(),memory_in.peek()});
      memory_out.send(memory_in.peek(),1);memory_in.pop();
    }
    if(mode_.response_mode&&!memory_responses.empty()&&!memory_forward.full()
       &&(mode_.response_mode!=6||bool(memory_responses.peek().data))) {
      auto response=memory_responses.peek();
      response_cycles[response.uuid]=SimPlatform::instance().cycles();
      if(mode_.response_mode==2)response.uuid^=1;
      if(mode_.response_mode==3)response.tag+=FORZA_VM_CONTEXTS;
      if(mode_.response_mode==4)response.hart_id^=1;
      if(mode_.response_mode==5)response.data.reset();
      memory_forward.send(response,1);memory_responses.pop();
    }
    if(!results.empty()) {
      auto result=results.peek();results.pop();
      values.push_back({SimPlatform::instance().cycles(),result});
      acknowledgments_.push_back({result.request.source_zone,result.request.source_actor,
                                 result.request.mailbox,result.request.sequence,false});
    }
    if(mode_.acknowledge_results&&!acknowledgments_.empty()&&!reply_ack.full()) {
      auto ack=acknowledgments_.front();
      if(mode_.response_mode==7)ack.mailbox^=1;
      if(mode_.response_mode==8)ack.sequence+=9999;
      reply_ack.send(ack,1);acknowledgments_.pop_front();
    }
    if(!completion.empty()) {
      completions.push_back({SimPlatform::instance().cycles(),completion.peek()});
      completion.pop();
    }
  }
private:
  friend class SimObject<Probe>;
  Control mode_;
  std::deque<ForzaAck> acknowledgments_;
};

uint64_t code(uint32_t op,uint32_t dst=0,uint32_t a=0,uint32_t b=0,int32_t imm=0) {
  return forza_vm_instruction(op,dst,a,b,imm);
}

std::vector<uint64_t> add_xor_program(uint32_t offset=0) {
  return {code(FORZA_VM_LD,3,0,0,offset),code(FORZA_VM_ADD,4,3,1),
          code(FORZA_VM_ST,4,0,0,offset),code(FORZA_VM_XOR,5,3,2),
          code(FORZA_VM_RET,3,4,5)};
}
std::vector<uint64_t> conditional_program() {
  return {code(FORZA_VM_LD,3,0),code(FORZA_VM_SEXT32,1,1),
          code(FORZA_VM_BGE,0,3,1,5),code(FORZA_VM_ADDI,4,3,0,1),
          code(FORZA_VM_ST,4,0),code(FORZA_VM_RET,3,1,2)};
}

class Fixture {
public:
  Fixture()
    : vm_(ForzaVm::Create("programmable-unit",0)),
      memory_(LocalMem::Create("real-localmem",LocalMem::Config{
          FORZA_VM_REGION_BYTES*FORZA_VM_REGIONS,8,2,2,false})),
      probe_(Probe::Create("vm-probe")) {
    probe_->commands.bind(&vm_->request_in);
    vm_->result_out.bind(&probe_->results);
    probe_->reply_ack.bind(&vm_->result_ack_in);
    vm_->completion_out.bind(&probe_->completion);
    probe_->configuration.bind(&vm_->config_in);
    vm_->config_ack_out.bind(&probe_->configured);
    vm_->memory_req_out.bind(&probe_->memory_in);
    probe_->memory_out.bind(&memory_->Inputs.at(1));
    memory_->Outputs.at(1).bind(&probe_->memory_responses);
    probe_->memory_forward.bind(&vm_->memory_rsp_in);
    probe_->cpu_out.bind(&memory_->Inputs.at(0));
    memory_->Outputs.at(0).bind(&probe_->cpu_in);
    reset();
  }
  void reset() {SimPlatform::instance().reset();}
  void tick(uint32_t count=1) {
    for(uint32_t i=0;i<count;++i)SimPlatform::instance().tick();
  }
  void mode(uint32_t responses,bool acknowledge=true) {
    probe_->control.send(Control{responses,acknowledge},1);tick(2);
  }
  void configure(uint32_t program,const std::vector<uint64_t>& words) {
    for(uint32_t pc=0;pc<words.size();++pc) {
      uint64_t sequence=++sequence_;
      probe_->configuration.send({program,pc,words[pc],sequence},1);
      bool done=false;
      for(uint32_t i=0;i<64;++i) {
        tick();
        if(!probe_->configured.empty()) {
          check(probe_->configured.peek()==sequence,"configuration identity lost");
          probe_->configured.pop();done=true;break;
        }
      }
      check(done,"configuration ACK missing");
    }
  }
  ForzaMessage request(uint32_t program,uint32_t region,uint32_t arg0,uint32_t arg1,
                        bool direct=false) {
    ForzaMessage result;
    result.source_zone=0;result.source_actor=0;result.dest_zone=0;
    result.dest_pe=region;result.mailbox=0;result.sequence=++sequence_;
    result.words[0]=forza_message_control(0,region,0,direct?FORZA_VM_DIRECT_REQUEST_OPCODE:FORZA_VM_REQUEST_OPCODE);
    result.words[1]=forza_vm_pack(arg0,arg1);result.words[2]=0;result.words[3]=program;
    return result;
  }
  bool submit(const ForzaMessage& request) {
    if(probe_->commands.full())return false;
    probe_->commands.send(request,1);return true;
  }
  void complete(uint32_t count,uint32_t limit=10000) {
    for(uint32_t i=0;i<limit&&probe_->completions.size()<count;++i)tick();
    check(probe_->completions.size()==count,"operation completion count mismatch");
  }
  uint64_t cpu(uint32_t word,MemOp operation,uint64_t value=0) {
    MemReq request;
    request.op=operation;request.addr=VX_MEM_LMEM_BASE_ADDR+uint64_t(word)*8;
    request.flags.local=true;request.flags.strsp=operation==MemOp::ST;
    request.tag=++sequence_;request.uuid=sequence_;request.hart_id=0;
    uint32_t offset=request.addr&(VX_CFG_MEM_BLOCK_SIZE-1);
    request.byteen=255ull<<offset;
    if(operation!=MemOp::LD) {
      request.data=std::make_shared<mem_block_t>();request.data->fill(0);
      for(uint32_t b=0;b<8;++b)request.data->at(offset+b)=value>>(b*8);
    }
    probe_->cpu_out.send(request,1);
    for(uint32_t i=0;i<256;++i) {
      tick();
      if(!probe_->cpu_in.empty()) {
        auto response=probe_->cpu_in.peek();probe_->cpu_in.pop();
        check(response.tag==request.tag&&response.uuid==request.uuid,"CPU memory identity lost");
        return operation==MemOp::LD?unpack(response.data,offset):0;
      }
    }
    check(false,"CPU memory operation did not complete");return 0;
  }
  Probe& probe() {return *probe_;}
private:
  ForzaVm::Ptr vm_;
  LocalMem::Ptr memory_;
  Probe::Ptr probe_;
  uint64_t sequence_=0;
};

void normal(Fixture& f) {
  f.configure(0,add_xor_program());f.configure(1,conditional_program());
  f.cpu(5*8,MemOp::ST,10);f.cpu(6*8,MemOp::ST,4);
  auto r=f.request(0,5,7,3);check(f.submit(r),"submit failed");f.complete(1);
  check(f.probe().values[0].result.values==std::array<uint64_t,3>{10,17,9},"program0 arithmetic wrong");
  check(f.cpu(5*8,MemOp::LD)==17,"program0 state wrong");
  check(f.submit(f.request(1,6,10,123,true)),"direct submit failed");f.complete(2);
  check(f.probe().values[1].result.values==std::array<uint64_t,3>{4,10,123},"conditional taken-work result wrong");
  check(f.cpu(6*8,MemOp::LD)==5,"conditional update missing");
  check(f.submit(f.request(1,6,3,7)),"submit failed");f.complete(3);
  check(f.probe().values[2].result.values==std::array<uint64_t,3>{5,3,7},"conditional branch result wrong");
  check(f.cpu(6*8,MemOp::LD)==5,"conditional no-update changed state");
  check(f.submit(f.request(1,6,UINT32_MAX,9)),"submit failed");f.complete(4);
  check(f.probe().values[3].result.values==std::array<uint64_t,3>{5,UINT64_MAX,9},"SEXT32 or signed branch wrong");
  check(f.probe().accesses.size()==6,"unexpected memory accesses for two programs");
  for(const auto& e:f.probe().accesses) {
    const auto& q=e.request;
    check(__builtin_popcountll(q.byteen)==8&&q.flags.local,"VM memory access width/path changed");
    if(q.op==MemOp::ST)check(q.flags.strsp,"VM store is not acknowledged");
  }

  f.reset();f.configure(0,add_xor_program(56));
  f.cpu(FORZA_VM_REGIONS*8-1,MemOp::ST,UINT64_MAX);
  check(f.submit(f.request(0,FORZA_VM_REGIONS-1,1,UINT32_MAX)),"last region submit failed");f.complete(1);
  check(f.cpu(FORZA_VM_REGIONS*8-1,MemOp::LD)==0,"last aligned word or wrapping add wrong");
  check(f.probe().values[0].result.values[0]==UINT64_MAX,"last region old value wrong");

  f.reset();f.configure(2,{code(FORZA_VM_IMM,3,0,0,-1),code(FORZA_VM_AND,4,1,2),
                         code(FORZA_VM_ADDI,5,4,0,-1),code(FORZA_VM_RET,3,4,5)});
  uint64_t began=SimPlatform::instance().cycles();
  check(f.submit(f.request(2,0,0xf0f0,0x0ff0)),"ALU submit failed");f.complete(1);
  check(f.probe().values[0].result.values==std::array<uint64_t,3>{UINT64_MAX,0xf0,0xef},"ALU program wrong");
  check(f.probe().accesses.empty(),"ALU program made memory accesses");
  check(f.probe().values[0].cycle-began>=8,"instruction fetch/execute timing collapsed");
}

void withheld_store(Fixture& f) {
  f.configure(0,add_xor_program());f.cpu(0,MemOp::ST,0);f.mode(6);
  check(f.submit(f.request(0,0,1,2)),"submit failed");f.tick(200);
  check(f.probe().accesses.size()==2&&f.probe().values.empty()&&f.probe().completions.empty(),
        "RET or completion preceded acknowledged store");
  f.mode(1);f.complete(1);
  if(f.probe().values[0].result.values!=std::array<uint64_t,3>{0,1,2}) {
    auto v=f.probe().values[0].result.values;
    std::cerr<<"resumed values="<<v[0]<<","<<v[1]<<","<<v[2]<<std::endl;
  }
  check(f.probe().values[0].result.values==std::array<uint64_t,3>{0,1,2},"resumed store corrupted result");
}

void withheld_reply(Fixture& f) {
  f.configure(0,add_xor_program());f.cpu(0,MemOp::ST,0);f.mode(1,false);
  check(f.submit(f.request(0,0,7,2)),"submit failed");f.tick(200);
  check(f.probe().values.size()==1&&f.probe().completions.empty(),"operation completed before result ACK");
  check(f.cpu(0,MemOp::LD)==7,"result was published before state commit");
  f.mode(1,true);f.complete(1);
}

void same_region(Fixture& f) {
  f.configure(0,add_xor_program());
  f.cpu(3*8,MemOp::ST,0);
  uint32_t sent=0;
  for(uint32_t cycle=0;cycle<10000&&f.probe().completions.size()<16;++cycle) {
    if(sent<16&&f.submit(f.request(0,3,1,0)))++sent;
    f.tick();
  }
  check(sent==16&&f.probe().completions.size()==16,"same-region stream failed");
  std::vector<uint64_t> old;
  for(const auto& e:f.probe().values) {
    old.push_back(e.result.values[0]);
    check(e.result.values[1]==e.result.values[0]+1,"same-region state update torn");
  }
  std::sort(old.begin(),old.end());
  for(uint32_t i=0;i<old.size();++i)check(old[i]==i,"same-region operations overlapped");
  check(f.cpu(3*8,MemOp::LD)==16,"same-region final state wrong");
  check(f.probe().accesses.size()==32,"same-region operation count changed");
  for(const auto& e:f.probe().values) {
    uint64_t sequence=e.result.request.sequence;
    uint64_t store_ack=0;
    for(const auto& access:f.probe().accesses)if((access.request.uuid>>16)==sequence&&access.request.op==MemOp::ST) {
      auto found=f.probe().response_cycles.find(access.request.uuid);
      check(found!=f.probe().response_cycles.end(),"missing store acknowledgement");store_ack=found->second;
    }
    check(store_ack&&e.cycle>store_ack,"old tuple published before final store ACK");
  }
}

void saturation(Fixture& f) {
  f.configure(0,{code(FORZA_VM_RET,1,2,0)});f.mode(1,false);
  uint32_t sent=0;
  for(uint32_t cycle=0;cycle<512;++cycle) {
    if(sent<100&&f.submit(f.request(0,sent%FORZA_VM_REGIONS,sent,0)))++sent;
    f.tick();
  }
  check(f.probe().values.size()==FORZA_VM_CONTEXTS,"context count is not bounded at four");
  check(f.probe().completions.empty(),"withheld ACK allowed completion");
  check(sent>=FORZA_VM_QUEUE_DEPTH+FORZA_VM_CONTEXTS
        &&sent<=FORZA_VM_QUEUE_DEPTH+FORZA_VM_CONTEXTS+2,"request queue capacity changed");
  f.mode(1,true);f.complete(sent);
  check(f.probe().values.size()==sent,"saturation lost or duplicated replies");
}

bool negative(Fixture& f,const std::string& name) {
  if(name=="bad-uuid"||name=="bad-tag"||name=="bad-hart"||name=="missing-data") {
    f.configure(0,add_xor_program());
    f.mode(name=="bad-uuid"?2:name=="bad-tag"?3:name=="bad-hart"?4:5);
  } else if(name=="bad-result-mailbox"||name=="bad-result-sequence") {
    f.configure(0,{code(FORZA_VM_RET,1,2,0)});
    f.mode(name=="bad-result-mailbox"?7:8);
  } else if(name=="unconfigured") {
  } else if(name=="bad-opcode") {
    f.configure(0,{255});
  } else if(name=="bad-branch") {
    f.configure(0,{code(FORZA_VM_BEQ,0,0,0,FORZA_VM_PROGRAM_WORDS)});
  } else if(name=="step-limit") {
    f.configure(0,{code(FORZA_VM_BEQ,0,0,0,0)});
  } else if(name=="bad-address") {
    f.configure(0,{code(FORZA_VM_LD,3,0,0,64)});
  } else if(name=="unaligned-address") {
    f.configure(0,{code(FORZA_VM_LD,3,0,0,1)});
  } else if(name=="bad-request-opcode") {
    auto request=f.request(0,0,1,0);request.words[0]=0;
    check(f.submit(request),"submit failed");f.tick(256);return false;
  } else return false;
  check(f.submit(f.request(0,0,1,2)),"submit failed");f.tick(4096);return false;
}
}

int main(int argc,char** argv) {
  std::string name=argc>1?argv[1]:"normal";
  Fixture fixture;
  try {
    if(name=="normal")normal(fixture);
    else if(name=="withheld-store")withheld_store(fixture);
    else if(name=="withheld-reply")withheld_reply(fixture);
    else if(name=="same-region")same_region(fixture);
    else if(name=="saturation")saturation(fixture);
    else {
      bool rejected=negative(fixture,name);check(rejected,"malformed operation was not rejected");
    }
  } catch(const std::runtime_error& e) {
    if(name=="normal"||name=="withheld-store"||name=="withheld-reply"||name=="same-region"||name=="saturation")throw;
    std::cout << "PASSED: " << name << " rejected: " << e.what() << std::endl;return 0;
  }
  std::cout << "PASSED: programmable VM " << name << std::endl;
}
