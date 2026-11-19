#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
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
    : SimObject<Probe>(ctx, name)
    , request(this)
    , response(this)
    , inject(this)
    , transmitted(this)
    , ack(this)
    , returned_ack(this) {
  }
  SimChannel<LsuReq> request;
  SimChannel<LsuRsp> response;
  SimChannel<ForzaMessage> inject;
  SimChannel<ForzaMessage> transmitted;
  SimChannel<ForzaAck> ack;
  SimChannel<ForzaAck> returned_ack;
};

struct Result {
  std::vector<uint64_t> values;
  std::vector<uint64_t> cycles;
};

class Fixture {
public:
  Fixture()
    : zone_(ForzaZone::Create("mmio-zone", 0))
    , probe_(Probe::Create("mmio-probe")) {
    probe_->request.bind(&zone_->mem_req_in.at(0));
    zone_->mem_rsp_out.at(0).bind(&probe_->response);
    probe_->inject.bind(&zone_->net_rx);
    zone_->net_tx.bind(&probe_->transmitted);
    zone_->ack_tx.bind(&probe_->ack);
    probe_->returned_ack.bind(&zone_->ack_rx);
    reset();
  }

  void reset() {
    SimPlatform::instance().reset();
    sent.clear();
    acks.clear();
    capture = true;
  }

  void tick() {
    SimPlatform::instance().tick();
    while (capture && !probe_->transmitted.empty()) {
      sent.push_back(probe_->transmitted.peek());
      probe_->transmitted.pop();
    }
    while (!probe_->ack.empty()) {
      acks.push_back(probe_->ack.peek());
      probe_->ack.pop();
    }
  }

  static void operand(LsuReq& request, uint32_t lane, uint64_t address, uint64_t value) {
    request.mask.set(lane);
    request.addrs[lane] = address;
    request.data[lane] = std::make_shared<mem_block_t>();
    request.data[lane]->fill(0);
    uint32_t offset = address & (VX_CFG_MEM_BLOCK_SIZE - 1);
    request.byteen[lane] = 0xffull << offset;
    for (uint32_t byte = 0; byte < 8; ++byte) {
      request.data[lane]->at(offset + byte) = value >> (byte * 8);
    }
  }

  Result exchange(LsuReq request) {
    request.tag = ++tag_;
    request.uuid = tag_ + 0x12340000;
    request.cid = 0x42;
    probe_->request.send(request, 1);
    Result result{std::vector<uint64_t>(VX_CFG_NUM_LSU_LANES, 0), {}};
    BitVector<> observed(VX_CFG_NUM_LSU_LANES);
    for (uint32_t cycle = 0; cycle < 256; ++cycle) {
      tick();
      if (probe_->response.empty()) {
        continue;
      }
      auto response = probe_->response.peek();
      probe_->response.pop();
      check(response.tag == request.tag && response.uuid == request.uuid
            && response.cid == request.cid, "response lost LSU identity");
#ifdef VX_CFG_FORZA_MMIO_VECTOR_ENABLE
      check(response.mask == request.mask, "vector command response omitted active lanes");
#else
      check(response.mask.count() == 1, "scalar command port returned multiple lanes in one cycle");
#endif
      for (uint32_t lane = 0; lane < response.mask.size(); ++lane) {
        if (!response.mask.test(lane)) {
          continue;
        }
        check(request.mask.test(lane) && !observed.test(lane), "duplicate or inactive response lane");
        check(bool(response.data[lane]), "missing response data");
        uint32_t offset = request.addrs[lane] & (VX_CFG_MEM_BLOCK_SIZE - 1);
        for (uint32_t byte = 0; byte < 8 && offset + byte < VX_CFG_MEM_BLOCK_SIZE; ++byte) {
          result.values[lane] |= uint64_t(response.data[lane]->at(offset + byte)) << (byte * 8);
        }
        observed.set(lane);
      }
      result.cycles.push_back(SimPlatform::instance().cycles());
      if (observed == request.mask) {
        return result;
      }
    }
    check(false, "MMIO command exceeded bounded completion time");
    return result;
  }

  uint64_t command(uint32_t actor, uint64_t address, uint64_t value = 0,
                   MemOp op = MemOp::AMO_SWAP) {
    if (address >= FORZA_MMIO_ENDPOINT_BASE
        && address < FORZA_MMIO_ENDPOINT_BASE + FORZA_MMIO_ACTOR_STRIDE) {
      address = forza_mmio_actor_address(actor, address);
    }
    LsuReq request(VX_CFG_NUM_LSU_LANES);
    request.op = op;
    request.wid = actor / VX_CFG_NUM_THREADS;
    uint32_t lane = actor % VX_CFG_NUM_THREADS;
    request.tids[lane] = lane;
    operand(request, lane, address, value);
    return exchange(request).values[lane];
  }

  void inject(uint32_t pe, uint32_t mailbox, uint64_t payload) {
    ForzaMessage message;
    message.source_zone = 1;
    message.source_actor = 7;
    message.dest_zone = 0;
    message.dest_pe = pe;
    message.mailbox = mailbox;
    message.sequence = ++sequence_;
    message.words[0] = forza_message_control(0, pe, mailbox, 0);
    message.words[1] = payload;
    probe_->inject.send(message, 1);
    tick();
  }

  void delivery(uint32_t count) {
    for (uint32_t cycle = 0; cycle < 256 && acks.size() < count; ++cycle) {
      tick();
    }
    check(acks.size() == count && !acks.back().error, "message did not reach registered slot");
  }

  void acknowledge(const ForzaMessage& message) {
    probe_->returned_ack.send(ForzaAck{message.source_zone, message.source_actor,
                                      message.mailbox, message.sequence, false}, 1);
    tick();
  }

  void release_one_transmit() {
    check(!probe_->transmitted.empty(), "TX sink had no packet to release");
    sent.push_back(probe_->transmitted.peek());
    probe_->transmitted.pop();
    tick();
  }

  void delayed_pop_contract() {
    reset();
    check(command(0, FORZA_MMIO_REGISTER, 255) == 0, "delayed-pop registration failed");
    inject(0, 0, 11);
    delivery(1);
    auto submit = [&](uint64_t address, MemOp operation) {
      LsuReq request(VX_CFG_NUM_LSU_LANES);
      request.op = operation;
      request.tag = ++tag_;
      request.uuid = tag_;
      operand(request, 0, address, 0);
      probe_->request.send(request, 1);
      return request.tag;
    };
    for (uint32_t index = 0; index < 2; ++index) {
      submit(FORZA_MMIO_RX_STATUS, MemOp::LD);
      for (uint32_t cycle = 0; cycle < 4; ++cycle) {
        tick();
      }
    }
    check(probe_->response.full(), "response sink did not fill");
    uint32_t pop_tag = submit(forza_mmio_pop_address(0), MemOp::AMO_SWAP);
    inject(0, 0, 22);
    inject(0, 0, 33);
    for (uint32_t cycle = 0; cycle < 20; ++cycle) {
      tick();
    }
    check(acks.size() == 2, "pop mutated its slot while response capacity was unavailable");
    probe_->response.pop();
    for (uint32_t cycle = 0; cycle < 20; ++cycle) {
      tick();
    }
    check(acks.size() == 3, "stalled pop did not resume when response capacity returned");
    probe_->response.pop();
    check(!probe_->response.empty(), "captured pop response missing");
    auto captured = probe_->response.peek();
    probe_->response.pop();
    check(captured.tag == pop_tag && captured.mask.count() == 1 && captured.mask.test(0),
          "delayed pop returned duplicate or incorrect response identity");
    check(command(0, forza_mmio_pop_address(0)) == 22, "stalled pop consumed more than one slot");
    check(command(0, forza_mmio_pop_address(0)) == 33, "resumed slot reuse changed queue order");
    uint64_t value = 0;
    for (uint32_t byte = 0; byte < 8; ++byte) {
      value |= uint64_t(captured.data[0]->at(byte)) << (8 * byte);
    }
    check(value == 11, "slot reuse changed a previously captured response payload");
    check(command(0, FORZA_MMIO_RX_STATUS, 0, MemOp::LD) == 0 && probe_->response.empty(),
          "delayed response produced a duplicate pop or response");
  }

#ifdef VX_CFG_FORZA_MMIO_VECTOR_ENABLE
  void vector_backpressure_contract() {
    reset();
    for (uint32_t actor = 0; actor < VX_CFG_NUM_LSU_LANES; ++actor) {
      check(command(actor, FORZA_MMIO_REGISTER, (actor << 8) | 255) == 0,
            "vector receiver registration failed");
      inject(actor, 0, 11 + actor);
    }
    delivery(VX_CFG_NUM_LSU_LANES);
    auto submit = [&](LsuReq request) {
      request.tag = ++tag_;
      request.uuid = tag_;
      probe_->request.send(request, 1);
      return request.tag;
    };
    LsuReq status(VX_CFG_NUM_LSU_LANES);
    status.op = MemOp::LD;
    operand(status, 0, FORZA_MMIO_RX_STATUS, 0);
    for (uint32_t index = 0; index < 2; ++index) {
      submit(status);
      for (uint32_t cycle = 0; cycle < 4; ++cycle) {
        tick();
      }
    }
    check(probe_->response.full(), "vector response sink did not fill");
    LsuReq pop(VX_CFG_NUM_LSU_LANES);
    pop.op = MemOp::AMO_SWAP;
    for (uint32_t lane = 0; lane < VX_CFG_NUM_LSU_LANES; ++lane) {
      pop.tids[lane] = lane;
      operand(pop, lane, forza_mmio_actor_address(lane, forza_mmio_pop_address(0)), 0);
    }
    uint32_t pop_tag = submit(pop);
    tick();
    LsuReq legacy(VX_CFG_NUM_LSU_LANES);
    legacy.op = MemOp::LD;
    operand(legacy, 0, FORZA_MAILBOX_BASE + 8, 0);
    uint32_t legacy_tag = submit(legacy);
    tick();
    for (uint32_t actor = 0; actor < VX_CFG_NUM_LSU_LANES; ++actor) {
      inject(actor, 0, 21 + actor);
    }
    delivery(VX_CFG_NUM_LSU_LANES * 2);
    for (uint32_t actor = 0; actor < VX_CFG_NUM_LSU_LANES; ++actor) {
      inject(actor, 0, 31 + actor);
    }
    for (uint32_t cycle = 0; cycle < 12; ++cycle) {
      tick();
    }
    check(acks.size() == VX_CFG_NUM_LSU_LANES * 2,
          "full response sink allowed vector pop side effects");
    probe_->response.pop();
    probe_->response.pop();
    std::vector<LsuRsp> responses;
    std::vector<uint64_t> response_cycles;
    for (uint32_t cycle = 0; cycle < 32 && responses.size() < 2; ++cycle) {
      tick();
      while (!probe_->response.empty()) {
        responses.push_back(probe_->response.peek());
        response_cycles.push_back(SimPlatform::instance().cycles());
        probe_->response.pop();
      }
    }
    check(responses.size() == 2 && responses[0].tag == pop_tag
          && responses[0].mask == pop.mask && responses[1].tag == legacy_tag,
          "vector/legacy response order or mask changed");
    check(response_cycles[1] > response_cycles[0],
          "vector service also processed the following legacy request in one cycle");
    delivery(VX_CFG_NUM_LSU_LANES * 3);
    for (uint32_t actor = 0; actor < VX_CFG_NUM_LSU_LANES; ++actor) {
      uint64_t value = 0;
      for (uint32_t byte = 0; byte < 8; ++byte) {
        value |= uint64_t(responses[0].data[actor]->at(byte)) << (byte * 8);
      }
      check(value == 11 + actor, "vector response changed after slot reuse");
      check(command(actor, forza_mmio_pop_address(0)) == 21 + actor,
            "vector pop consumed more than one slot per actor");
      check(command(actor, forza_mmio_pop_address(0)) == 31 + actor,
            "vector pop lost the next reused slot");
    }
  }
#endif

  bool capture = true;
  std::vector<ForzaMessage> sent;
  std::vector<ForzaAck> acks;

private:
  ForzaZone::Ptr zone_;
  Probe::Ptr probe_;
  uint64_t tag_ = 0;
  uint64_t sequence_ = 0;
};

void registration_and_lanes(Fixture& fixture) {
  fixture.reset();
  LsuReq request(VX_CFG_NUM_LSU_LANES);
  request.op = MemOp::AMO_SWAP;
  request.wid = 1;
  for (uint32_t lane = 0; lane < VX_CFG_NUM_LSU_LANES; ++lane) {
    request.tids[lane] = VX_CFG_NUM_LSU_LANES - lane - 1;
    uint32_t actor = request.wid * VX_CFG_NUM_THREADS + request.tids[lane];
    Fixture::operand(request, lane, forza_mmio_actor_address(actor, FORZA_MMIO_REGISTER),
                     ((100 + lane) << 8) | 255);
  }
  auto result = fixture.exchange(request);
  for (uint32_t lane = 0; lane < VX_CFG_NUM_LSU_LANES; ++lane) {
    check(result.values[lane] == FORZA_MMIO_ACCEPTED, "multi-lane registration rejected");
    check(fixture.command(VX_CFG_NUM_THREADS + request.tids[lane], FORZA_MMIO_REGISTER,
                          0, MemOp::LD) == (((100 + lane) << 8) | 255),
          "actor selection ignored wid/tids metadata");
#ifndef VX_CFG_FORZA_MMIO_VECTOR_ENABLE
    if (lane) {
      check(result.cycles[lane] == result.cycles[lane - 1] + 1,
            "single command port did not serialize active lanes");
    }
#endif
  }
#ifdef VX_CFG_FORZA_MMIO_VECTOR_ENABLE
  check(result.cycles.size() == 1, "vector command did not return one full-mask response");
#endif
  request.mask.reset();
  request.mask.set(1);
  request.mask.set(VX_CFG_NUM_LSU_LANES - 1);
  request.op = MemOp::LD;
  auto sparse = fixture.exchange(request);
#ifdef VX_CFG_FORZA_MMIO_VECTOR_ENABLE
  check(sparse.cycles.size() == 1, "sparse vector produced extra responses");
#else
  check(sparse.cycles.size() == request.mask.count(), "sparse mask generated ghost responses");
#endif
  request.mask.reset();
  request.mask.set(0);
  request.addrs[0] = forza_mmio_actor_address(0, FORZA_MMIO_RX_STATUS);
  check(fixture.exchange(request).values[0] == FORZA_MMIO_INVALID,
        "an endpoint encoded for another actor was accepted");
}

void payload_and_pop(Fixture& fixture) {
  fixture.reset();
  check(fixture.command(0, FORZA_MMIO_REGISTER, (23 << 8) | 255) == 0,
        "MMIO-only registration failed");
  for (uint32_t mailbox = 0; mailbox < 8; ++mailbox) {
    uint64_t value = mailbox == 0 ? 0 : (0xfedcba9876543210ull ^ mailbox);
    fixture.inject(23, mailbox, value);
    fixture.delivery(mailbox + 1);
    check(fixture.command(0, FORZA_MMIO_RX_STATUS, 0, MemOp::LD) == (1ull << mailbox),
          "receive status did not select registered actor/mailbox");
    check(fixture.command(0, forza_mmio_pop_address(mailbox)) == value,
          "atomic pop lost payload or used wrong block byte offset");
    check(fixture.command(0, FORZA_MMIO_RX_STATUS, 0, MemOp::LD) == 0,
          "pop did not release exactly one slot");
  }
  fixture.inject(23, 0, 11);
  fixture.inject(23, 0, 22);
  fixture.delivery(10);
  check(fixture.command(0, forza_mmio_pop_address(0)) == 11, "first pop reordered slots");
  fixture.inject(23, 0, 33);
  fixture.delivery(11);
  check(fixture.command(0, forza_mmio_pop_address(0)) == 22, "second pop reordered slots");
  check(fixture.command(0, forza_mmio_pop_address(0)) == 33, "slot reuse corrupted captured payload");
}

void transmit_and_backpressure(Fixture& fixture) {
  fixture.reset();
  for (uint32_t mailbox = 0; mailbox < 8; ++mailbox) {
    uint64_t value = forza_mmio_pack_update(123 + mailbox, -99 - int32_t(mailbox));
    check(fixture.command(0, forza_mmio_tx_address(1, 31, mailbox), value) == FORZA_MMIO_ACCEPTED,
          "8B atomic enqueue failed");
    for (uint32_t cycle = 0; cycle < 16 && fixture.sent.size() <= mailbox; ++cycle) {
      fixture.tick();
    }
    const auto& message = fixture.sent.at(mailbox);
    check(message.source_actor == 0 && message.source_zone == 0 && message.dest_zone == 1
          && message.dest_pe == 31 && message.mailbox == mailbox && message.words[1] == value,
          "address destination or payload decode failed");
    for (uint32_t word = 2; word < 8; ++word) {
      check(message.words[word] == 0, "8B enqueue retained stale unused payload");
    }
    fixture.acknowledge(message);
  }
  check(fixture.command(0, FORZA_MMIO_TX_COUNTS, 0, MemOp::LD) == 0,
        "delivery ACK failed to return credit");
  check(fixture.command(0, FORZA_MMIO_INVALID_ADDRESS, 7) == FORZA_MMIO_INVALID,
        "malformed command did not fail explicitly");
  check(fixture.command(0, forza_mmio_tx_address(15, 1, 0), 7) == FORZA_MMIO_INVALID,
        "uninstantiated destination zone was accepted");
  check(fixture.command(0, forza_mmio_tx_address(0, 0, 0), 7, MemOp::AMO_ADD)
        == FORZA_MMIO_INVALID, "unsupported AMO mutated device state");
  LsuReq narrow(VX_CFG_NUM_LSU_LANES);
  narrow.op = MemOp::AMO_SWAP;
  narrow.mask.set(0);
  narrow.addrs[0] = FORZA_MMIO_BASE + 60;
  narrow.byteen[0] = 0xfull << 60;
  narrow.data[0] = std::make_shared<mem_block_t>();
  narrow.data[0]->fill(0);
  check(fixture.exchange(narrow).values[0] == FORZA_MMIO_INVALID,
        "AMO.W at the final line word did not return INVALID safely");
  narrow.addrs[0] = FORZA_MMIO_BASE + 8;
  narrow.byteen[0] = 0xfull << 8;
  check(fixture.exchange(narrow).values[0] == FORZA_MMIO_INVALID,
        "narrow AMO at an 8B-aligned endpoint was accepted");
  narrow.op = MemOp::LD;
  narrow.addrs[0] = FORZA_MMIO_BASE + 63;
  check(fixture.exchange(narrow).values[0] == FORZA_MMIO_INVALID,
        "byte load at the line boundary did not return INVALID safely");

  fixture.reset();
  check(fixture.command(0, FORZA_MMIO_REGISTER, 255) == 0, "receiver registration failed");
  fixture.capture = false;
  uint32_t accepted = 0;
  while (accepted < 255) {
    uint64_t status = fixture.command(0, forza_mmio_tx_address(1, 0, 0), accepted + 1);
    if (status == FORZA_MMIO_BUSY) {
      break;
    }
    check(status == FORZA_MMIO_ACCEPTED, "valid queue-fill command failed");
    ++accepted;
  }
  check(accepted >= 64 && accepted < 255, "TX queue did not apply finite backpressure");
  fixture.release_one_transmit();
  LsuReq partial(VX_CFG_NUM_LSU_LANES);
  partial.op = MemOp::AMO_SWAP;
  for (uint32_t lane = 0; lane < VX_CFG_NUM_LSU_LANES; ++lane) {
    partial.tids[lane] = lane;
    Fixture::operand(partial, lane, forza_mmio_tx_address(1, 0, 0), accepted + lane + 1);
  }
  auto partial_result = fixture.exchange(partial);
  for (uint32_t lane = 0; lane < VX_CFG_NUM_LSU_LANES; ++lane) {
    check(partial_result.values[lane] == (lane == 0 ? FORZA_MMIO_ACCEPTED : FORZA_MMIO_BUSY),
          "partially accepted TX vector lost lane status or exceeded FIFO capacity");
  }
  ++accepted;
  fixture.inject(0, 0, 0x123456789abcdef0ull);
  fixture.delivery(1);
  check(fixture.command(0, forza_mmio_pop_address(0)) == 0x123456789abcdef0ull,
        "BUSY sender blocked receive progress");
  check(fixture.command(0, FORZA_MMIO_TX_COUNTS, 0, MemOp::LD) == accepted,
        "BUSY attempt consumed a credit");
  fixture.capture = true;
  for (uint32_t cycle = 0; cycle < 256 && fixture.sent.size() < accepted; ++cycle) {
    fixture.tick();
  }
  check(fixture.sent.size() == accepted, "backpressure lost accepted packets");
  for (uint32_t index = 0; index < fixture.sent.size(); ++index) {
    check(fixture.sent[index].words[1] == index + 1, "TX FIFO reordered accepted packets");
    fixture.acknowledge(fixture.sent[index]);
  }
  check(fixture.command(0, FORZA_MMIO_TX_COUNTS, 0, MemOp::LD) == 0,
        "ACK drain retained stale outstanding messages");
}

void mixed_sram_mmio_load(Fixture& fixture) {
  fixture.reset();
  check(fixture.command(0, FORZA_MMIO_REGISTER, 255) == 0, "mixed-load actor0 registration failed");
  check(fixture.command(1, FORZA_MMIO_REGISTER, (1 << 8) | 255) == 0,
        "mixed-load actor1 registration failed");
  fixture.inject(0, 0, 0x1122334455667788ull);
  fixture.inject(1, 0, 0x8877665544332211ull);
  fixture.delivery(2);
  LsuReq request(VX_CFG_NUM_LSU_LANES);
  request.op = MemOp::LD;
  Fixture::operand(request, 0, FORZA_MAILBOX_BASE + 8, 0);
  Fixture::operand(request, 1, forza_mmio_actor_address(1, FORZA_MMIO_RX_STATUS), 0);
  request.tids[1] = 1;
  auto result = fixture.exchange(request);
  check(result.values[0] == 0x1122334455667788ull && result.values[1] == 1,
        "mixed SRAM/MMIO packet lost lane-specific data");
  check(fixture.command(1, forza_mmio_pop_address(0)) == 0x8877665544332211ull,
        "fragment cursor was not reset before the next request");
  check(fixture.command(0, forza_mmio_pop_address(0)) == 0x1122334455667788ull,
        "non-destructive mixed SRAM load released a slot");
}

}

int main() {
  check(SimPlatform::instance().initialize(), "simulation platform initialization failed");
  check(VX_CFG_NUM_THREADS == VX_CFG_NUM_LSU_LANES && VX_CFG_NUM_LSU_LANES >= 2
        && VX_CFG_NUM_WARPS >= 2 && VX_CFG_NUM_CORES >= 2,
        "fixture requires two cores/warps and full-width LSU lanes");
  Fixture fixture;
  registration_and_lanes(fixture);
  payload_and_pop(fixture);
  transmit_and_backpressure(fixture);
  mixed_sram_mmio_load(fixture);
  fixture.delayed_pop_contract();
#ifdef VX_CFG_FORZA_MMIO_VECTOR_ENABLE
  fixture.vector_backpressure_contract();
#endif
  fixture.reset();
#ifdef VX_CFG_FORZA_MMIO_VECTOR_ENABLE
  std::cout << "PASSED: vector MMIO full-mask responses, partial acceptance and no double service; ";
#else
  std::cout << "PASSED: scalar MMIO lane fragments; ";
#endif
  std::cout << "identity, 8B enqueue/pop, zero payload, "
               "slot reuse, mailbox offsets, bounded BUSY progress, ACK credits and invalid commands"
            << std::endl;
  return 0;
}
