#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
#include "forza/forza_zone.h"
#include "forza/forza_fabric.h"
#include "mem/local_mem.h"

using namespace vortex;

namespace {
void check(bool value, const char* message) {
  if (!value) {
    std::cerr << "FAILED: " << message << std::endl;
    std::exit(1);
  }
}
struct Control { uint32_t response_mode = 1; bool consume_ack = true; bool forward_reply = true; };
struct MemoryEvent { uint64_t cycle; MemReq request; };
struct AckEvent { uint64_t cycle; ForzaAck ack; };

class Probe : public SimObject<Probe> {
public:
  Probe(const SimContext& ctx, const char* name)
    : SimObject<Probe>(ctx, name), mmio_request(this), mmio_response(this)
    , cpu_request(this), cpu_response(this), network_in(this), network_out(this)
    , ack_in(this), ack_out(this), reply_in(this), reply_out(this), memory_request(this), memory_send(this), memory_response(this), memory_forward(this), control(this) {
  }
  SimChannel<LsuReq> mmio_request;
  SimChannel<LsuRsp> mmio_response;
  SimChannel<MemReq> cpu_request;
  SimChannel<MemRsp> cpu_response;
  SimChannel<ForzaMessage> network_in;
  SimChannel<ForzaMessage> network_out;
  SimChannel<ForzaAck> ack_in;
  SimChannel<ForzaAck> ack_out;
  SimChannel<ForzaMessage> reply_in;
  SimChannel<ForzaMessage> reply_out;
  SimChannel<MemReq> memory_request;
  SimChannel<MemReq> memory_send;
  SimChannel<MemRsp> memory_response;
  SimChannel<MemRsp> memory_forward;
  SimChannel<Control> control;
  std::vector<AckEvent> acks;
  std::vector<MemoryEvent> requests;
  std::map<uint64_t, uint64_t> response_cycles;
protected:
  void on_reset() {
    mode_ = Control{};
    acks.clear();
    requests.clear();
    response_cycles.clear();
  }
  void on_tick() {
    if (!control.empty()) {
      mode_ = control.peek();
      control.pop();
    }
    if (!network_in.empty() && !network_out.full()
        && (mode_.forward_reply || network_in.peek().source_actor != VX_CFG_NUM_WARPS * VX_CFG_NUM_THREADS)) {
      network_out.send(network_in.peek(), 1);
      network_in.pop();
    }
    if (mode_.forward_reply && !reply_in.empty() && !reply_out.full()) {
      reply_out.send(reply_in.peek(), 1);
      reply_in.pop();
    }
    if (!memory_request.empty() && !memory_send.full()) {
      requests.push_back(MemoryEvent{SimPlatform::instance().cycles(), memory_request.peek()});
      memory_send.send(memory_request.peek(), 1);
      memory_request.pop();
    }
    if (mode_.response_mode && !memory_response.empty() && !memory_forward.full()
        && (mode_.response_mode != 4 || memory_response.peek().data)) {
      auto response = memory_response.peek();
      response_cycles[response.uuid] = SimPlatform::instance().cycles();
      if (mode_.response_mode == 2) {
        response.uuid ^= 1;
      } else if (mode_.response_mode == 3) {
        response.hart_id ^= 1;
      } else if (mode_.response_mode == 5) {
        response.tag += 6ull * (1ull << 32);
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
  Fixture(uint32_t core = 0, const ForzaFabric::Ptr& fabric = nullptr)
    : zone_(ForzaZone::Create("record-zone", core))
    , memory_(LocalMem::Create("real-localmem", LocalMem::Config{
        FORZA_UPDATE_ZONE_STRIDE, 8, 2, 2, false}))
    , probe_(Probe::Create("update-probe")) {
    probe_->mmio_request.bind(&zone_->mem_req_in.at(0));
    zone_->mem_rsp_out.at(0).bind(&probe_->mmio_response);
    zone_->ack_tx.bind(&probe_->ack_in);
    if (fabric) {
      zone_->net_tx.bind(&fabric->msg_in.at(core));
      fabric->msg_out.at(core).bind(&zone_->net_rx);
      zone_->reply_net_tx.bind(&fabric->record_reply_in.at(core));
      fabric->record_reply_out.at(core).bind(&zone_->reply_net_rx);
      probe_->ack_out.bind(&fabric->ack_in.at(core));
      fabric->ack_out.at(core).bind(&zone_->ack_rx);
    } else {
      zone_->net_tx.bind(&probe_->network_in);
      probe_->network_out.bind(&zone_->net_rx);
      probe_->ack_out.bind(&zone_->ack_rx);
      zone_->reply_net_tx.bind(&probe_->reply_in);
      probe_->reply_out.bind(&zone_->reply_net_rx);
    }
    probe_->cpu_request.bind(&memory_->Inputs.at(0));
    memory_->Outputs.at(0).bind(&probe_->cpu_response);
    zone_->update_req_out.bind(&probe_->memory_request);
    probe_->memory_send.bind(&memory_->Inputs.at(1));
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
  void control(uint32_t response_mode, bool consume_ack = true, bool forward_reply = true) {
    probe_->control.send(Control{response_mode, consume_ack, forward_reply}, 1);
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
    std::cerr << "CPU timeout word=" << word << " op=" << op << " tag=" << tag_ << std::endl;
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
  void memory_contract(uint32_t transactions) const {
    check(probe_->requests.size() == transactions * 6, "transaction did not issue exactly six 8B SRAM accesses");
    std::map<uint64_t, uint32_t> operations;
    for (const auto& event : probe_->requests) {
      const auto& request = event.request;
      check(__builtin_popcountll(request.byteen) == 8, "transaction accessed more than one bank word");
      uint32_t operation = request.uuid & 15;
      check(operation < 6 && ((operation < 3 && request.op == MemOp::LD)
            || (operation >= 3 && request.op == MemOp::ST && request.flags.strsp)),
            "transaction skipped real reads or acknowledged stores");
      operations[request.uuid >> 8] |= 1u << operation;
    }
    check(operations.size() == transactions, "memory operations lost transaction identity");
    for (const auto& item : operations) check(item.second == 63, "transaction memory field coverage incomplete");
  }
  void register_source() {
    check(mmio(FORZA_MMIO_REGISTER, (7u << 8) | 2) == FORZA_MMIO_ACCEPTED,
          "mailbox1 registration failed");
  }
  std::vector<uint64_t> receive() {
    uint64_t address = mmio(forza_record_peek_address(1), 0, MemOp::LD);
    if (!address) return {};
    std::vector<uint64_t> words(5);
    uint64_t token = 0;
    for (uint32_t i = 0; i < words.size(); ++i) {
      words[i] = mmio(address + i * 8, 0, MemOp::LD);
      token ^= words[i];
    }
    check(((words[0] >> FORZA_CONTROL_OPCODE_SHIFT) & 255) == FORZA_RECORD_REPLY_OPCODE
          && (words[0] & 0x7ff) == 7, "reply opcode or captured return PE is wrong");
    check(mmio(forza_record_release_address(1), token) == FORZA_MMIO_ACCEPTED,
          "wide reply release failed");
    return words;
  }
  std::vector<uint64_t> receive_direct() {
    uint64_t id = mmio(FORZA_RECORD_CQ_ID, 0, MemOp::LD);
    if (!id) return {};
    std::vector<uint64_t> words{0, id - 1,
        mmio(FORZA_RECORD_CQ_COUNT, 0, MemOp::LD),
        mmio(FORZA_RECORD_CQ_SUM, 0, MemOp::LD),
        mmio(FORZA_RECORD_CQ_XOR, 0, MemOp::LD)};
    check(mmio(FORZA_RECORD_CQ_ID, 0, MemOp::LD) == id,
          "direct completion front changed before release");
    uint64_t token = id ^ words[2] ^ words[3] ^ words[4];
    check(mmio(FORZA_RECORD_CQ_RELEASE, token) == FORZA_MMIO_ACCEPTED,
          "direct completion release failed");
    return words;
  }
  std::vector<std::vector<uint64_t>> collect(uint32_t count, bool direct = false) {
    std::vector<std::vector<uint64_t>> values;
    for (uint32_t cycle = 0; cycle < 30000 && values.size() < count; ++cycle) {
      auto result = direct ? receive_direct() : receive();
      if (!result.empty()) values.push_back(result);
      tick();
    }
    check(values.size() == count, "record replies did not drain exactly");
    drain();
    return values;
  }
  size_t acks() const { return probe_->acks.size(); }
private:
  ForzaZone::Ptr zone_;
  LocalMem::Ptr memory_;
  Probe::Ptr probe_;
  uint32_t tag_ = 0;
};

void normal_contract(Fixture& f) {
  check(f.mmio(forza_record_address(0, 0), forza_record_pack(1, 1)) == FORZA_MMIO_INVALID,
        "record accepted without reply mailbox registration");
  f.register_source();
  check(f.mmio(FORZA_RECORD_INVALID_ADDRESS, 0) == FORZA_MMIO_INVALID,
        "reserved record address accepted");
  check(f.mmio(forza_record_address(15, 0), 0) == FORZA_MMIO_INVALID,
        "uninstantiated record zone accepted");
  check(f.mmio(forza_record_release_address(1), 0) == FORZA_MMIO_INVALID,
        "empty reply release succeeded");
  const uint32_t records[] = {0, 1, 2, FORZA_RECORDS_PER_ZONE - 1};
  const uint64_t initial[] = {0, UINT64_MAX, 0x8000000000000000ull, 12};
  for (uint32_t index = 0; index < 4; ++index) {
    for (uint32_t field = 0; field < 3; ++field) {
      f.cpu(records[index] * 3 + field, MemOp::AMO_SWAP, initial[index] + field);
    }
  }
  for (uint32_t index = 0; index < 4; ++index) {
    check(f.mmio(forza_record_address(0, records[index]), forza_record_pack(100 + index, -7))
          == FORZA_MMIO_ACCEPTED, "valid record rejected");
  }
  auto replies = f.collect(4);
  for (const auto& reply : replies) {
    uint32_t index = reply[1] - 100;
    check(index < 4, "reply request id corrupted");
    for (uint32_t field = 0; field < 3; ++field) {
      check(reply[2 + field] == initial[index] + field, "reply did not return exact old tuple");
    }
  }
  for (uint32_t index = 0; index < 4; ++index) {
    check(f.cpu(records[index] * 3, MemOp::LD) == initial[index] + 1,
          "record count update wrong");
    check(f.cpu(records[index] * 3 + 1, MemOp::LD) == initial[index] + 1 + uint64_t(-7),
          "record signed sum update wrong");
    check(f.cpu(records[index] * 3 + 2, MemOp::LD) == ((initial[index] + 2) ^ (100 + index)),
          "record request-id xor update wrong");
  }
  f.memory_contract(4);

  f.reset(); f.register_source();
  for (uint32_t field = 0; field < 3; ++field) f.cpu(field, MemOp::AMO_SWAP, 0);
  f.control(4);
  check(f.mmio(forza_record_address(0, 0), forza_record_pack(9, -5)) == FORZA_MMIO_ACCEPTED,
        "withheld store response request rejected");
  f.tick(128);
  check(f.outstanding() == 1 && f.acks() == 0 && f.receive().empty(),
        "record replied or ACKed before all actual store responses");
  f.control(1, true, false);
  f.tick(64);
  check(f.outstanding() == 1 && f.acks() == 0 && f.receive().empty(),
        "record ACK preceded reply delivery");
  check(f.cpu(0, MemOp::LD) == 1 && f.cpu(1, MemOp::LD) == uint64_t(-5)
        && f.cpu(2, MemOp::LD) == 9, "record stores bypassed actual LocalMem");
  f.control(1);
  f.tick(64);
  check(f.outstanding() == 0, "committed reply READY did not retire request credit");
  auto one = f.collect(1);
  check(one[0][1] == 9 && one[0][2] == 0 && one[0][3] == 0 && one[0][4] == 0,
        "withheld response changed old tuple");
  f.memory_contract(1);

  f.reset(); f.register_source();
  for (uint32_t field = 0; field < 3; ++field) f.cpu(field, MemOp::AMO_SWAP, 0);
  f.control(1, false);
  uint32_t accepted = 0;
  for (; accepted < 512; ++accepted) {
    auto result = f.mmio(forza_record_address(0, 0), forza_record_pack(accepted + 1, 1));
    if (result == FORZA_MMIO_BUSY) break;
    check(result == FORZA_MMIO_ACCEPTED, "bounded record rejected valid request");
  }
  check(accepted >= FORZA_RECORD_QUEUE_DEPTH && accepted < 255,
        "record queue/context/fabric backpressure was not bounded");
  check(f.outstanding() == accepted && f.acks() == 0, "record BUSY changed source credit");
  f.control(1);
  auto many = f.collect(accepted);
  uint64_t expected_xor = 0;
  for (uint32_t i = 0; i < accepted; ++i) {
    check(many[i][1] == i + 1 && many[i][2] == i && many[i][3] == i
          && many[i][4] == expected_xor, "same-record transactions were not serialized");
    expected_xor ^= i + 1;
  }
  check(f.cpu(0, MemOp::LD) == accepted && f.cpu(1, MemOp::LD) == accepted
        && f.cpu(2, MemOp::LD) == expected_xor, "bounded resume lost or duplicated transactions");
  f.memory_contract(accepted);
}

void direct_contract(Fixture& f) {
  check(f.mmio(FORZA_MMIO_REGISTER, 0, MemOp::LD) == 0,
        "direct source unexpectedly has actor registration");
  check(f.mmio(FORZA_RECORD_CQ_ID, 0, MemOp::LD) == 0,
        "empty direct completion queue reports an ID");
  check(f.mmio(FORZA_RECORD_CQ_COUNT, 0, MemOp::LD) == FORZA_MMIO_INVALID
        && f.mmio(FORZA_RECORD_CQ_RELEASE, 0) == FORZA_MMIO_INVALID,
        "empty direct completion access accepted");
  check(f.mmio(FORZA_RECORD_DIRECT_INVALID_ADDRESS, 0) == FORZA_MMIO_INVALID
        && f.mmio(forza_record_direct_address(15, 0), 0) == FORZA_MMIO_INVALID,
        "invalid direct command address accepted");
  check(f.mmio(forza_mmio_actor_address(1, FORZA_RECORD_CQ_ID), 0, MemOp::LD)
        == FORZA_MMIO_INVALID, "direct completion accepted a foreign physical source endpoint");
  const uint32_t records[] = {0, 1, 2, FORZA_RECORDS_PER_ZONE - 1};
  const uint32_t ids[] = {0, UINT32_MAX, 17, 99};
  const uint64_t initial[] = {0, UINT64_MAX, 0x8000000000000000ull, 12};
  for (uint32_t index = 0; index < 4; ++index) {
    for (uint32_t field = 0; field < 3; ++field) {
      f.cpu(records[index] * 3 + field, MemOp::AMO_SWAP, initial[index] + field);
    }
    check(f.mmio(forza_record_direct_address(0, records[index]), forza_record_pack(ids[index], -7))
          == FORZA_MMIO_ACCEPTED, "direct record rejected without actor registration");
  }
  f.tick(512);
  check(f.outstanding() == 2, "two direct slots did not bound committed reply capacity");
  uint64_t front = f.mmio(FORZA_RECORD_CQ_ID, 0, MemOp::LD);
  check(front && f.mmio(forza_record_peek_address(1), 0, MemOp::LD) == 0
        && f.mmio(forza_mmio_pop_address(1), 0) == 0
        && f.mmio(forza_record_release_address(1), 0) == FORZA_MMIO_INVALID
        && f.mmio(FORZA_RECORD_CQ_ID, 0, MemOp::LD) == front,
        "generic mailbox APIs accessed or released a direct completion");
  auto replies = f.collect(4, true);
  for (const auto& reply : replies) {
    uint32_t index = 0;
    while (index < 4 && ids[index] != reply[1]) ++index;
    check(index < 4, "direct reply lost full32 request ID");
    for (uint32_t field = 0; field < 3; ++field) {
      check(reply[2 + field] == initial[index] + field, "direct reply old tuple changed");
    }
  }
  for (uint32_t index = 0; index < 4; ++index) {
    check(f.cpu(records[index] * 3, MemOp::LD) == initial[index] + 1,
          "direct record count update wrong");
    check(f.cpu(records[index] * 3 + 1, MemOp::LD) == initial[index] + 1 + uint64_t(-7),
          "direct record signed sum update wrong");
    check(f.cpu(records[index] * 3 + 2, MemOp::LD) == ((initial[index] + 2) ^ ids[index]),
          "direct record request-id xor update wrong");
  }
  check(f.mmio(FORZA_RECORD_CQ_ID, 0, MemOp::LD) == 0,
        "drained direct completion queue retained a result");
  check(f.mmio(forza_record_address(0, 0), forza_record_pack(5, 1)) == FORZA_MMIO_INVALID,
        "direct source silently switched to actor mailbox mode");
  check(f.mmio(FORZA_MMIO_REGISTER, (7u << 8) | 2) == FORZA_MMIO_INVALID,
        "actor mailbox registration overwrote direct completion storage");
  f.memory_contract(4);

  f.reset();
  f.register_source();
  check(f.mmio(forza_record_direct_address(0, 0), forza_record_pack(1, 1)) == FORZA_MMIO_INVALID,
        "registered mailbox source silently switched to direct completion mode");
  check(f.mmio(FORZA_RECORD_CQ_ID, 0, MemOp::LD) == FORZA_MMIO_INVALID,
        "direct endpoint read registered mailbox storage");

  f.reset();
  for (uint32_t field = 0; field < 3; ++field) f.cpu(field, MemOp::AMO_SWAP, 0);
  f.control(4);
  check(f.mmio(forza_record_direct_address(0, 0), forza_record_pack(0, INT32_MIN))
        == FORZA_MMIO_ACCEPTED, "withheld direct-store request rejected");
  f.tick(128);
  check(f.outstanding() == 1 && f.acks() == 0 && f.receive_direct().empty(),
        "direct reply preceded all actual store responses");
  f.control(1, true, false);
  f.tick(64);
  check(f.outstanding() == 1 && f.acks() == 0 && f.receive_direct().empty(),
        "direct credit retired before result queue delivery");
  f.control(1);
  f.tick(64);
  check(f.outstanding() == 0, "READY direct result did not retire request credit");
  auto one = f.collect(1, true);
  check(one[0][1] == 0 && one[0][2] == 0 && one[0][3] == 0 && one[0][4] == 0,
        "direct zero ID or previous zero tuple was treated as empty");
  check(f.cpu(0, MemOp::LD) == 1 && f.cpu(1, MemOp::LD) == uint64_t(int64_t(INT32_MIN))
        && f.cpu(2, MemOp::LD) == 0, "direct commit bypassed real memory or signed32 conversion");
  f.memory_contract(1);

  f.reset();
  for (uint32_t field = 0; field < 3; ++field) f.cpu(field, MemOp::AMO_SWAP, 0);
  f.control(1, false);
  uint32_t accepted = 0;
  for (; accepted < 512; ++accepted) {
    auto status = f.mmio(forza_record_direct_address(0, 0), forza_record_pack(accepted + 1, 1));
    if (status == FORZA_MMIO_BUSY) break;
    check(status == FORZA_MMIO_ACCEPTED, "valid direct request rejected during flood");
  }
  check(accepted >= FORZA_RECORD_QUEUE_DEPTH && accepted < 255,
        "direct request backpressure was not bounded");
  check(f.outstanding() == accepted && f.acks() == 0, "direct BUSY changed source credit");
  f.control(1);
  auto many = f.collect(accepted, true);
  uint64_t prefix = 0;
  for (uint32_t index = 0; index < accepted; ++index) {
    check(many[index][1] == index + 1 && many[index][2] == index && many[index][3] == index
          && many[index][4] == prefix, "direct recovery reordered/lost same-record transactions");
    prefix ^= index + 1;
  }
  check(f.cpu(0, MemOp::LD) == accepted && f.cpu(1, MemOp::LD) == accepted
        && f.cpu(2, MemOp::LD) == prefix, "direct flood final state differs from reply history");
  f.memory_contract(accepted);
}

void symmetric_contract(bool direct = false) {
  auto fabric = ForzaFabric::Create("record-fabric", 2);
  Fixture left(0, fabric), right(1, fabric);
  if (!direct) { left.register_source(); right.register_source(); }
  for (uint32_t field = 0; field < 3; ++field) {
    left.cpu(field, MemOp::AMO_SWAP, 0);
    right.cpu(field, MemOp::AMO_SWAP, 0);
  }
  uint32_t sent[2] = {0, 0};
  bool busy[2] = {false, false};
  for (uint32_t step = 0; step < 512 && (!busy[0] || !busy[1]); ++step) {
    for (uint32_t side = 0; side < 2; ++side) {
      if (busy[side]) continue;
      auto& source = side ? right : left;
      auto result = source.mmio(direct ? forza_record_direct_address(1 - side, 0)
                                      : forza_record_address(1 - side, 0),
                                forza_record_pack(sent[side] + 1, 1));
      if (result == FORZA_MMIO_BUSY) busy[side] = true;
      else {
        check(result == FORZA_MMIO_ACCEPTED, "cross-core flood rejected valid record");
        ++sent[side];
      }
    }
  }
  check(busy[0] && busy[1] && sent[0] > FORZA_RECORD_QUEUE_DEPTH
        && sent[1] > FORZA_RECORD_QUEUE_DEPTH, "symmetric request flood did not saturate both paths");
  uint32_t received[2] = {0, 0};
  uint64_t prefix[2] = {0, 0};
  for (uint32_t step = 0; step < 60000 && (received[0] < sent[0] || received[1] < sent[1]); ++step) {
    for (uint32_t side = 0; side < 2; ++side) {
      auto& source = side ? right : left;
      auto reply = direct ? source.receive_direct() : source.receive();
      if (reply.empty()) continue;
      uint32_t ticket = received[side];
      check(reply[1] == ticket + 1 && reply[2] == ticket && reply[3] == ticket
            && reply[4] == prefix[side], "symmetric saturation lost transaction linearization");
      prefix[side] ^= ticket + 1;
      ++received[side];
    }
    left.tick();
  }
  check(received[0] == sent[0] && received[1] == sent[1],
        "reply virtual channel failed to escape saturated symmetric request queues");
  left.drain(); right.drain();
  check(left.cpu(0, MemOp::LD) == sent[1] && left.cpu(1, MemOp::LD) == sent[1]
        && left.cpu(2, MemOp::LD) == prefix[1], "left final record differs from exact flood prefix");
  check(right.cpu(0, MemOp::LD) == sent[0] && right.cpu(1, MemOp::LD) == sent[0]
        && right.cpu(2, MemOp::LD) == prefix[0], "right final record differs from exact flood prefix");
  left.memory_contract(sent[1]); right.memory_contract(sent[0]);
  std::cout << "PASSED: " << (direct ? "direct CQ " : "mailbox ")
            << "symmetric real-Fabric flood requests=" << sent[0] << "," << sent[1]
            << " drains through independent reply VC with exact tuples and credits" << std::endl;
}

}

int main(int argc, char** argv) {
  check(SimPlatform::instance().initialize(), "simulation platform initialization failed");
  std::string mode = argc > 1 ? argv[1] : "";
  if (mode == "--symmetric" || mode == "--direct-symmetric") {
    symmetric_contract(mode == "--direct-symmetric");
    return 0;
  }
  Fixture fixture;
  if (mode == "--direct") {
    direct_contract(fixture);
    std::cout << "PASSED: direct CQ uses same two64B slots, engine/LocalMem/fabric, full32 IDs, "
                 "no registration, exclusive modes, commit+delivery ACK, bounded exact recovery"
              << std::endl;
    return 0;
  }
  if (argc > 1) {
    bool direct = mode.find("--direct-") == 0;
    uint32_t fault = mode.find("bad-uuid") != std::string::npos ? 2
                   : mode.find("bad-tag") != std::string::npos ? 5 : 3;
    if (!direct) fixture.register_source();
    fixture.cpu(0, MemOp::AMO_SWAP, 0);
    fixture.control(fault);
    try {
      check(fixture.mmio(direct ? forza_record_direct_address(0, 0) : forza_record_address(0, 0),
                         forza_record_pack(1, 1)) == FORZA_MMIO_ACCEPTED,
            "identity test enqueue failed");
      fixture.tick(128);
    } catch (const std::runtime_error& error) {
      check((std::string(error.what()).find("identity mismatch") != std::string::npos
             || (fault == 5 && std::string(error.what()).find("unknown LocalMem tag") != std::string::npos))
            && fixture.acks() == 0, "wrong memory identity caused an unexpected failure/ACK");
      std::cout << "PASSED: matching-tag wrong LocalMem "
                << (fault == 2 ? "uuid" : fault == 5 ? "wide tag" : "hart") << " rejected before ACK/release" << std::endl;
      return 0;
    }
    check(false, "wrong LocalMem response identity was accepted");
  }
  normal_contract(fixture);
  std::cout << "PASSED: RECORD uses real banked LocalMem six8B requests, signed/wrap/bounds, "
               "snapshot replies, commit+reply ACK, bounded contexts/backpressure and serialized exact recovery"
            << std::endl;
  return 0;
}
