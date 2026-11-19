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

  void inject(uint32_t pe, uint32_t mailbox, const std::array<uint64_t, 7>& payload) {
    ForzaMessage message;
    message.source_zone = 1;
    message.source_actor = 7;
    message.dest_zone = 0;
    message.dest_pe = pe;
    message.mailbox = mailbox;
    message.sequence = ++sequence_;
    message.words[0] = forza_message_control(0, pe, mailbox, 0x11);
    std::copy(payload.begin(), payload.end(), message.words.begin() + 1);
    probe_->inject.send(message, 1);
    tick();
  }
  void delivery(uint32_t count) {
    for (uint32_t cycle = 0; cycle < 256 && acks.size() < count; ++cycle) tick();
    check(acks.size() == count && !acks.back().error, "wide message did not reach registered slot");
  }
  std::vector<ForzaMessage> sent;
  std::vector<ForzaAck> acks;
  bool capture = true;
private:
  ForzaZone::Ptr zone_;
  Probe::Ptr probe_;
  uint32_t tag_ = 0;
  uint64_t sequence_ = 0;
};

void copy_release(Fixture& f, uint32_t actor, uint32_t mailbox, uint32_t pe,
                  uint32_t slot, const std::array<uint64_t, 7>& payload, uint32_t words) {
  const uint64_t pointer = f.command(actor, forza_mmio_peek_address(mailbox), 0, MemOp::LD);
  check(pointer == FORZA_MAILBOX_BASE + actor * 1024 + mailbox * 128 + slot * 64,
        "generic PEEK did not return this actor's current slot");
  check(f.command(actor, forza_mmio_peek_address(mailbox), 0, MemOp::LD) == pointer,
        "generic PEEK destructively consumed a message");
  uint64_t control = f.command(actor, pointer, 0, MemOp::LD);
  check(control == forza_message_control(0, pe, mailbox, 0x11), "wide control word corrupted");
  uint64_t token = control;
  for (uint32_t word = 0; word < words; ++word) {
    uint64_t value = f.command(actor, pointer + 8 + word * 8, 0, MemOp::LD);
    check(value == payload[word], "wide payload word corrupted");
    token ^= value;
  }
  check(f.command(actor, forza_mmio_release_address(mailbox), token) == FORZA_MMIO_ACCEPTED,
        "generic wide slot RELEASE failed");
}
}

int main() {
#ifdef VX_CFG_FORZA_UPDATE_ENABLE
#error "wide generic contract must not include UPDATE"
#endif
#ifdef VX_CFG_FORZA_RECORD_ENABLE
#error "wide generic contract must not include RECORD"
#endif
  check(SimPlatform::instance().initialize(), "simulation platform initialization failed");
  Fixture f;
  check(forza_mmio_peek_address(8) == FORZA_MMIO_INVALID_ADDRESS
        && forza_mmio_release_address(8) == FORZA_MMIO_INVALID_ADDRESS,
        "wide mailbox helper accepted invalid mailbox");
  check(f.command(0, FORZA_MMIO_REGISTER, (19u << 8) | 255) == FORZA_MMIO_ACCEPTED,
        "generic receiver registration failed");
  check(f.command(1, FORZA_MMIO_REGISTER, (20u << 8) | 255) == FORZA_MMIO_ACCEPTED,
        "second actor registration failed");
  const std::array<uint64_t, 7> a{{0, UINT64_MAX, 0x8000000000000000ull, 0x1122334455667788ull,
                                 0x123456789abcdef0ull, 42, 0xfeedfacecafebeefull}};
  const std::array<uint64_t, 7> b{{0xfedcba9876543210ull, 1, 2, 3, 4, 5, 6}};
  for (uint32_t mailbox = 0; mailbox < 8; ++mailbox) {
    check(f.command(0, forza_mmio_peek_address(mailbox), 0, MemOp::LD) == 0,
          "empty generic PEEK returned an address");
    check(f.command(0, forza_mmio_release_address(mailbox), 0) == FORZA_MMIO_INVALID,
          "empty generic RELEASE succeeded");
    check(f.command(0, forza_mmio_peek_address(mailbox), 0) == FORZA_MMIO_INVALID,
          "generic PEEK accepted a write");
    check(f.command(0, forza_mmio_release_address(mailbox), 0, MemOp::LD) == FORZA_MMIO_INVALID,
          "generic RELEASE accepted a read");
    f.inject(19, mailbox, a); f.delivery(mailbox * 3 + 1);
    f.inject(19, mailbox, b); f.delivery(mailbox * 3 + 2);
    check(f.command(1, forza_mmio_peek_address(mailbox), 0, MemOp::LD) == 0,
          "different actor observed another actor's message");
    LsuReq wrong_actor(VX_CFG_NUM_LSU_LANES);
    wrong_actor.op = MemOp::LD;
    wrong_actor.tids[1] = 1;
    Fixture::operand(wrong_actor, 1, forza_mmio_peek_address(mailbox), 0);
    check(f.exchange(wrong_actor).values[1] == FORZA_MMIO_INVALID,
          "generic PEEK accepted another physical actor's endpoint address");
    copy_release(f, 0, mailbox, 19, 0, a, 4);
    copy_release(f, 0, mailbox, 19, 1, b, 7);
    f.inject(19, mailbox, a); f.delivery(mailbox * 3 + 3);
    copy_release(f, 0, mailbox, 19, 0, a, 7);
    check(f.command(0, forza_mmio_peek_address(mailbox), 0, MemOp::LD) == 0,
          "generic RELEASE did not empty and advance current slot");
  }
  f.inject(20, 1, b); f.delivery(25);
  copy_release(f, 1, 1, 20, 0, b, 7);
  check(f.sent.empty(), "receive-only generic interface unexpectedly emitted a request");
  std::cout << "PASSED: generic wide MMIO PEEK/RELEASE with UPDATE=OFF RECORD=OFF, "
               "eight mailboxes, two actors,4/7word copy,zero/highbits,slot reuse and invalid operations"
            << std::endl;
}
