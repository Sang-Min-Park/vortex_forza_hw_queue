#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
#include "forza/forza_zone.h"
#include "mem/local_mem.h"

using namespace vortex;

namespace {
void check(bool value, const char* message) {
  if (!value) {
    std::cerr << "FAILED: " << message << std::endl;
    std::exit(1);
  }
}
struct Control { uint32_t response_mode = 1; bool consume_ack = true; };
struct AckEvent { uint64_t cycle; ForzaAck ack; };

class Probe : public SimObject<Probe> {
public:
  Probe(const SimContext& ctx, const char* name)
    : SimObject<Probe>(ctx, name), mmio_request(this), mmio_response(this)
    , cpu_request(this), cpu_response(this), network_in(this), network_out(this)
    , ack_in(this), ack_out(this), memory_response(this), memory_forward(this), control(this) {
  }
  SimChannel<LsuReq> mmio_request;
  SimChannel<LsuRsp> mmio_response;
  SimChannel<MemReq> cpu_request;
  SimChannel<MemRsp> cpu_response;
  SimChannel<ForzaMessage> network_in;
  SimChannel<ForzaMessage> network_out;
  SimChannel<ForzaAck> ack_in;
  SimChannel<ForzaAck> ack_out;
  SimChannel<MemRsp> memory_response;
  SimChannel<MemRsp> memory_forward;
  SimChannel<Control> control;
  std::vector<AckEvent> acks;
  std::map<uint64_t, uint64_t> response_cycles;
protected:
  void on_reset() {
    mode_ = Control{};
    acks.clear();
    response_cycles.clear();
  }
  void on_tick() {
    if (!control.empty()) {
      mode_ = control.peek();
      control.pop();
    }
    if (!network_in.empty() && !network_out.full()) {
      network_out.send(network_in.peek(), 1);
      network_in.pop();
    }
    if (mode_.response_mode && !memory_response.empty() && !memory_forward.full()) {
      auto response = memory_response.peek();
      response_cycles[response.uuid >> 4] = SimPlatform::instance().cycles();
      if (mode_.response_mode == 2) {
        response.uuid ^= 1;
      } else if (mode_.response_mode == 3) {
        response.hart_id ^= 1;
      }
      memory_forward.send(response, 1);
      memory_response.pop();
    }
    if (mode_.consume_ack && !ack_in.empty() && !ack_out.full()) {
      acks.push_back(AckEvent{SimPlatform::instance().cycles(), ack_in.peek()});
      ack_out.send(ack_in.peek(), 1);
      ack_in.pop();
    }
  }
private:
  friend class SimObject<Probe>;
  Control mode_;
};

uint64_t unpack(const std::shared_ptr<mem_block_t>& data, uint32_t offset) {
  check(bool(data), "response has no payload");
  uint64_t value = 0;
  for (uint32_t byte = 0; byte < 8; ++byte) {
    value |= uint64_t(data->at(offset + byte)) << (byte * 8);
  }
  return value;
}

class Fixture {
public:
  Fixture()
    : zone_(ForzaZone::Create("update-zone", 0))
    , memory_(LocalMem::Create("real-localmem", LocalMem::Config{
        FORZA_UPDATE_ZONE_STRIDE, 8, 2, 2, false}))
    , probe_(Probe::Create("update-probe")) {
    probe_->mmio_request.bind(&zone_->mem_req_in.at(0));
    zone_->mem_rsp_out.at(0).bind(&probe_->mmio_response);
    zone_->net_tx.bind(&probe_->network_in);
    probe_->network_out.bind(&zone_->net_rx);
    zone_->ack_tx.bind(&probe_->ack_in);
    probe_->ack_out.bind(&zone_->ack_rx);
    probe_->cpu_request.bind(&memory_->Inputs.at(0));
    memory_->Outputs.at(0).bind(&probe_->cpu_response);
    zone_->update_req_out.bind(&memory_->Inputs.at(1));
    memory_->Outputs.at(1).bind(&probe_->memory_response);
    probe_->memory_forward.bind(&zone_->update_rsp_in);
    reset();
  }
  void reset() { SimPlatform::instance().reset(); }
  void tick(uint32_t count = 1) {
    for (uint32_t i = 0; i < count; ++i) {
      SimPlatform::instance().tick();
    }
  }
  void control(uint32_t response_mode, bool consume_ack = true) {
    probe_->control.send(Control{response_mode, consume_ack}, 1);
    tick(2);
  }
  uint64_t mmio(uint64_t address, uint64_t operand = 0, MemOp op = MemOp::AMO_SWAP) {
    LsuReq request(VX_CFG_NUM_LSU_LANES);
    request.op = op;
    request.mask.set(0);
    request.addrs[0] = address;
    request.tag = ++tag_;
    request.uuid = tag_;
    request.data[0] = std::make_shared<mem_block_t>();
    request.data[0]->fill(0);
    uint32_t offset = address & (VX_CFG_MEM_BLOCK_SIZE - 1);
    request.byteen[0] = 0xffull << offset;
    for (uint32_t byte = 0; byte < 8; ++byte) {
      request.data[0]->at(offset + byte) = operand >> (byte * 8);
    }
    probe_->mmio_request.send(request, 1);
    for (uint32_t cycle = 0; cycle < 256; ++cycle) {
      tick();
      if (!probe_->mmio_response.empty()) {
        auto response = probe_->mmio_response.peek();
        probe_->mmio_response.pop();
        check(response.tag == request.tag && response.uuid == request.uuid
              && response.mask == request.mask, "MMIO response identity changed");
        return unpack(response.data[0], offset);
      }
    }
    check(false, "MMIO command failed to complete while backpressured");
    return 0;
  }
  uint64_t cpu(uint32_t word, MemOp op, uint64_t operand = 0) {
    MemReq request;
    request.op = op;
    request.addr = VX_MEM_LMEM_BASE_ADDR + uint64_t(word) * 8;
    request.tag = ++tag_;
    request.uuid = tag_;
    request.hart_id = 0;
    request.flags.local = true;
    uint32_t offset = request.addr & (VX_CFG_MEM_BLOCK_SIZE - 1);
    if (op != MemOp::LD) {
      request.data = std::make_shared<mem_block_t>();
      request.data->fill(0);
      request.byteen = 0xffull << offset;
      for (uint32_t byte = 0; byte < 8; ++byte) {
        request.data->at(offset + byte) = operand >> (byte * 8);
      }
    }
    probe_->cpu_request.send(request, 1);
    for (uint32_t cycle = 0; cycle < 1024; ++cycle) {
      tick();
      if (!probe_->cpu_response.empty()) {
        auto response = probe_->cpu_response.peek();
        probe_->cpu_response.pop();
        check(response.tag == request.tag && response.uuid == request.uuid
              && response.hart_id == request.hart_id, "CPU LocalMem identity changed");
        return unpack(response.data, offset);
      }
    }
    check(false, "CPU access to actual LocalMem did not complete");
    return 0;
  }
  uint64_t outstanding() { return mmio(FORZA_MMIO_TX_COUNTS, 0, MemOp::LD) & 255; }
  void drain() {
    for (uint32_t cycle = 0; cycle < 10000; ++cycle) {
      if (outstanding() == 0) {
        return;
      }
      tick();
    }
    check(false, "UPDATE did not drain after downstream capacity returned");
  }
  void ack_contract() const {
    for (const auto& event : probe_->acks) {
      auto response = probe_->response_cycles.find(event.ack.sequence);
      check(!event.ack.error && event.ack.actor == 0 && event.ack.mailbox == FORZA_UPDATE_MAILBOX,
            "UPDATE ACK lost source identity or reported an error");
      check(response != probe_->response_cycles.end() && event.cycle > response->second,
            "ACK preceded its actual LocalMem AMO response");
    }
  }
  size_t acks() const { return probe_->acks.size(); }
private:
  ForzaZone::Ptr zone_;
  LocalMem::Ptr memory_;
  Probe::Ptr probe_;
  uint32_t tag_ = 0;
};

void normal_contract(Fixture& f) {
  const uint32_t words[] = {0, 1, 7, FORZA_UPDATE_WORDS_PER_ZONE - 1};
  const uint64_t initial[] = {10, 0x8000000000000000ull, UINT64_MAX, 0};
  const uint64_t delta[] = {uint64_t(-7), 9, 2, 0xfedcba9876543210ull};
  for (uint32_t i = 0; i < 4; ++i) {
    f.cpu(words[i], MemOp::AMO_SWAP, initial[i]);
  }
  for (uint32_t i = 0; i < 4; ++i) {
    check(f.mmio(forza_update_address(0, words[i]), delta[i]) == FORZA_MMIO_ACCEPTED,
          "valid UPDATE enqueue rejected");
  }
  f.drain();
  for (uint32_t i = 0; i < 4; ++i) {
    check(f.cpu(words[i], MemOp::LD) == initial[i] + delta[i],
          "real LocalMem signed/full64 update result mismatch");
  }
  check(f.acks() == 4, "accepted UPDATE count did not equal completion ACK count");
  check(f.mmio(FORZA_MMIO_RX_STATUS, 0, MemOp::LD) == 0,
        "UPDATE unexpectedly occupied a software mailbox");
  check(f.mmio(FORZA_UPDATE_INVALID_ADDRESS, 1) == FORZA_MMIO_INVALID,
        "reserved UPDATE address was accepted");
  check(f.mmio(forza_update_address(15, 0), 1) == FORZA_MMIO_INVALID,
        "uninstantiated UPDATE zone was accepted");
  f.ack_contract();

  f.reset();
  f.cpu(3, MemOp::AMO_SWAP, 10);
  f.control(0);
  check(f.mmio(forza_update_address(0, 3), uint64_t(-7)) == FORZA_MMIO_ACCEPTED,
        "withheld-response request rejected");
  f.tick(64);
  check(f.outstanding() == 1 && f.acks() == 0,
        "UPDATE sent ACK without receiving the LocalMem response");
  check(f.cpu(3, MemOp::LD) == 3, "UPDATE did not use the actual LocalMem bank");
  f.control(1);
  f.drain();
  check(f.acks() == 1, "withheld response did not resume exactly once");
  f.ack_contract();

  f.reset();
  f.cpu(0, MemOp::AMO_SWAP, 0);
  f.control(1, false);
  uint32_t accepted = 0;
  for (; accepted < 512; ++accepted) {
    uint64_t result = f.mmio(forza_update_address(0, 0), 1);
    if (result == FORZA_MMIO_BUSY) {
      break;
    }
    check(result == FORZA_MMIO_ACCEPTED, "valid bounded UPDATE failed");
  }
  check(accepted >= FORZA_UPDATE_QUEUE_DEPTH && accepted < 255,
        "UPDATE queue/inflight backpressure was not finite");
  check(f.outstanding() == accepted && f.acks() == 0, "BUSY changed source credit");
  f.control(1, true);
  f.drain();
  check(f.acks() == accepted && f.cpu(0, MemOp::LD) == accepted,
        "UPDATE lost or duplicated work while the ACK sink was full");
  f.ack_contract();
}
}

int main(int argc, char** argv) {
  check(SimPlatform::instance().initialize(), "simulation platform initialization failed");
  Fixture fixture;
  if (argc > 1) {
    uint32_t fault = std::string(argv[1]) == "--bad-uuid" ? 2 : 3;
    fixture.cpu(0, MemOp::AMO_SWAP, 0);
    fixture.control(fault);
    try {
      check(fixture.mmio(forza_update_address(0, 0), 1) == FORZA_MMIO_ACCEPTED,
            "identity test enqueue failed");
      fixture.tick(128);
    } catch (const std::runtime_error& error) {
      check(std::string(error.what()).find("identity mismatch") != std::string::npos
            && fixture.acks() == 0, "wrong memory identity caused an unexpected failure/ACK");
      std::cout << "PASSED: matching-tag wrong LocalMem "
                << (fault == 2 ? "uuid" : "hart") << " rejected before ACK/release" << std::endl;
      return 0;
    }
    check(false, "wrong LocalMem response identity was accepted");
  }
  normal_contract(fixture);
  std::cout << "PASSED: UPDATE uses real banked LocalMem AMO, signed/full64/bounds, "
               "completion-only ACK, finite queue/inflight/ACK backpressure and exact credit recovery"
            << std::endl;
  return 0;
}
