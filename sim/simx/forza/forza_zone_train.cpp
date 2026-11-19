// CSR 학습용 복사본. ISA/MMIO/UPDATE/RECORD/VM 확장을 제외한 기본 경로.
// 원본을 대체하거나 빌드에 함께 추가하지 않는다. 클래스 이름은 원본과 같다.
#include "forza_zone_train.h"
#include "instr.h"
#include "instr_trace.h"
#include <cstring>
#include <stdexcept>

using namespace vortex;

namespace {
bool trace_enabled() {
  static const bool enabled = std::getenv("VORTEX_FORZA_TRACE") != nullptr;
  return enabled;
}
}

static_assert(VX_CFG_XLEN == 64, "FORZA native message CSRs require RV64");
static_assert(SIMX_NUM_WORKERS == 1, "FORZA prototype currently requires one SimX worker");
static_assert(VX_CFG_NUM_SFU_LANES == VX_CFG_NUM_THREADS,
              "FORZA prototype requires full-width per-warp CSR dispatch");
#ifdef VX_CFG_VM_ENABLE
#error "FORZA prototype mailbox aperture currently requires VM disabled"
#endif
#ifdef SIMX_FUNCTIONAL
#error "FORZA bounded queue prototype requires the timing simulation kernel"
#endif
static_assert(VX_CFG_NUM_CORES * VX_CFG_NUM_CLUSTERS <= 16, "FORZA zone field is four bits");
static_assert(VX_CFG_NUM_WARPS * VX_CFG_NUM_THREADS * 1024 <= FORZA_MAILBOX_APERTURE,
              "actor mailbox SRAM exceeds its aperture");

ForzaZone::ForzaZone(const SimContext& ctx, const char* name, uint32_t core_id)
  : SimObject<ForzaZone>(ctx, name)
  , csr_req_in(this, 2)
  , csr_rsp_out(this)
  , mem_req_in(VX_CFG_NUM_LSU_BLOCKS, SimChannel<LsuReq>(this, 2))
  , mem_rsp_out(VX_CFG_NUM_LSU_BLOCKS, SimChannel<LsuRsp>(this))
  , net_tx(this)
  , net_rx(this, 2)
  , ack_tx(this)
  , ack_rx(this, 2)
  , core_id_(core_id)
  , actors_(ACTORS)
  , sram_(ACTORS * 1024) {
}

void ForzaZone::on_reset() {
  actors_.assign(ACTORS, Actor{});
  pe_to_actor_.clear();
  std::fill(sram_.begin(), sram_.end(), 0);
  local_tx_.clear();
  external_tx_.clear();
  for (auto& queue : incoming_) {
    queue.clear();
  }
  filling_.clear();
  pending_.clear();
  sequence_ = 0;
  receive_rr_ = 0;
  transmit_rr_ = 0;
  csr_work_.clear();
}

uint64_t ForzaZone::slot_address(uint32_t actor, uint32_t mailbox, uint32_t slot) const {
  return FORZA_MAILBOX_BASE + actor * 1024 + mailbox * 128 + slot * 64;
}

uint64_t ForzaZone::read_csr(uint32_t actor_id, uint32_t addr) const {
  if (addr == FORZA_CSR_ZQMAVAIL) {
    addr = FORZA_CSR_ZQMSTAT;
  } else if (addr >= FORZA_CSR_ZQMMSG0 && addr < FORZA_CSR_ZQMMSG0 + 8) {
    addr = FORZA_CSR_ZQMDQ0 + addr - FORZA_CSR_ZQMMSG0;
  }
  const auto& actor = actors_.at(actor_id);
  if (addr == FORZA_CSR_ZENSTAT) {
    uint64_t status = (1ull << 63) | (uint64_t(actor.send_errors) << 8)
                    | (uint64_t(actor.write_error) << 16);
    // Sequence credits and outgoing space are independent from receiver slots.
    status |= uint64_t(255 - *std::max_element(actor.outstanding.begin(),
                                             actor.outstanding.end())) << 17;
    for (uint32_t mailbox = 0; mailbox < 8; ++mailbox) {
      if (actor.outstanding[mailbox] == 255
          || local_tx_.size() >= TX_DEPTH || external_tx_.size() >= TX_DEPTH) {
        status |= 1ull << mailbox;
      }
    }
    return status;
  }
  if (addr == FORZA_CSR_ZQMSTAT) {
    uint64_t status = 0;
    for (uint32_t mailbox = 0; mailbox < 8; ++mailbox) {
      const auto& queue = actor.mailboxes[mailbox];
      if ((actor.registration & (1u << mailbox))
          && queue.slots[queue.read_slot] == SlotState::Ready) {
        status |= 1ull << mailbox;
      }
    }
    return status;
  }
  if (addr == FORZA_CSR_ZQMMBOXREG) {
    return actor.registration;
  }
  if (addr == FORZA_CSR_ZENCOUNT) {
    uint64_t counters = 0;
    for (uint32_t mailbox = 0; mailbox < 8; ++mailbox) {
      counters |= uint64_t(actor.outstanding[mailbox]) << (8 * mailbox);
    }
    return counters;
  }
  if (addr >= FORZA_CSR_ZQMDQ0 && addr < FORZA_CSR_ZQMDQ0 + 8) {
    uint32_t mailbox = addr - FORZA_CSR_ZQMDQ0;
    const auto& queue = actor.mailboxes[mailbox];
    if ((actor.registration & (1u << mailbox))
        && queue.slots[queue.read_slot] == SlotState::Ready) {
      if (trace_enabled()) {
        std::cerr << "FORZA peek zone=" << core_id_ << " actor=" << actor_id
                  << " mb=" << mailbox << " slot=" << queue.read_slot << std::endl;
      }
      return slot_address(actor_id, mailbox, queue.read_slot);
    }
  }
  return 0;
}

bool ForzaZone::write_csr(uint32_t actor_id, uint32_t addr, uint64_t value) {
  auto& actor = actors_.at(actor_id);
  if (addr == FORZA_CSR_ZENEQD) {
    if (actor.payload_size < FORZA_PAYLOAD_WORDS) {
      actor.payload[actor.payload_size++] = value;
    } else {
      actor.write_error = true;
    }
    return true;
  }
  if (addr == FORZA_CSR_ZENEQC) {
    uint32_t mailbox = (value >> FORZA_CONTROL_MBOX_SHIFT) & 7;
    uint32_t zone = (value >> FORZA_DEST_ZONE_SHIFT) & 15;
    // Unsupported precincts and oversize payloads must not create messages.
    if (actor.write_error || ((value >> 19) & 0x1fff)
        || zone >= VX_CFG_NUM_CORES * VX_CFG_NUM_CLUSTERS) {
      actor.send_errors |= 1u << mailbox;
      actor.payload_size = 0;
      actor.payload.fill(0);
      return true;
    }
    auto& tx = zone == core_id_ ? local_tx_ : external_tx_;
    if (tx.size() == TX_DEPTH || actor.outstanding[mailbox] == 255) {
      return false;
    }
    ForzaMessage message;
    message.source_zone = core_id_;
    message.source_actor = actor_id;
    message.dest_zone = zone;
    message.dest_pe = value & 0x7ff;
    message.mailbox = mailbox;
    message.sequence = sequence_++;
    message.words[0] = value;
    std::copy(actor.payload.begin(), actor.payload.end(), message.words.begin() + 1);
    tx.push_back(message);
    if (trace_enabled()) {
      std::cerr << "FORZA send zone=" << core_id_ << " actor=" << actor_id
                << " dest=" << zone << ":" << message.dest_pe
                << " mb=" << mailbox << " seq=" << message.sequence << std::endl;
    }
    pending_.emplace(message.sequence, std::make_pair(actor_id, mailbox));
    ++actor.outstanding[mailbox];
    actor.payload_size = 0;
    actor.payload.fill(0);
    return true;
  }
  if (addr == FORZA_CSR_ZQMMBOXREG) {
    uint32_t pe = (value >> 8) & 0x7ff;
    uint32_t registration = value & 0x7ffff;
    auto existing = pe_to_actor_.find(pe);
    if (
        (existing != pe_to_actor_.end() && existing->second != actor_id)
        || (actor.registration != 0 && actor.registration != registration)) {
      actor.write_error = true;
      return true;
    }
    actor.registration = registration;
    if (registration & 255) {
      pe_to_actor_[pe] = actor_id;
    }
    return true;
  }
  if (addr >= FORZA_CSR_ZQMDQ0 && addr < FORZA_CSR_ZQMDQ0 + 8) {
    auto& mailbox = actor.mailboxes[addr - FORZA_CSR_ZQMDQ0];
    if (mailbox.slots[mailbox.read_slot] == SlotState::Ready) {
      if (trace_enabled()) {
        std::cerr << "FORZA release zone=" << core_id_ << " actor=" << actor_id
                  << " mb=" << addr - FORZA_CSR_ZQMDQ0
                  << " slot=" << mailbox.read_slot << std::endl;
      }
      mailbox.slots[mailbox.read_slot] = SlotState::Empty;
      mailbox.read_slot ^= 1;
    } else {
      actor.write_error = true;
    }
    return true;
  }
  actor.write_error = true;
  return true;
}

void ForzaZone::tick_csr() {
  if (!csr_req_in.empty() && csr_work_.size() < VX_CFG_NUM_WARPS * 2) {
    auto request = csr_req_in.peek();
    request.trace->dst_data.assign(VX_CFG_NUM_THREADS, reg_data_t{});
    CsrWork work{};
    work.request = request;
    csr_work_.push_back(work);
    csr_req_in.pop();
  }
  if (csr_work_.empty() || csr_rsp_out.full()) {
    return;
  }
  auto work = csr_work_.front();
  csr_work_.pop_front();
  auto request = work.request;
  auto* trace = request.trace;
  const auto args = std::get<IntrCsrArgs>(trace->instr_ptr->get_args());
  const auto op = std::get<CsrType>(trace->op_type);
  // Retain the lane cursor while a bounded TX queue applies backpressure.
  // Replaying completed lanes would duplicate enqueue/release side effects.
  for (; work.lane < VX_CFG_NUM_THREADS; ++work.lane) {
    if (!trace->tmask.test(work.lane)) {
      continue;
    }
    uint32_t actor = trace->wid * VX_CFG_NUM_THREADS + work.lane;
    uint64_t old_value = read_csr(actor, args.csr);
    uint64_t source = args.is_imm ? args.imm : trace->src_data[0][work.lane].i;
    if (args.csr == FORZA_CSR_ZENTRY && op == CsrType::CSRRW) {
      auto& state = actors_[actor];
      uint32_t zone = (source >> FORZA_DEST_ZONE_SHIFT) & 15;
      bool invalid = state.write_error || ((source >> 19) & 0x1fff)
                  || zone >= VX_CFG_NUM_CORES * VX_CFG_NUM_CLUSTERS;
      bool accepted = write_csr(actor, FORZA_CSR_ZENEQC, source);
      if (!accepted) {
        // The caller retains the packet when the attempt fails. Reset its
        // assembler so the next attempt can safely write all seven words.
        state.payload_size = 0;
        state.payload.fill(0);
      }
      trace->dst_data[work.lane].i = invalid ? FORZA_TRY_INVALID
                                      : accepted ? FORZA_TRY_ACCEPTED : FORZA_TRY_BUSY;
      continue;
    }
    bool write = op == CsrType::CSRRW || source != 0;
    uint64_t new_value = source;
    if (op == CsrType::CSRRS) {
      new_value = old_value | source;
    } else if (op == CsrType::CSRRC) {
      new_value = old_value & ~source;
    }
    if (write && !write_csr(actor, args.csr, new_value)) {
      csr_work_.push_back(work);
      return;
    }
    trace->dst_data[work.lane].i = old_value;
  }
  trace->resume_warp = trace->fetch_stall;
  csr_rsp_out.send(request, 1);
}

void ForzaZone::tick_memory() {
  for (uint32_t port = 0; port < mem_req_in.size(); ++port) {
    if (mem_req_in[port].empty() || mem_rsp_out[port].full()) {
      continue;
    }
    const auto& request = mem_req_in[port].peek();
    if (request.op != MemOp::LD) {
      throw std::runtime_error("FORZA mailbox aperture supports loads only");
    }
    LsuRsp response(request.mask.size());
    response.mask = request.mask;
    response.tag = request.tag;
    response.cid = request.cid;
    response.uuid = request.uuid;
    for (uint32_t lane = 0; lane < request.mask.size(); ++lane) {
      if (!request.mask.test(lane)) {
        continue;
      }
      uint64_t offset = (request.addrs[lane] - FORZA_MAILBOX_BASE)
                      & ~uint64_t(VX_CFG_MEM_BLOCK_SIZE - 1);
      if (offset + VX_CFG_MEM_BLOCK_SIZE > sram_.size()) {
        throw std::runtime_error("FORZA mailbox load outside instantiated SRAM");
      }
      response.data[lane] = std::make_shared<mem_block_t>();
      std::memcpy(response.data[lane]->data(), sram_.data() + offset,
                  VX_CFG_MEM_BLOCK_SIZE);
    }
    mem_rsp_out[port].send(response, 1);
    if (trace_enabled()) {
      std::cerr << "FORZA load zone=" << core_id_ << " port=" << port
                << " tag=" << request.tag << " mask=" << request.mask << std::endl;
    }
    mem_req_in[port].pop();
  }
}

void ForzaZone::tick_receive() {
  if (!filling_.empty() && !ack_tx.full()) {
    const auto& fill = filling_.front();
    uint64_t offset = slot_address(fill.actor, fill.message.mailbox, fill.slot)
                    - FORZA_MAILBOX_BASE;
    // Store bytes in guest little-endian order independently of host endianness.
    for (uint32_t word = 0; word < 8; ++word) {
      for (uint32_t byte = 0; byte < 8; ++byte) {
        sram_[offset + word * 8 + byte] = fill.message.words[word] >> (byte * 8);
      }
    }
    actors_[fill.actor].mailboxes[fill.message.mailbox].slots[fill.slot] = SlotState::Ready;
    if (trace_enabled()) {
      std::cerr << "FORZA ready zone=" << core_id_ << " actor=" << fill.actor
                << " mb=" << fill.message.mailbox << " slot=" << fill.slot
                << " seq=" << fill.message.sequence << std::endl;
    }
    ack_tx.send(ForzaAck{fill.message.source_zone, fill.message.source_actor,
                        fill.message.mailbox, fill.message.sequence, false}, 1);
    filling_.pop_front();
  }
  if (!net_rx.empty()) {
    const auto& message = net_rx.peek();
      auto& queue = incoming_[message.mailbox];
      if (queue.size() < RX_DEPTH) {
        queue.push_back(message);
        net_rx.pop();
      }
  }
  // At most one reservation per cycle; independent mailboxes arbitrate fairly.
  for (uint32_t offset = 0; offset < 8; ++offset) {
    uint32_t mb = (receive_rr_ + offset) % 8;
    auto& queue = incoming_[mb];
    if (queue.empty()) {
      continue;
    }
    auto message = queue.front();
    auto mapping = pe_to_actor_.find(message.dest_pe);
    uint32_t actor_id = mapping == pe_to_actor_.end() ? ACTORS : mapping->second;
    bool valid = actor_id < ACTORS && (actors_[actor_id].registration & (1u << mb));
    if (!valid) {
      if (ack_tx.full()) {
        continue;
      }
      ack_tx.send(ForzaAck{message.source_zone, message.source_actor, mb,
                          message.sequence, true}, 1);
      queue.pop_front();
    } else {
      auto& mailbox = actors_[actor_id].mailboxes[mb];
      if (mailbox.slots[mailbox.write_slot] == SlotState::Empty
          && filling_.empty()) {
        uint32_t slot = mailbox.write_slot;
        mailbox.slots[slot] = SlotState::Filling;
        mailbox.write_slot ^= 1;
        filling_.push_back(Filling{message, actor_id, slot});
        queue.pop_front();
      }
      // A blocked head is retained: reordering messages for one destination
      // would violate per-sender FIFO delivery. Other mailboxes still progress.
    }
    receive_rr_ = (mb + 1) % 8;
    break;
  }
}





void ForzaZone::on_tick() {
  if (!ack_rx.empty()) {
    const auto& ack = ack_rx.peek();
    auto pending = pending_.find(ack.sequence);
    if (pending == pending_.end() || pending->second.first != ack.actor
        || pending->second.second != ack.mailbox) {
      throw std::runtime_error("FORZA unmatched delivery acknowledgement");
    }
    auto& actor = actors_[ack.actor];
    --actor.outstanding[ack.mailbox];
    if (ack.error) {
      actor.send_errors |= 1u << ack.mailbox;
    }
    pending_.erase(pending);
    ack_rx.pop();
  }
  tick_receive();
  tick_memory();
  tick_csr();
  if (!net_tx.full()) {
    for (uint32_t offset = 0; offset < 2; ++offset) {
      uint32_t selected = (transmit_rr_ + offset) % 2;
      auto& queue = selected ? external_tx_ : local_tx_;
      if (!queue.empty()) {
        net_tx.send(queue.front(), 1);
        queue.pop_front();
        transmit_rr_ = selected ^ 1;
        break;
      }
    }
  }
}
