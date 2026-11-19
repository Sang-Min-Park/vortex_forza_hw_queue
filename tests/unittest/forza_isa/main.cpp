#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <vector>
#include "forza/forza_zone.h"

using namespace vortex;

namespace {
void check(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << std::endl;
    std::exit(1);
  }
}

class Probe : public SimObject<Probe> {
public:
  Probe(const SimContext& ctx, const char* name)
    : SimObject<Probe>(ctx, name), request(this), response(this), inject(this),
      transmitted(this, 1), ack(this), returned_ack(this), mmio_request(this),
      mmio_response(this) {}
  SimChannel<ForzaCsrRequest> request;
  SimChannel<ForzaCsrRequest> response;
  SimChannel<ForzaMessage> inject;
  SimChannel<ForzaMessage> transmitted;
  SimChannel<ForzaAck> ack;
  SimChannel<ForzaAck> returned_ack;
  SimChannel<LsuReq> mmio_request;
  SimChannel<LsuRsp> mmio_response;
};

class Fixture {
public:
  Fixture() : zone_(ForzaZone::Create("isa-zone", 0)),
              probe_(Probe::Create("isa-probe")) {
    probe_->request.bind(&zone_->csr_req_in);
    zone_->csr_rsp_out.bind(&probe_->response);
    probe_->inject.bind(&zone_->net_rx);
    zone_->net_tx.bind(&probe_->transmitted);
    zone_->ack_tx.bind(&probe_->ack);
    probe_->returned_ack.bind(&zone_->ack_rx);
    probe_->mmio_request.bind(&zone_->mem_req_in[0]);
    zone_->mem_rsp_out[0].bind(&probe_->mmio_response);
    reset();
  }
  void reset() {
    SimPlatform::instance().reset();
    sent.clear();
    acknowledgments.clear();
    completed.clear();
    completion_cycles.clear();
    mmio_responses.clear();
    capture = true;
  }
  void tick(uint32_t count = 1) {
    for (uint32_t i = 0; i < count; ++i) {
      SimPlatform::instance().tick();
      if (capture && !probe_->transmitted.empty()) {
        sent.push_back(probe_->transmitted.peek());
        probe_->transmitted.pop();
      }
      if (!probe_->ack.empty()) {
        acknowledgments.push_back(probe_->ack.peek());
        probe_->ack.pop();
      }
      if (!probe_->response.empty()) {
        auto response = probe_->response.peek();
        probe_->response.pop();
        check(response.block == 0 && response.trace->resume_warp,
              "completion lost block or warp resume");
        completed.push_back(response.trace);
        completion_cycles[response.trace] = SimPlatform::instance().cycles();
      }
      if (!probe_->mmio_response.empty()) {
        mmio_responses.emplace_back(SimPlatform::instance().cycles(),
                                    probe_->mmio_response.peek());
        probe_->mmio_response.pop();
      }
    }
  }
  instr_trace_t* submit(ForzaType op, uint32_t wid, ThreadMask mask,
                        const std::array<uint64_t, VX_CFG_NUM_THREADS>& first,
                        const std::array<uint64_t, VX_CFG_NUM_THREADS>& second = {}) {
    auto* trace = new instr_trace_t(++sequence_);
    owned_.emplace_back(trace);
    trace->wid = wid;
    trace->tmask = mask;
    trace->fu_type = FUType::SFU;
    trace->op_type = op;
    trace->fetch_stall = true;
    trace->resume_warp = false;
    for (uint32_t lane = 0; lane < VX_CFG_NUM_THREADS; ++lane) {
      trace->src_data[0][lane].i = first[lane];
      trace->src_data[1][lane].i = second[lane];
    }
    probe_->request.send(ForzaCsrRequest{trace, 0}, 1);
    return trace;
  }
  bool done(instr_trace_t* trace) const {
    return std::find(completed.begin(), completed.end(), trace) != completed.end();
  }
  uint64_t command(ForzaType op, uint32_t actor, uint64_t first = 0,
                    uint64_t second = 0) {
    ThreadMask mask(VX_CFG_NUM_THREADS);
    uint32_t lane = actor % VX_CFG_NUM_THREADS;
    mask.set(lane);
    std::array<uint64_t, VX_CFG_NUM_THREADS> a{}, b{};
    a[lane] = first;
    b[lane] = second;
    auto* trace = submit(op, actor / VX_CFG_NUM_THREADS, mask, a, b);
    for (uint32_t i = 0; i < 256 && !done(trace); ++i) tick();
    check(done(trace), "bounded instruction failed to complete");
    return trace->dst_data[lane].i;
  }
  void inject(uint32_t pe, uint32_t mailbox, uint64_t token) {
    ForzaMessage message;
    message.source_zone = 0;
    message.source_actor = 0;
    message.dest_zone = 0;
    message.dest_pe = pe;
    message.mailbox = mailbox;
    message.sequence = ++sequence_;
    message.words[0] = forza_message_control(0, pe, mailbox, 0);
    message.words[1] = token;
    probe_->inject.send(message, 1);
  }
  void loopback(uint32_t index) { probe_->inject.send(sent.at(index), 1); }
  void ack(uint32_t index) { probe_->returned_ack.send(acknowledgments.at(index), 1); }
  void mmio_send(uint32_t wid, uint64_t first_token) {
    LsuReq request(VX_CFG_NUM_LSU_LANES);
    request.op=MemOp::AMO_SWAP;
    request.wid=wid;
    request.tag=++sequence_;
    request.uuid=sequence_;
    for(uint32_t lane=0;lane<VX_CFG_NUM_LSU_LANES;++lane) {
      request.mask.set(lane);
      request.tids[lane]=lane;
      request.addrs[lane]=FORZA_MMIO_BASE;
      request.byteen[lane]=255;
      request.data[lane]=std::make_shared<mem_block_t>();
      request.data[lane]->fill(0);
      for(uint32_t b=0;b<8;++b)request.data[lane]->at(b)=(first_token+lane)>>(b*8);
    }
    probe_->mmio_request.send(request,1);
  }
  bool capture = true;
  std::vector<ForzaMessage> sent;
  std::vector<ForzaAck> acknowledgments;
  std::vector<instr_trace_t*> completed;
  std::map<instr_trace_t*,uint64_t> completion_cycles;
  std::vector<std::pair<uint64_t,LsuRsp>> mmio_responses;
private:
  ForzaZone::Ptr zone_;
  Probe::Ptr probe_;
  uint64_t sequence_ = 0;
  std::vector<std::unique_ptr<instr_trace_t>> owned_;
};

void contract(Fixture& f) {
  check(f.command(ForzaType::REGISTER, 0, (17 << 8) | 3) == 0, "register failed");
  check(f.command(ForzaType::RECV_TRY, 0, 0) == 0, "empty receive changed");
  check(f.command(ForzaType::SEND_TRY, 0, forza_isa_route(0, 17, 0),
                  UINT64_C(0xfffffffedcba9876)) == 0, "send failed");
  check(f.sent.size() == 1 && f.sent[0].words[1] == UINT64_C(0xfffffffedcba9876),
        "send lost payload or duplicated");
  check((f.command(ForzaType::CREDITS, 0) & 255) == 1, "credit not reserved");
  f.loopback(0);
  f.tick(16);
  check(f.acknowledgments.size() == 1 && !f.acknowledgments[0].error,
        "slot did not become READY");
  check(f.command(ForzaType::RECV_TRY, 0, 0) == UINT64_C(0xfffffffedcba9876),
        "receive changed payload");
  check(f.command(ForzaType::RECV_TRY, 0, 0) == 0, "receive did not release");
  f.ack(0);
  f.tick(8);
  check((f.command(ForzaType::CREDITS, 0) & 255) == 0, "credit did not recover");
  check(f.command(ForzaType::SEND_TRY, 0, UINT64_MAX, 1) == 2,
        "malformed route accepted");
  check(f.command(ForzaType::SEND_TRY, 0, forza_isa_route(0,17,0), 0) == 2,
        "reserved zero token accepted");
  check(f.command(ForzaType::STATUS, 0) == (UINT64_C(1) << 63) + (UINT64_C(255) << 17),
        "successful protocol left an error");
}

void receive_wait(Fixture& f) {
  f.reset();
  check(f.command(ForzaType::REGISTER, 0, 1) == 0, "register0 failed");
  check(f.command(ForzaType::REGISTER, 1, (1 << 8) | 1) == 0, "register1 failed");
  ThreadMask mask(VX_CFG_NUM_THREADS);
  mask.set(0); mask.set(1);
  std::array<uint64_t,VX_CFG_NUM_THREADS> args{};
  auto* wait = f.submit(ForzaType::RECV_WAIT_ANY, 0, mask, args);
  f.tick(12);
  check(!f.done(wait), "wait-any returned while empty");
  f.inject(1,0,123);
  f.tick(16);
  check(f.done(wait) && wait->dst_data[0].i == 0 && wait->dst_data[1].i == 123,
        "wait-any did not select available lane");
  auto* all = f.submit(ForzaType::RECV_WAIT, 0, mask, args);
  f.inject(0,0,456);
  f.tick(16);
  check(!f.done(all), "wait-all completed before every active endpoint");
  auto other = f.command(ForzaType::REGISTER, VX_CFG_NUM_THREADS,
                        (VX_CFG_NUM_THREADS << 8) | 1);
  check(other == 0, "parked warp starved another warp's setup");
  f.inject(1,0,789);
  f.tick(16);
  check(f.done(all) && all->dst_data[0].i == 456 && all->dst_data[1].i == 789,
        "wait-all lost partial progress");
  f.inject(0,0,321);
  f.tick(8);
  check(f.command(ForzaType::RECV_WAIT,0,0)==321,
        "preexisting arrival caused a lost wakeup");
}

void send_wait(Fixture& f) {
  f.reset();
  check(f.command(ForzaType::REGISTER,0,1)==0,"register failed");
  f.capture=false;
  uint32_t accepted=0;
  for(uint32_t i=0;i<100;++i) {
    uint64_t r=f.command(ForzaType::SEND_TRY,0,forza_isa_route(0,0,0),i+1);
    if(r==1)break;
    check(r==0,"flood invalid");++accepted;
  }
  check(accepted>=64 && accepted<100,"bounded TX queue did not backpressure");
  ThreadMask mask(VX_CFG_NUM_THREADS);mask.set(0);
  std::array<uint64_t,VX_CFG_NUM_THREADS> route{}, token{};
  route[0]=forza_isa_route(0,0,0);token[0]=UINT64_C(0x1122334455667788);
  auto* blocked=f.submit(ForzaType::SEND_WAIT,0,mask,route,token);
  f.tick(12);
  check(!f.done(blocked),"send-wait bypassed full TX queue");
  check(f.command(ForzaType::REGISTER,VX_CFG_NUM_THREADS,
                  (VX_CFG_NUM_THREADS<<8)|1)==0,"blocked send starved other warp");
  f.inject(VX_CFG_NUM_THREADS,0,91);
  f.tick(8);
  check(f.command(ForzaType::RECV_TRY,VX_CFG_NUM_THREADS,0)==91,
        "blocked send prevented receiver progress");
  f.capture=true;
  f.tick(200);
  check(f.done(blocked),"send-wait failed to wake on available credit");
  uint32_t copies=0;
  for(auto&m:f.sent)copies+=m.words[1]==token[0];
  check(copies==1,"send-wait duplicated or lost its token");
}

void credit_wait(Fixture& f) {
  f.reset();
  check(f.command(ForzaType::REGISTER,0,1)==0,"register failed");
  for(uint32_t i=0;i<255;++i) {
    check(f.command(ForzaType::SEND_TRY,0,forza_isa_route(0,0,0),i+1)==0,
          "credit flood rejected before255");
  }
  check(f.command(ForzaType::SEND_TRY,0,forza_isa_route(0,0,0),256)==1,
        "credit limit did not backpressure");
  ThreadMask mask(VX_CFG_NUM_THREADS);mask.set(0);
  std::array<uint64_t,VX_CFG_NUM_THREADS> route{},token{};
  token[0]=257;
  auto* pending=f.submit(ForzaType::SEND_WAIT,0,mask,route,token);
  f.tick(12);
  check(!f.done(pending),"send-wait ignored outstanding limit");
  f.loopback(0);
  f.tick(16);
  check(!f.done(pending),"READY alone reclaimed sender credit before ACK");
  f.ack(0);
  f.tick(16);
  check(f.done(pending),"returned credit failed to wake sender");
  check((f.command(ForzaType::CREDITS,0)&255)==255,
        "reused credit counted incorrectly");
  uint32_t copies=0;for(auto&m:f.sent)copies+=m.words[1]==257;
  check(copies==1,"credit-woken instruction was replayed");
}

void shared_command_service(Fixture& f) {
  f.reset();
  ThreadMask mask(VX_CFG_NUM_THREADS);
  for(uint32_t lane=0;lane<VX_CFG_NUM_THREADS;++lane)mask.set(lane);
  std::array<uint64_t,VX_CFG_NUM_THREADS> route{},token{};
  for(uint32_t lane=0;lane<VX_CFG_NUM_THREADS;++lane)token[lane]=lane+1;
  auto* instruction=f.submit(ForzaType::SEND_TRY,0,mask,route,token);
  f.mmio_send(1,101);
  for(uint32_t i=0;i<64 && (!f.done(instruction)||f.mmio_responses.empty());++i)f.tick();
  check(f.done(instruction)&&!f.mmio_responses.empty(),"mixed command service stalled");
  check(f.completion_cycles[instruction]!=f.mmio_responses.front().first,
        "ISA and MMIO exceeded shared full-width command budget");
  f.tick(16);
  check(f.sent.size()==VX_CFG_NUM_THREADS+VX_CFG_NUM_LSU_LANES,
        "mixed command arbitration lost messages");
}
}

int main() {
  static_assert(VX_CFG_NUM_THREADS>=2 && VX_CFG_NUM_WARPS>=2,"two independent warps required");
  Fixture f;
  contract(f);
  receive_wait(f);
  send_wait(f);
  credit_wait(f);
  shared_command_service(f);
  std::cout << "PASSED: SEND/RECV try/wait, wake-any, masked lanes, bounded queues, credit recovery and other-warp progress" << std::endl;
}
