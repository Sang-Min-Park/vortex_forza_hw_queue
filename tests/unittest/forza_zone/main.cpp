#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

#include "forza/forza_zone.h"
#include "instr.h"
#include "instr_trace.h"

using namespace vortex;

namespace {

[[noreturn]] void fail(const char* message) {
  std::cerr << "FAILED: " << message << std::endl;
  std::exit(1);
}

void check(bool condition, const char* message) {
  if (!condition) {
    fail(message);
  }
}

class ZoneProbe : public SimObject<ZoneProbe> {
public:
  ZoneProbe(const SimContext& ctx, const char* name)
    : SimObject<ZoneProbe>(ctx, name)
    , csr_request(this)
    , csr_response(this)
    , network_injection(this)
    , network_output(this)
    , ack_output(this)
    , ack_return(this)
    , memory_request(this)
    , memory_response(this) {
  }

  SimChannel<ForzaCsrRequest> csr_request;
  SimChannel<ForzaCsrRequest> csr_response;
  SimChannel<ForzaMessage> network_injection;
  SimChannel<ForzaMessage> network_output;
  SimChannel<ForzaAck> ack_output;
  SimChannel<ForzaAck> ack_return;
  SimChannel<LsuReq> memory_request;
  SimChannel<LsuRsp> memory_response;
};

class Fixture {
public:
  Fixture()
    : zone_(ForzaZone::Create("zone", 0))
    , probe_(ZoneProbe::Create("probe")) {
    probe_->csr_request.bind(&zone_->csr_req_in);
    zone_->csr_rsp_out.bind(&probe_->csr_response);
    probe_->network_injection.bind(&zone_->net_rx);
    zone_->net_tx.bind(&probe_->network_output);
    zone_->ack_tx.bind(&probe_->ack_output);
    probe_->ack_return.bind(&zone_->ack_rx);
    probe_->memory_request.bind(&zone_->mem_req_in.at(0));
    zone_->mem_rsp_out.at(0).bind(&probe_->memory_response);
    SimPlatform::instance().reset();
  }

  void tick() {
    SimPlatform::instance().tick();
    while (!probe_->ack_output.empty()) {
      acks.push_back(probe_->ack_output.peek());
      probe_->ack_output.pop();
    }
    while (capture_transmit && !probe_->network_output.empty()) {
      transmitted.push_back(probe_->network_output.peek());
      probe_->network_output.pop();
    }
  }

  void capture_one_transmit() {
    check(!probe_->network_output.empty(), "no packet available to release sink capacity");
    transmitted.push_back(probe_->network_output.peek());
    probe_->network_output.pop();
  }

  std::vector<reg_data_t> csr_packet(uint32_t wid, uint32_t address, CsrType operation,
                                     const ThreadMask& tmask, const std::vector<uint64_t>& sources) {
    auto instruction = std::make_shared<Instr>(++uuid_, FUType::SFU);
    instruction->set_op_type(operation);
    instruction->set_args(IntrCsrArgs{0, 0, address});
    instr_trace_t trace(uuid_);
    trace.instr_ptr = instruction;
    trace.fu_type = FUType::SFU;
    trace.op_type = operation;
    trace.wid = wid;
    trace.tmask = tmask;
    for (uint32_t tid = 0; tid < VX_CFG_NUM_THREADS; ++tid) {
      trace.src_data[0][tid].u64 = sources.at(tid);
    }
    trace.fetch_stall = true;
    probe_->csr_request.send(ForzaCsrRequest{&trace, 0}, 1);
    for (uint32_t cycle = 0; cycle < 128; ++cycle) {
      tick();
      if (!probe_->csr_response.empty()) {
        auto response = probe_->csr_response.peek();
        probe_->csr_response.pop();
        check(response.trace == &trace && response.block == 0,
              "CSR response did not retain its instruction identity");
        check(trace.resume_warp, "CSR completion did not release its parked warp");
        return trace.dst_data;
      }
    }
    std::cerr << "warp=" << wid << ", CSR=0x" << std::hex << address << std::dec << std::endl;
    fail("CSR request did not complete within the cycle bound");
  }

  uint64_t csr(uint32_t actor, uint32_t address,
               CsrType operation = CsrType::CSRRS, uint64_t source = 0) {
    uint32_t tid = actor % VX_CFG_NUM_THREADS;
    ThreadMask tmask(VX_CFG_NUM_THREADS);
    tmask.set(tid);
    std::vector<uint64_t> sources(VX_CFG_NUM_THREADS, 0);
    sources[tid] = source;
    return csr_packet(actor / VX_CFG_NUM_THREADS, address, operation, tmask, sources).at(tid).u64;
  }

  std::unique_ptr<instr_trace_t> submit_csr(uint32_t actor, uint32_t address,
                                           CsrType operation, uint64_t source) {
    auto instruction = std::make_shared<Instr>(++uuid_, FUType::SFU);
    instruction->set_op_type(operation);
    instruction->set_args(IntrCsrArgs{0, 0, address});
    auto trace = std::make_unique<instr_trace_t>(uuid_);
    trace->instr_ptr = instruction;
    trace->fu_type = FUType::SFU;
    trace->op_type = operation;
    trace->wid = actor / VX_CFG_NUM_THREADS;
    trace->tmask = ThreadMask(VX_CFG_NUM_THREADS);
    trace->tmask.set(actor % VX_CFG_NUM_THREADS);
    trace->src_data[0][actor % VX_CFG_NUM_THREADS].u64 = source;
    trace->fetch_stall = true;
    probe_->csr_request.send(ForzaCsrRequest{trace.get(), 0}, 1);
    return trace;
  }

  bool csr_response_pending() const {
    return !probe_->csr_response.empty();
  }

  bool receive_csr(instr_trace_t* trace) {
    if (probe_->csr_response.empty()) {
      return false;
    }
    auto response = probe_->csr_response.peek();
    probe_->csr_response.pop();
    check(response.trace == trace && response.block == 0,
          "asynchronous CSR completed the wrong instruction");
    check(trace->resume_warp, "asynchronous CSR did not release its parked warp");
    return true;
  }

  void inject(const ForzaMessage& packet, uint32_t delay = 1) {
    probe_->network_injection.send(packet, delay);
  }

  void return_ack(const ForzaAck& ack) {
    probe_->ack_return.send(ack, 1);
  }

  void wait_for_acks(uint32_t count) {
    for (uint32_t cycle = 0; cycle < 128 && acks.size() < count; ++cycle) {
      tick();
    }
    check(acks.size() == count, "delivery acknowledgement did not arrive");
  }

  void wait_for_transmit(uint32_t count) {
    for (uint32_t cycle = 0; cycle < 128 && transmitted.size() < count; ++cycle) {
      tick();
    }
    check(transmitted.size() == count, "assembled CSR message did not transmit");
  }

  std::array<uint64_t, 8> read_message(uint64_t pointer) {
    std::array<uint64_t, 8> words{};
    for (uint32_t first = 0; first < words.size(); first += VX_CFG_NUM_LSU_LANES) {
      LsuReq request(VX_CFG_NUM_LSU_LANES);
      request.op = MemOp::LD;
      request.tag = ++memory_tag_;
      request.uuid = ++uuid_;
      for (uint32_t lane = 0; lane < request.mask.size() && first + lane < words.size(); ++lane) {
        request.mask.set(lane);
        request.addrs[lane] = pointer + (first + lane) * 8;
      }
      probe_->memory_request.send(request, 1);
      bool completed = false;
      for (uint32_t cycle = 0; cycle < 128; ++cycle) {
        tick();
        if (probe_->memory_response.empty()) {
          continue;
        }
        auto response = probe_->memory_response.peek();
        probe_->memory_response.pop();
        check(response.tag == request.tag && response.uuid == request.uuid
              && response.cid == request.cid && response.mask == request.mask,
              "SRAM response did not retain lane mask and request identity");
        for (uint32_t lane = 0; lane < response.mask.size(); ++lane) {
          if (!response.mask.test(lane)) {
            continue;
          }
          check(bool(response.data[lane]), "SRAM load returned no data block");
          uint32_t offset = request.addrs[lane] & (VX_CFG_MEM_BLOCK_SIZE - 1);
          for (uint32_t byte = 0; byte < 8; ++byte) {
            words[first + lane] |= uint64_t(response.data[lane]->at(offset + byte)) << (8 * byte);
          }
        }
        completed = true;
        break;
      }
      check(completed, "mailbox SRAM load did not complete within the cycle bound");
    }
    return words;
  }

  std::vector<ForzaAck> acks;
  std::vector<ForzaMessage> transmitted;
  bool capture_transmit = true;

private:
  ForzaZone::Ptr zone_;
  ZoneProbe::Ptr probe_;
  uint64_t uuid_ = 0;
  uint32_t memory_tag_ = 0;
};

ForzaMessage incoming(uint32_t logical_pe, uint64_t sequence) {
  ForzaMessage packet;
  packet.source_zone = 1;
  packet.source_actor = 5;
  packet.dest_zone = 0;
  packet.dest_pe = logical_pe;
  packet.mailbox = 0;
  packet.sequence = sequence;
  packet.words[0] = forza_message_control(0, logical_pe, 0, 0x31) | (sequence << 48);
  for (uint32_t word = 1; word < packet.words.size(); ++word) {
    packet.words[word] = 0xfedcba9876543200ull ^ (sequence << 8) ^ word;
  }
  return packet;
}

void check_ack(const ForzaAck& ack, const ForzaMessage& message) {
  check(ack.dest_zone == message.source_zone && ack.actor == message.source_actor
        && ack.mailbox == message.mailbox && ack.sequence == message.sequence && !ack.error,
        "delivery acknowledgement did not preserve sender identity");
}

}

int main() {
  check(SimPlatform::instance().initialize(), "simulation platform did not initialize");
  check(VX_CFG_NUM_THREADS >= 2 && VX_CFG_NUM_WARPS >= 2,
        "zone fixture requires at least two lanes and two warps");
  Fixture fixture;
  constexpr uint64_t REG0 = (7u << 8) | 1;
  constexpr uint64_t REG1 = (6u << 8) | 1;
  ThreadMask registration_mask(VX_CFG_NUM_THREADS);
  registration_mask.set(0);
  registration_mask.set(1);
  std::vector<uint64_t> registrations(VX_CFG_NUM_THREADS, 0);
  registrations[0] = REG0;
  registrations[1] = REG1;
  fixture.csr_packet(0, FORZA_CSR_ZQMMBOXREG, CsrType::CSRRW, registration_mask, registrations);
  check(fixture.csr(0, FORZA_CSR_ZQMMBOXREG) == REG0, "actor zero registration changed");
  check(fixture.csr(1, FORZA_CSR_ZQMMBOXREG) == REG1, "actor one registration changed");

  auto first = incoming(7, 0x111);
  auto second = incoming(7, 0x112);
  fixture.inject(first);
  fixture.inject(second, 2);
  fixture.wait_for_acks(2);
  check_ack(fixture.acks[0], first);
  check_ack(fixture.acks[1], second);
  check(fixture.csr(0, FORZA_CSR_ZQMSTAT) == 1, "ready mailbox was not reported");
  check(fixture.csr(1, FORZA_CSR_ZQMSTAT) == 0, "logical PE seven routed to physical actor one");
  uint64_t first_pointer = fixture.csr(0, FORZA_CSR_ZQMDQ0);
  check(first_pointer == FORZA_MAILBOX_BASE, "logical PE mapping changed mailbox SRAM ownership");
  check(fixture.read_message(first_pointer) == first.words, "first slot control or payload is corrupt");
  fixture.csr(0, FORZA_CSR_ZQMDQ0, CsrType::CSRRW, 0);
  check(fixture.csr(0, FORZA_CSR_ZQMSTAT) == 1, "second ready slot disappeared after release");
  uint64_t second_pointer = fixture.csr(0, FORZA_CSR_ZQMDQ0);
  check(second_pointer == first_pointer + FORZA_MESSAGE_BYTES, "release did not alternate mailbox slots");
  check(fixture.read_message(second_pointer) == second.words, "second slot control or payload is corrupt");
  fixture.csr(0, FORZA_CSR_ZQMDQ0, CsrType::CSRRW, 0);
  check(fixture.csr(0, FORZA_CSR_ZQMSTAT) == 0 && fixture.csr(0, FORZA_CSR_ZQMDQ0) == 0,
        "released mailbox remained visible");

  auto third = incoming(6, 0x113);
  fixture.inject(third);
  fixture.wait_for_acks(3);
  check_ack(fixture.acks[2], third);
  check(fixture.csr(1, FORZA_CSR_ZQMSTAT) == 1 && fixture.csr(0, FORZA_CSR_ZQMSTAT) == 0,
        "logical PE six did not route exclusively to physical actor one");
  uint64_t actor_one_pointer = fixture.csr(1, FORZA_CSR_ZQMDQ0);
  check(actor_one_pointer == FORZA_MAILBOX_BASE + 1024, "actor mailbox SRAM ranges overlap");
  check(fixture.read_message(actor_one_pointer) == third.words, "actor one payload is corrupt");
  fixture.csr(1, FORZA_CSR_ZQMDQ0, CsrType::CSRRW, 0);

  std::array<uint64_t, 8> assembled{};
  assembled[0] = forza_message_control(0, 7, 0, 0x52);
  for (uint32_t word = 1; word < assembled.size(); ++word) {
    assembled[word] = 0x1234567800000000ull + word;
    fixture.csr(1, FORZA_CSR_ZENEQD, CsrType::CSRRW, assembled[word]);
  }
  fixture.csr(1, FORZA_CSR_ZENEQC, CsrType::CSRRW, assembled[0]);
  fixture.wait_for_transmit(1);
  auto transmitted = fixture.transmitted[0];
  check(transmitted.source_zone == 0 && transmitted.source_actor == 1
        && transmitted.dest_zone == 0 && transmitted.dest_pe == 7 && transmitted.mailbox == 0
        && transmitted.words == assembled, "CSR sender assembled incorrect message metadata or payload");
  check(fixture.csr(1, FORZA_CSR_ZENCOUNT) == 1, "accepted send did not consume one sequence credit");
  fixture.inject(transmitted);
  fixture.wait_for_acks(4);
  check_ack(fixture.acks[3], transmitted);
  fixture.return_ack(fixture.acks[3]);
  check(fixture.csr(1, FORZA_CSR_ZENCOUNT) == 0, "delivery ACK did not return the sender credit");
  uint64_t pointer = fixture.csr(0, FORZA_CSR_ZQMDQ0);
  check(fixture.read_message(pointer) == assembled, "assembled sender message did not survive SRAM delivery");
  fixture.csr(0, FORZA_CSR_ZQMDQ0, CsrType::CSRRW, 0);
  check(fixture.csr(0, FORZA_CSR_ZQMSTAT) == 0, "assembled message was not released");

  const uint64_t credit_control = forza_message_control(0, 7, 0, 0x53);
  for (uint32_t message = 0; message < 255; ++message) {
    fixture.csr(0, FORZA_CSR_ZENEQC, CsrType::CSRRW, credit_control);
    fixture.wait_for_transmit(message + 2);
    check(fixture.transmitted.back().source_actor == 0,
          "credit saturation changed the sender identity");
    for (uint32_t word = 1; word < 8; ++word) {
      check(fixture.transmitted.back().words[word] == 0,
            "short payload was not zero filled");
    }
  }
  check(fixture.csr(0, FORZA_CSR_ZENCOUNT) == 255,
        "outstanding counter did not retain all 255 unacknowledged sends");
  check((fixture.csr(0, FORZA_CSR_ZENSTAT) & 1) != 0,
        "exhausted mailbox credits did not assert busy");

  auto blocked = fixture.submit_csr(0, FORZA_CSR_ZENEQC, CsrType::CSRRW, credit_control);
  for (uint32_t cycle = 0; cycle < 32; ++cycle) {
    fixture.tick();
  }
  check(!fixture.csr_response_pending() && fixture.transmitted.size() == 256,
        "256th unacknowledged send completed without a free credit");
  check(fixture.csr(VX_CFG_NUM_THREADS, FORZA_CSR_ZQMSTAT) == 0,
        "blocked producer prevented another warp from reading receive status");
  const auto& oldest = fixture.transmitted[1];
  fixture.return_ack(ForzaAck{oldest.source_zone, oldest.source_actor, oldest.mailbox,
                              oldest.sequence, false});
  bool unblocked = false;
  for (uint32_t cycle = 0; cycle < 128; ++cycle) {
    fixture.tick();
    if (fixture.receive_csr(blocked.get())) {
      unblocked = true;
      break;
    }
  }
  check(unblocked, "delivery ACK did not resume the credit-blocked commit");
  fixture.wait_for_transmit(257);
  for (uint32_t cycle = 0; cycle < 32; ++cycle) {
    fixture.tick();
  }
  check(fixture.transmitted.size() == 257 && !fixture.csr_response_pending(),
        "resumed commit duplicated a message or CSR completion");
  check(fixture.csr(0, FORZA_CSR_ZENCOUNT) == 255,
        "ACK and resumed commit did not conserve outstanding credits");
  check((fixture.csr(0, FORZA_CSR_ZENSTAT) & (0x1ffull << 8)) == 0,
        "valid credit backpressure reported a sender error");

  for (uint32_t message = 2; message < fixture.transmitted.size(); ++message) {
    const auto& packet = fixture.transmitted[message];
    fixture.return_ack(ForzaAck{packet.source_zone, packet.source_actor, packet.mailbox,
                                packet.sequence, false});
    fixture.tick();
    fixture.tick();
  }
  check(fixture.csr(0, FORZA_CSR_ZENCOUNT) == 0,
        "final delivery acknowledgements leaked sequence credits");

  uint32_t try_base = fixture.transmitted.size();
  for (uint32_t message = 0; message < 255; ++message) {
    check(fixture.csr(0, FORZA_CSR_ZENTRY, CsrType::CSRRW, credit_control)
            == FORZA_TRY_ACCEPTED, "nonblocking commit rejected available credit");
    fixture.wait_for_transmit(try_base + message + 1);
  }
  for (uint32_t word = 0; word < FORZA_PAYLOAD_WORDS; ++word) {
    std::vector<uint64_t> values(VX_CFG_NUM_THREADS, 0);
    values[0] = 0xaaaa0000ull + word;
    values[1] = 0xbbbb0000ull + word;
    fixture.csr_packet(0, FORZA_CSR_ZENEQD, CsrType::CSRRW, registration_mask, values);
  }
  std::vector<uint64_t> controls(VX_CFG_NUM_THREADS, credit_control);
  auto results = fixture.csr_packet(0, FORZA_CSR_ZENTRY, CsrType::CSRRW,
                                    registration_mask, controls);
  check(results[0].u64 == FORZA_TRY_BUSY && results[1].u64 == FORZA_TRY_ACCEPTED,
        "one busy lane stalled or changed another lane's nonblocking commit");
  fixture.wait_for_transmit(try_base + 256);
  check(fixture.csr(0, FORZA_CSR_ZENCOUNT) == 255
        && fixture.csr(1, FORZA_CSR_ZENCOUNT) == 1,
        "mixed-lane nonblocking commit miscounted accepted messages");
  for (uint32_t word = 1; word < 8; ++word) {
    check(fixture.transmitted.back().words[word] == 0xbbbb0000ull + word - 1,
          "mixed-lane commit corrupted the accepted lane's payload");
  }
  for (uint32_t word = 0; word < FORZA_PAYLOAD_WORDS; ++word) {
    fixture.csr(0, FORZA_CSR_ZENEQD, CsrType::CSRRW, 0xcccc0000ull + word);
  }
  check(fixture.csr(0, FORZA_CSR_ZENTRY, CsrType::CSRRW, credit_control) == FORZA_TRY_BUSY,
        "busy retry did not return immediately");
  check((fixture.csr(0, FORZA_CSR_ZENSTAT) & (0x1ffull << 8)) == 0,
        "busy retry retained stale assembly or raised a write error");
  const auto first_try = fixture.transmitted[try_base];
  fixture.return_ack(ForzaAck{first_try.source_zone, first_try.source_actor,
                              first_try.mailbox, first_try.sequence, false});
  fixture.tick();
  fixture.tick();
  for (uint32_t word = 0; word < FORZA_PAYLOAD_WORDS; ++word) {
    fixture.csr(0, FORZA_CSR_ZENEQD, CsrType::CSRRW, 0xdddd0000ull + word);
  }
  check(fixture.csr(0, FORZA_CSR_ZENTRY, CsrType::CSRRW, credit_control)
          == FORZA_TRY_ACCEPTED, "nonblocking commit did not reuse a returned credit");
  fixture.wait_for_transmit(try_base + 257);
  for (uint32_t word = 1; word < 8; ++word) {
    check(fixture.transmitted.back().words[word] == 0xdddd0000ull + word - 1,
          "retry transmitted stale assembly instead of the new payload");
  }
  for (uint32_t message = try_base + 1; message < fixture.transmitted.size(); ++message) {
    const auto& packet = fixture.transmitted[message];
    fixture.return_ack(ForzaAck{packet.source_zone, packet.source_actor, packet.mailbox,
                                packet.sequence, false});
    fixture.tick();
    fixture.tick();
  }
  check(fixture.csr(0, FORZA_CSR_ZENCOUNT) == 0
        && fixture.csr(1, FORZA_CSR_ZENCOUNT) == 0,
        "nonblocking commit leaked outstanding credits");

  uint32_t fifo_base = fixture.transmitted.size();
  fixture.capture_transmit = false;
  // Two packets fit in the probe endpoint and 64 in the zone's TX FIFO.
  for (uint32_t message = 0; message < 66; ++message) {
    check(fixture.csr(0, FORZA_CSR_ZENTRY, CsrType::CSRRW, credit_control)
            == FORZA_TRY_ACCEPTED, "TX path exhausted before its bounded capacity");
  }
  check(fixture.csr(0, FORZA_CSR_ZENTRY, CsrType::CSRRW, credit_control) == FORZA_TRY_BUSY,
        "nonblocking commit overbooked a full TX FIFO");
  fixture.capture_one_transmit();
  fixture.tick();
  fixture.tick();
  results = fixture.csr_packet(0, FORZA_CSR_ZENTRY, CsrType::CSRRW,
                               registration_mask, controls);
  check(results[0].u64 == FORZA_TRY_ACCEPTED && results[1].u64 == FORZA_TRY_BUSY,
        "two lanes did not atomically arbitrate the last TX FIFO entry");
  check(fixture.csr(0, FORZA_CSR_ZENCOUNT) == 67
        && fixture.csr(1, FORZA_CSR_ZENCOUNT) == 0,
        "a rejected FIFO-full attempt consumed a credit");
  fixture.capture_transmit = true;
  fixture.wait_for_transmit(fifo_base + 67);
  for (uint32_t message = fifo_base; message < fixture.transmitted.size(); ++message) {
    const auto& packet = fixture.transmitted[message];
    fixture.return_ack(ForzaAck{packet.source_zone, packet.source_actor, packet.mailbox,
                                packet.sequence, false});
    fixture.tick();
    fixture.tick();
  }
  check(fixture.csr(0, FORZA_CSR_ZENCOUNT) == 0,
        "FIFO-full nonblocking attempts leaked credits or messages");
  check(fixture.csr(2, FORZA_CSR_ZENTRY, CsrType::CSRRW, credit_control | (1ull << 19))
          == FORZA_TRY_INVALID, "nonblocking commit accepted an invalid destination");
  check(SimPlatform::instance().idle(), "zone fixture retained undelivered channel traffic");
  std::cout << "PASSED: FORZA zone CSR identity, SRAM slots, payloads, delivery ACKs, "
               "255-credit backpressure, independent warp progress and per-lane try-commit"
            << std::endl;
  return 0;
}
