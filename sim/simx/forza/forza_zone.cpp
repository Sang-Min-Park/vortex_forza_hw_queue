#include "forza_zone.h"
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
#ifdef VX_CFG_FORZA_MMIO_ENABLE
bool update_address(uint64_t address) {
#ifdef VX_CFG_FORZA_UPDATE_ENABLE
  return address >= FORZA_UPDATE_BASE
      && address - FORZA_UPDATE_BASE < FORZA_UPDATE_APERTURE;
#else
  (void)address;
  return false;
#endif
}

bool record_address(uint64_t address) {
#ifdef VX_CFG_FORZA_RECORD_ENABLE
  return (address >= FORZA_RECORD_BASE
          && address - FORZA_RECORD_BASE < FORZA_RECORD_APERTURE)
      || (address >= FORZA_RECORD_DIRECT_BASE
          && address - FORZA_RECORD_DIRECT_BASE < FORZA_RECORD_APERTURE);
#else
  (void)address;
  return false;
#endif
}

bool vm_address(uint64_t address) {
#ifdef VX_CFG_FORZA_VM_ENABLE
  return (address >= FORZA_VM_BASE && address - FORZA_VM_BASE < FORZA_VM_APERTURE)
      || (address >= FORZA_VM_DIRECT_BASE && address - FORZA_VM_DIRECT_BASE < FORZA_VM_APERTURE)
      || (address >= FORZA_VM_CONFIG_BASE && address - FORZA_VM_CONFIG_BASE < FORZA_VM_CONFIG_BYTES);
#else
  (void)address;
  return false;
#endif
}

bool mmio_address(uint64_t address) {
  return (address >= FORZA_MMIO_BASE
      && address - FORZA_MMIO_BASE < FORZA_MMIO_APERTURE)
      || update_address(address) || record_address(address) || vm_address(address);
}

bool mmio_request(const LsuReq& request) {
  for (uint32_t lane = 0; lane < request.mask.size(); ++lane) {
    if (request.mask.test(lane) && mmio_address(request.addrs[lane])) {
      return true;
    }
  }
  return false;
}
#endif
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
#ifdef VX_CFG_FORZA_MMIO_ENABLE
static_assert(VX_CFG_NUM_WARPS * VX_CFG_NUM_THREADS <= FORZA_MMIO_MAX_ACTORS,
              "actor MMIO endpoints exceed their aperture");
#endif
#ifdef VX_CFG_FORZA_UPDATE_ENABLE
static_assert(VX_CFG_LMEM_ENABLED, "FORZA update requires CPU-shared LocalMem");
static_assert(FORZA_UPDATE_ZONE_STRIDE == (1ull << VX_CFG_LMEM_LOG_SIZE),
              "FORZA update ABI requires the configured 16KiB LocalMem");
#endif

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
#if defined(VX_CFG_FORZA_RECORD_ENABLE) || defined(VX_CFG_FORZA_VM_ENABLE)
  , reply_net_tx(this, 2)
  , reply_net_rx(this, 2)
#endif
#ifdef VX_CFG_FORZA_UPDATE_ENABLE
  , update_req_out(this, 2)
  , update_rsp_in(this, 2)
#endif
#ifdef VX_CFG_FORZA_VM_ENABLE
  , vm_request_out(this, 2), vm_result_in(this, 2), vm_result_ack_out(this, 2)
  , vm_completion_in(this, 2), vm_config_out(this, 2), vm_config_ack_in(this, 2)
#endif
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
#ifdef VX_CFG_FORZA_VM_ENABLE
  vm_replies_.clear();
  vm_configs_.clear();
  vm_config_sequence_ = 0;
#endif
#ifdef VX_CFG_FORZA_ISA_ENABLE
  command_budget_ = 0;
  isa_turn_ = false;
#endif
#ifdef VX_CFG_FORZA_UPDATE_ENABLE
  update_queue_.clear();
  update_pending_.clear();
#ifdef VX_CFG_FORZA_RECORD_ENABLE
  record_queue_.clear();
  record_contexts_.fill(RecordContext{});
  record_replies_.clear();
  record_issue_rr_ = 0;
  record_memory_turn_ = false;
#endif
#endif
#ifdef VX_CFG_FORZA_MMIO_ENABLE
  mmio_lane_.fill(0);
  mmio_port_rr_ = 0;
#endif
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
#if defined(VX_CFG_FORZA_RECORD_ENABLE) || defined(VX_CFG_FORZA_VM_ENABLE)
        (actor.record_direct && (registration & (1u << FORZA_RECORD_REPLY_MAILBOX)))
        ||
#endif
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
#if defined(VX_CFG_FORZA_RECORD_ENABLE) || defined(VX_CFG_FORZA_VM_ENABLE)
    if (actor.record_direct && addr == FORZA_CSR_ZQMDQ0 + FORZA_RECORD_REPLY_MAILBOX) {
      actor.write_error = true;
      return true;
    }
#endif
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
#ifdef VX_CFG_FORZA_ISA_ENABLE
  if (std::get_if<ForzaType>(&trace->op_type)) {
    tick_isa(work);
    return;
  }
#endif
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

uint64_t ForzaZone::enqueue_token(uint32_t actor_id, uint32_t zone, uint32_t pe,
                                  uint32_t mailbox, uint64_t token) {
  auto& actor = actors_.at(actor_id);
  if (actor.write_error || zone >= VX_CFG_NUM_CORES * VX_CFG_NUM_CLUSTERS
      || pe >= 2048 || mailbox >= 8) {
    return FORZA_TRY_INVALID;
  }
  auto& tx = zone == core_id_ ? local_tx_ : external_tx_;
  if (tx.size() >= TX_DEPTH || actor.outstanding[mailbox] == 255) {
    return FORZA_TRY_BUSY;
  }
  ForzaMessage message;
  message.source_zone = core_id_;
  message.source_actor = actor_id;
  message.dest_zone = zone;
  message.dest_pe = pe;
  message.mailbox = mailbox;
  message.sequence = sequence_++;
  message.words[0] = forza_message_control(zone, pe, mailbox, 0);
  message.words[1] = token;
  tx.push_back(message);
  pending_.emplace(message.sequence, std::make_pair(actor_id, mailbox));
  ++actor.outstanding[mailbox];
  return FORZA_TRY_ACCEPTED;
}

uint64_t ForzaZone::pop_token(uint32_t actor_id, uint32_t mailbox_id) {
  auto& actor = actors_.at(actor_id);
  auto& mailbox = actor.mailboxes.at(mailbox_id);
  if (!(actor.registration & (1u << mailbox_id))
      || mailbox.slots[mailbox.read_slot] != SlotState::Ready) {
    return 0;
  }
  uint64_t offset = slot_address(actor_id, mailbox_id, mailbox.read_slot)
                  - FORZA_MAILBOX_BASE + 8;
  uint64_t value = 0;
  for (uint32_t byte = 0; byte < 8; ++byte) {
    value |= uint64_t(sram_[offset + byte]) << (byte * 8);
  }
  mailbox.slots[mailbox.read_slot] = SlotState::Empty;
  mailbox.read_slot ^= 1;
  return value;
}

#ifdef VX_CFG_FORZA_ISA_ENABLE
uint64_t ForzaZone::isa_command(uint32_t actor_id, ForzaType op,
                                uint64_t first, uint64_t second) {
  auto& actor = actors_.at(actor_id);
  switch (op) {
  case ForzaType::SEND_TRY:
  case ForzaType::SEND_WAIT: {
    constexpr uint64_t route_mask = 0x7ffull | (15ull << FORZA_DEST_ZONE_SHIFT)
                                  | (7ull << FORZA_CONTROL_MBOX_SHIFT);
    if ((first & ~route_mask) || second == 0) {
      return FORZA_TRY_INVALID;
    }
    return enqueue_token(actor_id, (first >> FORZA_DEST_ZONE_SHIFT) & 15,
                         first & 0x7ff, (first >> FORZA_CONTROL_MBOX_SHIFT) & 7,
                         second);
  }
  case ForzaType::RECV_TRY:
  case ForzaType::RECV_WAIT:
  case ForzaType::RECV_WAIT_ANY:
    if (first >= 8 || !(actor.registration & (1u << first))) {
      actor.write_error = true;
      return 0;
    }
    return pop_token(actor_id, first);
  case ForzaType::CREDITS:
    return read_csr(actor_id, FORZA_CSR_ZENCOUNT);
  case ForzaType::REGISTER:
    if (first >> 19 || actor.write_error) {
      return FORZA_TRY_INVALID;
    }
    write_csr(actor_id, FORZA_CSR_ZQMMBOXREG, first);
    return actor.write_error ? FORZA_TRY_INVALID : FORZA_TRY_ACCEPTED;
  case ForzaType::STATUS:
    return read_csr(actor_id, FORZA_CSR_ZENSTAT);
  }
  throw std::runtime_error("invalid FORZA ISA operation");
}

void ForzaZone::tick_isa(CsrWork work) {
  auto* trace = work.request.trace;
  const auto op = std::get<ForzaType>(trace->op_type);
  if (trace->wid >= VX_CFG_NUM_WARPS || trace->tmask.none()) {
    throw std::runtime_error("invalid FORZA ISA warp or active mask");
  }
  if (op == ForzaType::RECV_WAIT_ANY && !work.wait_any_ready) {
    for (uint32_t lane = 0; lane < VX_CFG_NUM_THREADS; ++lane) {
      if (!trace->tmask.test(lane)) {
        continue;
      }
      uint32_t actor_id = trace->wid * VX_CFG_NUM_THREADS + lane;
      const auto& actor = actors_[actor_id];
      uint64_t mailbox = trace->src_data[0][lane].i;
      if (mailbox >= 8 || !(actor.registration & (1u << mailbox))
          || actor.mailboxes[mailbox].slots[actor.mailboxes[mailbox].read_slot]
             == SlotState::Ready) {
        work.wait_any_ready = true;
        break;
      }
    }
    if (!work.wait_any_ready) {
      csr_work_.push_back(work);
      return;
    }
  }
  uint32_t lane = work.lane;
  for (uint32_t visited = 0; visited < VX_CFG_NUM_THREADS; ++visited) {
    uint32_t selected = lane;
    lane = (lane + 1) % VX_CFG_NUM_THREADS;
    if (!trace->tmask.test(selected) || work.completed.test(selected)) {
      continue;
    }
    if (!command_budget_) {
      break;
    }
    uint32_t actor_id = trace->wid * VX_CFG_NUM_THREADS + selected;
    uint64_t first = trace->src_data[0][selected].i;
    uint64_t second = trace->src_data[1][selected].i;
    auto& actor = actors_[actor_id];
    if (op == ForzaType::RECV_WAIT && first < 8
        && (actor.registration & (1u << first))
        && actor.mailboxes[first].slots[actor.mailboxes[first].read_slot]
           != SlotState::Ready) {
      continue;
    }
    if (op == ForzaType::SEND_WAIT && !actor.write_error && second != 0) {
      constexpr uint64_t route_mask = 0x7ffull | (15ull << FORZA_DEST_ZONE_SHIFT)
                                    | (7ull << FORZA_CONTROL_MBOX_SHIFT);
      uint32_t zone = (first >> FORZA_DEST_ZONE_SHIFT) & 15;
      uint32_t mailbox = (first >> FORZA_CONTROL_MBOX_SHIFT) & 7;
      if (!(first & ~route_mask) && zone < VX_CFG_NUM_CORES * VX_CFG_NUM_CLUSTERS) {
        const auto& tx = zone == core_id_ ? local_tx_ : external_tx_;
        if (tx.size() >= TX_DEPTH || actor.outstanding[mailbox] == 255) {
          continue;
        }
      }
    }
    --command_budget_;
    uint64_t value = isa_command(actor_id, op, first, second);
    if (op == ForzaType::SEND_WAIT && value == FORZA_TRY_BUSY) {
      continue;
    }
    trace->dst_data[selected].i = value;
    work.completed.set(selected);
  }
  work.lane = lane;
  if ((work.completed & trace->tmask) == trace->tmask) {
    trace->resume_warp = trace->fetch_stall;
    csr_rsp_out.send(work.request, 1);
  } else {
    csr_work_.push_back(work);
  }
}
#endif

#ifdef VX_CFG_FORZA_MMIO_ENABLE
uint64_t ForzaZone::mmio_command(const LsuReq& request, uint32_t lane) {
  uint64_t address = request.addrs[lane];
  uint32_t actor_id = request.wid * VX_CFG_NUM_THREADS + request.tids[lane];
  if (request.wid >= VX_CFG_NUM_WARPS || request.tids[lane] >= VX_CFG_NUM_THREADS
      || actor_id >= actors_.size() || (address & 7)) {
    return FORZA_MMIO_INVALID;
  }
  uint64_t endpoint = address;
  if (!update_address(address) && !record_address(address) && !vm_address(address)
      && address >= FORZA_MMIO_ENDPOINT_BASE) {
    uint64_t offset = address - FORZA_MMIO_ENDPOINT_BASE;
    if (offset / FORZA_MMIO_ACTOR_STRIDE != actor_id) {
      return FORZA_MMIO_INVALID;
    }
    endpoint = FORZA_MMIO_ENDPOINT_BASE + offset % FORZA_MMIO_ACTOR_STRIDE;
  }
  uint32_t byte_offset = address & (VX_CFG_MEM_BLOCK_SIZE - 1);
  bool swap = request.op == MemOp::AMO_SWAP && request.data[lane]
           && request.byteen[lane] == (0xffull << byte_offset);
  uint64_t operand = 0;
  if (swap) {
    for (uint32_t byte = 0; byte < 8; ++byte) {
      operand |= uint64_t(request.data[lane]->at(byte_offset + byte)) << (byte * 8);
    }
  }
  auto& actor = actors_[actor_id];
#ifdef VX_CFG_FORZA_VM_ENABLE
  if (address >= FORZA_VM_CONFIG_BASE && address - FORZA_VM_CONFIG_BASE < FORZA_VM_CONFIG_BYTES) {
    if (address == FORZA_VM_CONFIG_PENDING)
      return request.op == MemOp::LD ? vm_configs_.size() : FORZA_MMIO_INVALID;
    uint64_t word = (address - FORZA_VM_CONFIG_BASE) / 8;
    if (!swap || word >= FORZA_VM_PROGRAMS * FORZA_VM_PROGRAM_WORDS)
      return FORZA_MMIO_INVALID;
    if (vm_config_out.full() || vm_configs_.size() >= 4)
      return FORZA_MMIO_BUSY;
    uint64_t sequence = vm_config_sequence_++;
    vm_config_out.send(ForzaVmConfig{uint32_t(word / FORZA_VM_PROGRAM_WORDS),
                                   uint32_t(word % FORZA_VM_PROGRAM_WORDS), operand, sequence}, 1);
    vm_configs_.push_back(sequence);
    return FORZA_MMIO_ACCEPTED;
  }
  if (vm_address(address)) {
    bool direct = address >= FORZA_VM_DIRECT_BASE;
    uint64_t offset = address - (direct ? FORZA_VM_DIRECT_BASE : FORZA_VM_BASE);
    uint32_t zone = offset / FORZA_VM_ZONE_STRIDE;
    uint32_t program = (offset % FORZA_VM_ZONE_STRIDE) / FORZA_VM_PROGRAM_STRIDE;
    uint32_t region = (offset % FORZA_VM_PROGRAM_STRIDE) / 8;
    const auto& queue = actor.mailboxes[1];
    bool invalid_mode = direct
        ? (actor.registration & 2) || (!actor.record_direct
            && (actor.outstanding[0] || actor.outstanding[1]
                || queue.slots[0] != SlotState::Empty || queue.slots[1] != SlotState::Empty))
        : actor.record_direct || !(actor.registration & 2);
    if (!swap || actor.write_error || invalid_mode || zone >= VX_CFG_NUM_CORES * VX_CFG_NUM_CLUSTERS
        || program >= FORZA_VM_PROGRAMS || region >= FORZA_VM_REGIONS)
      return FORZA_MMIO_INVALID;
    auto& tx = zone == core_id_ ? local_tx_ : external_tx_;
    if (tx.size() >= TX_DEPTH || actor.outstanding[0] == 255)
      return FORZA_MMIO_BUSY;
    ForzaMessage message;
    message.source_zone = core_id_;
    message.source_actor = actor_id;
    message.dest_zone = zone;
    message.dest_pe = region;
    message.sequence = sequence_++;
    message.words[0] = forza_message_control(zone, region, 0,
        direct ? FORZA_VM_DIRECT_REQUEST_OPCODE : FORZA_VM_REQUEST_OPCODE);
    message.words[1] = operand;
    message.words[2] = direct ? actor_id : (actor.registration >> 8) & 0x7ff;
    message.words[3] = program;
    actor.record_direct = direct;
    tx.push_back(message);
    pending_.emplace(message.sequence, std::make_pair(actor_id, 0));
    ++actor.outstanding[0];
    return FORZA_MMIO_ACCEPTED;
  }
#endif
#ifdef VX_CFG_FORZA_RECORD_ENABLE
  if (record_address(address)) {
    bool direct = address >= FORZA_RECORD_DIRECT_BASE;
    uint64_t offset = address - (direct ? FORZA_RECORD_DIRECT_BASE : FORZA_RECORD_BASE);
    uint32_t zone = offset / FORZA_RECORD_ZONE_STRIDE;
    uint32_t record = (offset % FORZA_RECORD_ZONE_STRIDE) / 8;
    const auto& reply_queue = actor.mailboxes[FORZA_RECORD_REPLY_MAILBOX];
    bool mode_invalid = direct
        ? (actor.registration & (1u << FORZA_RECORD_REPLY_MAILBOX))
          || (!actor.record_direct
              && (actor.outstanding[FORZA_RECORD_REQUEST_MAILBOX]
                  || actor.outstanding[FORZA_RECORD_REPLY_MAILBOX]
                  || reply_queue.slots[0] != SlotState::Empty
                  || reply_queue.slots[1] != SlotState::Empty))
        : actor.record_direct
          || !(actor.registration & (1u << FORZA_RECORD_REPLY_MAILBOX));
    if (!swap || actor.write_error || offset >= FORZA_RECORD_VALID_BYTES
        || zone >= VX_CFG_NUM_CORES * VX_CFG_NUM_CLUSTERS
        || record >= FORZA_RECORDS_PER_ZONE
        || mode_invalid) {
      return FORZA_MMIO_INVALID;
    }
    auto& tx = zone == core_id_ ? local_tx_ : external_tx_;
    if (tx.size() >= TX_DEPTH || actor.outstanding[FORZA_RECORD_REQUEST_MAILBOX] == 255) {
      return FORZA_MMIO_BUSY;
    }
    ForzaMessage message;
    message.source_zone = core_id_;
    message.source_actor = actor_id;
    message.dest_zone = zone;
    message.dest_pe = record;
    message.mailbox = FORZA_RECORD_REQUEST_MAILBOX;
    message.sequence = sequence_++;
    message.words[0] = forza_message_control(zone, record, FORZA_RECORD_REQUEST_MAILBOX,
        direct ? FORZA_RECORD_DIRECT_REQUEST_OPCODE : FORZA_RECORD_REQUEST_OPCODE);
    message.words[1] = operand;
    message.words[2] = direct ? actor_id : (actor.registration >> 8) & 0x7ff;
    actor.record_direct = direct;
    tx.push_back(message);
    pending_.emplace(message.sequence, std::make_pair(actor_id, FORZA_RECORD_REQUEST_MAILBOX));
    ++actor.outstanding[FORZA_RECORD_REQUEST_MAILBOX];
    return FORZA_MMIO_ACCEPTED;
  }
#endif
#if defined(VX_CFG_FORZA_RECORD_ENABLE) || defined(VX_CFG_FORZA_VM_ENABLE)
  if (endpoint >= FORZA_RECORD_CQ_ID && endpoint <= FORZA_RECORD_CQ_RELEASE) {
    if (actor.registration & (1u << FORZA_RECORD_REPLY_MAILBOX)) {
      return FORZA_MMIO_INVALID;
    }
    auto& queue = actor.mailboxes[FORZA_RECORD_REPLY_MAILBOX];
    bool ready = actor.record_direct && queue.slots[queue.read_slot] == SlotState::Ready;
    if (endpoint == FORZA_RECORD_CQ_RELEASE) {
      if (!swap || !ready) {
        return FORZA_MMIO_INVALID;
      }
      queue.slots[queue.read_slot] = SlotState::Empty;
      queue.read_slot ^= 1;
      return FORZA_MMIO_ACCEPTED;
    }
    if (request.op != MemOp::LD) {
      return FORZA_MMIO_INVALID;
    }
    if (!ready) {
      return endpoint == FORZA_RECORD_CQ_ID ? 0 : FORZA_MMIO_INVALID;
    }
    uint32_t field = (endpoint - FORZA_RECORD_CQ_ID) / 8;
    uint64_t offset = slot_address(actor_id, FORZA_RECORD_REPLY_MAILBOX, queue.read_slot)
                    - FORZA_MAILBOX_BASE + 8 + field * 8;
    uint64_t value = 0;
    for (uint32_t byte = 0; byte < 8; ++byte) {
      value |= uint64_t(sram_[offset + byte]) << (byte * 8);
    }
    return field == 0 ? value + 1 : value;
  }
#endif
  if (endpoint >= FORZA_MMIO_RX_PEEK_BASE && endpoint < FORZA_MMIO_RX_PEEK_BASE + 64) {
    return request.op == MemOp::LD
         ? read_csr(actor_id, FORZA_CSR_ZQMDQ0 + (endpoint - FORZA_MMIO_RX_PEEK_BASE) / 8)
         : FORZA_MMIO_INVALID;
  }
  if (endpoint >= FORZA_MMIO_RX_RELEASE_BASE && endpoint < FORZA_MMIO_RX_RELEASE_BASE + 64) {
    uint32_t mailbox_id = (endpoint - FORZA_MMIO_RX_RELEASE_BASE) / 8;
    const auto& mailbox = actor.mailboxes[mailbox_id];
    if (!swap || !(actor.registration & (1u << mailbox_id))
        || mailbox.slots[mailbox.read_slot] != SlotState::Ready) {
      return FORZA_MMIO_INVALID;
    }
    write_csr(actor_id, FORZA_CSR_ZQMDQ0 + mailbox_id, operand);
    return FORZA_MMIO_ACCEPTED;
  }
#ifdef VX_CFG_FORZA_UPDATE_ENABLE
  if (update_address(address)) {
    uint64_t offset = address - FORZA_UPDATE_BASE;
    if (!swap || actor.write_error || offset >= FORZA_UPDATE_VALID_BYTES) {
      return FORZA_MMIO_INVALID;
    }
    uint32_t zone = offset / FORZA_UPDATE_ZONE_STRIDE;
    uint32_t word = (offset % FORZA_UPDATE_ZONE_STRIDE) / 8;
    if (zone >= VX_CFG_NUM_CORES * VX_CFG_NUM_CLUSTERS) {
      return FORZA_MMIO_INVALID;
    }
    auto& tx = zone == core_id_ ? local_tx_ : external_tx_;
    if (tx.size() >= TX_DEPTH || actor.outstanding[FORZA_UPDATE_MAILBOX] == 255) {
      return FORZA_MMIO_BUSY;
    }
    ForzaMessage message;
    message.source_zone = core_id_;
    message.source_actor = actor_id;
    message.dest_zone = zone;
    message.dest_pe = word;
    message.mailbox = FORZA_UPDATE_MAILBOX;
    message.sequence = sequence_++;
    message.words[0] = forza_message_control(zone, word, FORZA_UPDATE_MAILBOX,
                                             FORZA_UPDATE_OPCODE);
    message.words[1] = operand;
    tx.push_back(message);
    pending_.emplace(message.sequence, std::make_pair(actor_id, FORZA_UPDATE_MAILBOX));
    ++actor.outstanding[FORZA_UPDATE_MAILBOX];
    return FORZA_MMIO_ACCEPTED;
  }
#endif
  if (address - FORZA_MMIO_BASE < FORZA_MMIO_TX_BYTES) {
    if (!swap || actor.write_error) {
      return FORZA_MMIO_INVALID;
    }
    uint32_t destination = (address - FORZA_MMIO_BASE) >> 6;
    uint32_t zone = destination >> 11;
    uint32_t pe = destination & 0x7ff;
    uint32_t mailbox = (address >> 3) & 7;
    return enqueue_token(actor_id, zone, pe, mailbox, operand);
  }
  if (endpoint == FORZA_MMIO_REGISTER) {
    if (request.op == MemOp::LD) {
      return actor.registration;
    }
    if (!swap || operand >> 19 || actor.write_error) {
      return FORZA_MMIO_INVALID;
    }
    write_csr(actor_id, FORZA_CSR_ZQMMBOXREG, operand);
    return actor.write_error ? FORZA_MMIO_INVALID : FORZA_MMIO_ACCEPTED;
  }
  if (endpoint == FORZA_MMIO_RX_STATUS && request.op == MemOp::LD) {
    return read_csr(actor_id, FORZA_CSR_ZQMSTAT);
  }
  if (endpoint == FORZA_MMIO_TX_STATUS && request.op == MemOp::LD) {
    return read_csr(actor_id, FORZA_CSR_ZENSTAT);
  }
  if (endpoint == FORZA_MMIO_TX_COUNTS && request.op == MemOp::LD) {
    return read_csr(actor_id, FORZA_CSR_ZENCOUNT);
  }
  if (endpoint >= FORZA_MMIO_RX_POP_BASE && endpoint < FORZA_MMIO_RX_POP_BASE + 64) {
    if (!swap) {
      return FORZA_MMIO_INVALID;
    }
    uint32_t mailbox_id = (endpoint - FORZA_MMIO_RX_POP_BASE) / 8;
    return pop_token(actor_id, mailbox_id);
  }
  return FORZA_MMIO_INVALID;
}

bool ForzaZone::tick_mmio() {
#ifdef VX_CFG_FORZA_ISA_ENABLE
  if (!command_budget_) {
    return false;
  }
#endif
  if (trace_enabled() && (SimPlatform::instance().cycles() & 65535) == 0) {
    std::cerr << "FORZA MMIO_STATE cycle=" << SimPlatform::instance().cycles()
              << " zone=" << core_id_ << " local_tx=" << local_tx_.size()
              << " external_tx=" << external_tx_.size()
              << " incoming0=" << incoming_[0].size()
              << " incoming1=" << incoming_[1].size()
              << " filling=" << filling_.size() << " pending=" << pending_.size()
              << " net_tx=" << net_tx.size() << " net_rx=" << net_rx.size()
              << " ack_tx=" << ack_tx.size() << " ack_rx=" << ack_rx.size() << std::endl;
    for (uint32_t port = 0; port < mem_req_in.size(); ++port) {
      std::cerr << "FORZA MMIO_PORT zone=" << core_id_ << " port=" << port
                << " req=" << mem_req_in[port].size()
                << " rsp=" << mem_rsp_out[port].size()
                << " rsp_full=" << mem_rsp_out[port].full()
                << " lane=" << mmio_lane_[port];
      if (!mem_req_in[port].empty()) {
        std::cerr << " head=" << mem_req_in[port].peek();
      }
      std::cerr << std::endl;
    }
    for (uint32_t actor = 0; actor < actors_.size(); ++actor) {
      const auto& state = actors_[actor];
      const auto& mailbox = state.mailboxes[0];
      std::cerr << "FORZA MMIO_ACTOR zone=" << core_id_ << " actor=" << actor
                << " reg=" << state.registration << " out0=" << uint32_t(state.outstanding[0])
                << " out1=" << uint32_t(state.outstanding[1])
                << " cur=" << mailbox.read_slot << " next=" << mailbox.write_slot
                << " slot0=" << uint32_t(mailbox.slots[0])
                << " slot1=" << uint32_t(mailbox.slots[1]) << std::endl;
    }
  }
  // One selected LSU request uses the zone's scalar or vector command resource.
  for (uint32_t offset = 0; offset < mem_req_in.size(); ++offset) {
    uint32_t port = (mmio_port_rr_ + offset) % mem_req_in.size();
    if (mem_req_in[port].empty() || mem_rsp_out[port].full()) {
      continue;
    }
    const auto& request = mem_req_in[port].peek();
    if (!mmio_request(request)) {
      continue;
    }
    if (request.op == MemOp::ST) {
      throw std::runtime_error("FORZA MMIO requires acknowledged AMOSWAP.D, not posted stores");
    }
#if defined(VX_CFG_FORZA_MMIO_VECTOR_ENABLE) && !defined(VX_CFG_FORZA_ISA_ENABLE)
    uint32_t lane = 0;
#else
    uint32_t lane = mmio_lane_[port];
#endif
    while (lane < request.mask.size() && !request.mask.test(lane)) {
      ++lane;
    }
    if (lane == request.mask.size()) {
      throw std::runtime_error("FORZA MMIO received an empty lane fragment");
    }
    LsuRsp response(request.mask.size());
    response.tag = request.tag;
    response.cid = request.cid;
    response.uuid = request.uuid;
    do {
#ifdef VX_CFG_FORZA_ISA_ENABLE
      --command_budget_;
#endif
      response.mask.set(lane);
      response.data[lane] = std::make_shared<mem_block_t>();
      response.data[lane]->fill(0);
      uint64_t address = request.addrs[lane];
      if (mmio_address(address)) {
        uint64_t value = mmio_command(request, lane);
        uint32_t byte_offset = address & (VX_CFG_MEM_BLOCK_SIZE - 1);
        if (trace_enabled()) {
          std::cerr << "FORZA MMIO_CMD cycle=" << SimPlatform::instance().cycles()
                    << " zone=" << core_id_ << " port=" << port << " wid=" << request.wid
                    << " lane=" << lane << " tid=" << request.tids[lane]
                    << " actor=" << request.wid * VX_CFG_NUM_THREADS + request.tids[lane]
                    << " op=" << request.op << " addr=0x" << std::hex << address
                    << " result=0x" << value << std::dec << " tag=" << request.tag
                    << " mask=" << request.mask << std::endl;
        }
        for (uint32_t byte = 0; byte < 8 && byte_offset + byte < VX_CFG_MEM_BLOCK_SIZE; ++byte) {
          response.data[lane]->at(byte_offset + byte) = value >> (byte * 8);
        }
      } else {
        if (request.op != MemOp::LD || address < FORZA_MAILBOX_BASE) {
          throw std::runtime_error("invalid mixed FORZA MMIO/SRAM request");
        }
        uint64_t sram_offset = (address - FORZA_MAILBOX_BASE)
                             & ~uint64_t(VX_CFG_MEM_BLOCK_SIZE - 1);
        if (sram_offset + VX_CFG_MEM_BLOCK_SIZE > sram_.size()) {
          throw std::runtime_error("mixed FORZA SRAM load outside instantiated SRAM");
        }
        std::memcpy(response.data[lane]->data(), sram_.data() + sram_offset,
                    VX_CFG_MEM_BLOCK_SIZE);
      }
#ifdef VX_CFG_FORZA_MMIO_VECTOR_ENABLE
      do {
        ++lane;
      } while (lane < request.mask.size() && !request.mask.test(lane));
#else
      break;
#endif
    } while (lane < request.mask.size()
#ifdef VX_CFG_FORZA_ISA_ENABLE
             && command_budget_
#endif
             );
    mem_rsp_out[port].send(response, 1);
#if defined(VX_CFG_FORZA_MMIO_VECTOR_ENABLE) && !defined(VX_CFG_FORZA_ISA_ENABLE)
    mem_req_in[port].pop();
    mmio_lane_[port] = 0;
#else
#ifndef VX_CFG_FORZA_MMIO_VECTOR_ENABLE
    do {
      ++lane;
    } while (lane < request.mask.size() && !request.mask.test(lane));
#endif
    if (lane == request.mask.size()) {
      mem_req_in[port].pop();
      lane = 0;
    }
    mmio_lane_[port] = lane;
#endif
    mmio_port_rr_ = (port + 1) % mem_req_in.size();
    return true;
  }
  return false;
}
#endif

void ForzaZone::tick_memory() {
#ifdef VX_CFG_FORZA_MMIO_ENABLE
#ifdef VX_CFG_FORZA_MMIO_VECTOR_ENABLE
  if (tick_mmio()) {
    return;
  }
#else
  tick_mmio();
#endif
#endif
  for (uint32_t port = 0; port < mem_req_in.size(); ++port) {
    if (mem_req_in[port].empty() || mem_rsp_out[port].full()) {
      continue;
    }
    const auto& request = mem_req_in[port].peek();
#ifdef VX_CFG_FORZA_MMIO_ENABLE
    if (mmio_request(request)) {
      continue;
    }
#endif
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
#if defined(VX_CFG_FORZA_RECORD_ENABLE) || defined(VX_CFG_FORZA_VM_ENABLE)
  if (!reply_net_rx.empty()) {
    const auto& reply = reply_net_rx.peek();
    uint32_t opcode = (reply.words[0] >> FORZA_CONTROL_OPCODE_SHIFT) & 255;
    bool valid = false;
#ifdef VX_CFG_FORZA_RECORD_ENABLE
    valid = reply.source_actor == ACTORS
         && (opcode == FORZA_RECORD_REPLY_OPCODE || opcode == FORZA_RECORD_DIRECT_REPLY_OPCODE);
#endif
#ifdef VX_CFG_FORZA_VM_ENABLE
    valid = reply.source_actor == ACTORS + 1
         && (opcode == FORZA_VM_REPLY_OPCODE || opcode == FORZA_VM_DIRECT_REPLY_OPCODE);
#endif
    if (reply.dest_zone != core_id_ || reply.mailbox != FORZA_RECORD_REPLY_MAILBOX || !valid) {
      throw std::runtime_error("FORZA record reply channel identity mismatch");
    }
    auto& queue = incoming_[FORZA_RECORD_REPLY_MAILBOX];
    if (queue.size() < RX_DEPTH) {
      queue.push_back(reply);
      reply_net_rx.pop();
    }
  }
#endif
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
#ifdef VX_CFG_FORZA_VM_ENABLE
    uint32_t vm_opcode = (message.words[0] >> FORZA_CONTROL_OPCODE_SHIFT) & 255;
    if (vm_opcode == FORZA_VM_REQUEST_OPCODE || vm_opcode == FORZA_VM_DIRECT_REQUEST_OPCODE) {
      if (!vm_request_out.full()) {
        vm_request_out.send(message, 1);
        net_rx.pop();
      }
    } else {
#endif
#ifdef VX_CFG_FORZA_UPDATE_ENABLE
    uint32_t opcode = (message.words[0] >> FORZA_CONTROL_OPCODE_SHIFT) & 255;
#ifdef VX_CFG_FORZA_RECORD_ENABLE
    if (opcode == FORZA_RECORD_REQUEST_OPCODE || opcode == FORZA_RECORD_DIRECT_REQUEST_OPCODE) {
      if (record_queue_.size() < FORZA_RECORD_QUEUE_DEPTH) {
        record_queue_.push_back(message);
        net_rx.pop();
      }
    } else
#endif
    if (opcode == FORZA_UPDATE_OPCODE) {
      if (update_queue_.size() < FORZA_UPDATE_QUEUE_DEPTH) {
        update_queue_.push_back(message);
        net_rx.pop();
      }
    } else {
#endif
      auto& queue = incoming_[message.mailbox];
      if (queue.size() < RX_DEPTH) {
        queue.push_back(message);
        net_rx.pop();
      }
#ifdef VX_CFG_FORZA_UPDATE_ENABLE
    }
#endif
#ifdef VX_CFG_FORZA_VM_ENABLE
    }
#endif
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
#ifdef VX_CFG_FORZA_RECORD_ENABLE
    if (((message.words[0] >> FORZA_CONTROL_OPCODE_SHIFT) & 255)
        == FORZA_RECORD_DIRECT_REPLY_OPCODE) {
      actor_id = message.dest_pe;
      valid = mb == FORZA_RECORD_REPLY_MAILBOX && message.source_actor == ACTORS
           && actor_id < ACTORS && actors_[actor_id].record_direct
           && !(actors_[actor_id].registration & (1u << mb));
    }
#endif
#ifdef VX_CFG_FORZA_VM_ENABLE
    if (((message.words[0] >> FORZA_CONTROL_OPCODE_SHIFT) & 255) == FORZA_VM_DIRECT_REPLY_OPCODE) {
      actor_id = message.dest_pe;
      valid = mb == 1 && message.source_actor == ACTORS + 1 && actor_id < ACTORS
           && actors_[actor_id].record_direct && !(actors_[actor_id].registration & 2);
    }
#endif
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

#ifdef VX_CFG_FORZA_UPDATE_ENABLE
bool ForzaZone::tick_update(bool allow_issue) {
  if (!update_rsp_in.empty() && !ack_tx.full()
#ifdef VX_CFG_FORZA_RECORD_ENABLE
      && update_rsp_in.peek().tag < FORZA_UPDATE_INFLIGHT
#endif
      ) {
    const auto& response = update_rsp_in.peek();
    if (response.tag >= FORZA_UPDATE_INFLIGHT
        || !update_pending_.contains(response.tag)) {
      throw std::runtime_error("FORZA update returned an unknown LocalMem tag");
    }
    const auto& message = update_pending_.at(response.tag);
    uint32_t engine_hart = make_hart_id(VX_CFG_NUM_CORES * VX_CFG_NUM_CLUSTERS, 0, 0) + core_id_;
    uint64_t expected_uuid = (message.sequence << 4) | message.source_zone;
    if (response.hart_id != engine_hart || response.uuid != expected_uuid || !response.data) {
      throw std::runtime_error("FORZA update LocalMem response identity mismatch");
    }
    ack_tx.send(ForzaAck{message.source_zone, message.source_actor,
                        FORZA_UPDATE_MAILBOX, message.sequence, false}, 1);
    if (trace_enabled()) {
      std::cerr << "FORZA UPDATE complete zone=" << core_id_
                << " tag=" << response.tag << " seq=" << message.sequence << std::endl;
    }
    update_pending_.release(response.tag);
    update_rsp_in.pop();
  }
  if (update_queue_.empty()) {
    return false;
  }
  const auto& message = update_queue_.front();
  if (message.dest_zone != core_id_ || message.dest_pe >= FORZA_UPDATE_WORDS_PER_ZONE
      || message.mailbox != FORZA_UPDATE_MAILBOX) {
    if (!ack_tx.full()) {
      ack_tx.send(ForzaAck{message.source_zone, message.source_actor,
                          message.mailbox, message.sequence, true}, 1);
      update_queue_.pop_front();
    }
    return false;
  }
  if (!allow_issue || update_req_out.full() || update_pending_.full()) {
    return false;
  }
  MemReq request;
  request.op = MemOp::AMO_ADD;
  request.addr = VX_MEM_LMEM_BASE_ADDR + uint64_t(message.dest_pe) * 8;
  request.tag = update_pending_.allocate(message);
  request.hart_id = make_hart_id(VX_CFG_NUM_CORES * VX_CFG_NUM_CLUSTERS, 0, 0) + core_id_;
  request.uuid = (message.sequence << 4) | message.source_zone;
  request.flags.local = true;
  request.data = std::make_shared<mem_block_t>();
  request.data->fill(0);
  uint32_t offset = request.addr & (VX_CFG_MEM_BLOCK_SIZE - 1);
  request.byteen = 0xffull << offset;
  for (uint32_t byte = 0; byte < 8; ++byte) {
    request.data->at(offset + byte) = message.words[1] >> (byte * 8);
  }
  update_req_out.send(request, 1);
  if (trace_enabled()) {
    std::cerr << "FORZA UPDATE issue zone=" << core_id_ << " tag=" << request.tag
              << " word=" << message.dest_pe << " delta=0x" << std::hex
              << message.words[1] << std::dec << std::endl;
  }
  update_queue_.pop_front();
  return true;
}
#endif


#ifdef VX_CFG_FORZA_RECORD_ENABLE
bool ForzaZone::tick_record(bool allow_issue) {
  for (auto& context : record_contexts_) {
    if (context.stage == RecordStage::AckReady && !ack_tx.full()) {
      const auto& message = context.request;
      ack_tx.send(ForzaAck{message.source_zone, message.source_actor,
                          FORZA_RECORD_REQUEST_MAILBOX, message.sequence,
                          context.reply_error}, 1);
      context = RecordContext{};
      break;
    }
  }
  for (uint32_t index = 0; index < record_contexts_.size(); ++index) {
    auto& context = record_contexts_[index];
    if (context.stage != RecordStage::ReplyReady) {
      continue;
    }
    const auto& original = context.request;
    if (reply_net_tx.full()) {
      continue;
    }
    ForzaMessage reply;
    reply.source_zone = core_id_;
    reply.source_actor = ACTORS;
    reply.dest_zone = original.source_zone;
    reply.dest_pe = original.words[2];
    reply.mailbox = FORZA_RECORD_REPLY_MAILBOX;
    reply.sequence = sequence_++;
    bool direct = ((original.words[0] >> FORZA_CONTROL_OPCODE_SHIFT) & 255)
               == FORZA_RECORD_DIRECT_REQUEST_OPCODE;
    reply.words[0] = forza_message_control(reply.dest_zone, reply.dest_pe, reply.mailbox,
        direct ? FORZA_RECORD_DIRECT_REPLY_OPCODE : FORZA_RECORD_REPLY_OPCODE);
    reply.words[1] = uint32_t(original.words[1] >> 32);
    for (uint32_t field = 0; field < 3; ++field) {
      reply.words[2 + field] = context.old_values[field];
    }
    record_replies_.emplace(reply.sequence, index);
    reply_net_tx.send(reply, 1);
    context.stage = RecordStage::ReplyFlight;
    break;
  }
  for (auto& context : record_contexts_) {
    if (context.stage != RecordStage::Compute) {
      continue;
    }
    context.new_values[0] = context.old_values[0] + 1;
    context.new_values[1] = context.old_values[1]
                         + uint64_t(int64_t(int32_t(uint32_t(context.request.words[1]))));
    context.new_values[2] = context.old_values[2] ^ uint32_t(context.request.words[1] >> 32);
    context.issued = 0;
    context.completed = 0;
    context.stage = RecordStage::Writing;
    break;
  }
  if (!update_rsp_in.empty() && update_rsp_in.peek().tag >= FORZA_UPDATE_INFLIGHT) {
    const auto& response = update_rsp_in.peek();
    uint64_t offset = response.tag - FORZA_UPDATE_INFLIGHT;
    if (offset >= record_contexts_.size() * 6) {
      throw std::runtime_error("FORZA record returned an unknown LocalMem tag");
    }
    uint32_t index = offset / 6;
    uint32_t operation = offset % 6;
    auto& context = record_contexts_[index];
    bool writing = operation >= 3;
    uint32_t field = operation % 3;
    uint64_t expected_uuid = (context.request.sequence << 8)
                           | (uint64_t(context.request.source_zone) << 4) | operation;
    uint32_t engine_hart = make_hart_id(VX_CFG_NUM_CORES * VX_CFG_NUM_CLUSTERS, 0, 0) + core_id_;
    if (context.stage != (writing ? RecordStage::Writing : RecordStage::Reading)
        || !(context.issued & (1u << field)) || (context.completed & (1u << field))
        || response.uuid != expected_uuid || response.hart_id != engine_hart
        || (!writing && !response.data)) {
      throw std::runtime_error("FORZA record LocalMem response identity mismatch");
    }
    if (!writing) {
      uint64_t address = VX_MEM_LMEM_BASE_ADDR
                       + uint64_t(context.request.dest_pe) * FORZA_RECORD_BYTES + field * 8;
      uint32_t byte_offset = address & (VX_CFG_MEM_BLOCK_SIZE - 1);
      uint64_t value = 0;
      for (uint32_t byte = 0; byte < 8; ++byte) {
        value |= uint64_t(response.data->at(byte_offset + byte)) << (byte * 8);
      }
      context.old_values[field] = value;
    }
    context.completed |= 1u << field;
    if (context.completed == 7) {
      context.stage = writing ? RecordStage::ReplyReady : RecordStage::Compute;
    }
    update_rsp_in.pop();
  }
  if (!record_queue_.empty()) {
    const auto& message = record_queue_.front();
    bool direct = ((message.words[0] >> FORZA_CONTROL_OPCODE_SHIFT) & 255)
               == FORZA_RECORD_DIRECT_REQUEST_OPCODE;
    if (message.dest_zone != core_id_ || message.dest_pe >= FORZA_RECORDS_PER_ZONE
        || message.mailbox != FORZA_RECORD_REQUEST_MAILBOX
        || message.words[2] >= (direct ? ACTORS : 2048)
        || (direct && message.words[2] != message.source_actor)
        || message.source_zone >= VX_CFG_NUM_CORES * VX_CFG_NUM_CLUSTERS
        || message.source_actor >= ACTORS) {
      if (!ack_tx.full()) {
        ack_tx.send(ForzaAck{message.source_zone, message.source_actor, message.mailbox,
                            message.sequence, true}, 1);
        record_queue_.pop_front();
      }
    } else {
      bool locked = false;
      uint32_t free_index = record_contexts_.size();
      for (uint32_t index = 0; index < record_contexts_.size(); ++index) {
        const auto& context = record_contexts_[index];
        if (context.stage == RecordStage::Empty && free_index == record_contexts_.size()) {
          free_index = index;
        }
        if ((context.stage == RecordStage::Reading || context.stage == RecordStage::Compute
             || context.stage == RecordStage::Writing)
            && context.request.dest_pe == message.dest_pe) {
          locked = true;
        }
      }
      if (!locked && free_index < record_contexts_.size()) {
        auto& context = record_contexts_[free_index];
        context = RecordContext{};
        context.request = message;
        context.stage = RecordStage::Reading;
        record_queue_.pop_front();
      }
    }
  }
  if (!allow_issue || update_req_out.full()) {
    return false;
  }
  for (uint32_t step = 0; step < record_contexts_.size(); ++step) {
    uint32_t index = (record_issue_rr_ + step) % record_contexts_.size();
    auto& context = record_contexts_[index];
    bool writing = context.stage == RecordStage::Writing;
    if ((!writing && context.stage != RecordStage::Reading) || context.issued == 7) {
      continue;
    }
    uint32_t field = 0;
    while (context.issued & (1u << field)) {
      ++field;
    }
    uint32_t operation = (writing ? 3 : 0) + field;
    MemReq request;
    request.op = writing ? MemOp::ST : MemOp::LD;
    request.addr = VX_MEM_LMEM_BASE_ADDR
                 + uint64_t(context.request.dest_pe) * FORZA_RECORD_BYTES + field * 8;
    request.tag = FORZA_UPDATE_INFLIGHT + index * 6 + operation;
    request.hart_id = make_hart_id(VX_CFG_NUM_CORES * VX_CFG_NUM_CLUSTERS, 0, 0) + core_id_;
    request.uuid = (context.request.sequence << 8)
                 | (uint64_t(context.request.source_zone) << 4) | operation;
    request.flags.local = true;
    request.flags.strsp = writing;
    uint32_t byte_offset = request.addr & (VX_CFG_MEM_BLOCK_SIZE - 1);
    request.byteen = 0xffull << byte_offset;
    if (writing) {
      request.data = std::make_shared<mem_block_t>();
      request.data->fill(0);
      for (uint32_t byte = 0; byte < 8; ++byte) {
        request.data->at(byte_offset + byte) = context.new_values[field] >> (byte * 8);
      }
    }
    update_req_out.send(request, 1);
    context.issued |= 1u << field;
    record_issue_rr_ = (index + 1) % record_contexts_.size();
    return true;
  }
  return false;
}
#endif

#ifdef VX_CFG_FORZA_VM_ENABLE
void ForzaZone::tick_vm() {
  if (!vm_config_ack_in.empty()) {
    if (vm_configs_.empty() || vm_configs_.front() != vm_config_ack_in.peek())
      throw std::runtime_error("FORZA VM configuration acknowledgement mismatch");
    vm_configs_.pop_front();
    vm_config_ack_in.pop();
  }
  if (!vm_completion_in.empty() && !ack_tx.full()) {
    ack_tx.send(vm_completion_in.peek(), 1);
    vm_completion_in.pop();
  }
  if (!vm_result_in.empty() && !reply_net_tx.full()) {
    const auto& result = vm_result_in.peek();
    const auto& original = result.request;
    if (vm_replies_.size() >= FORZA_VM_CONTEXTS)
      throw std::runtime_error("FORZA VM result contexts exceeded");
    bool direct = ((original.words[0] >> FORZA_CONTROL_OPCODE_SHIFT) & 255)
               == FORZA_VM_DIRECT_REQUEST_OPCODE;
    ForzaMessage reply;
    reply.source_zone = core_id_;
    reply.source_actor = ACTORS + 1;
    reply.dest_zone = original.source_zone;
    reply.dest_pe = original.words[2];
    reply.mailbox = 1;
    reply.sequence = sequence_++;
    reply.words[0] = forza_message_control(reply.dest_zone, reply.dest_pe, 1,
        direct ? FORZA_VM_DIRECT_REPLY_OPCODE : FORZA_VM_REPLY_OPCODE);
    reply.words[1] = uint32_t(original.words[1] >> 32);
    for (uint32_t i = 0; i < 3; ++i) reply.words[2 + i] = result.values[i];
    vm_replies_.emplace(reply.sequence, original);
    reply_net_tx.send(reply, 1);
    vm_result_in.pop();
  }
}
#endif

void ForzaZone::on_tick() {
  if (!ack_rx.empty()) {
    const auto& ack = ack_rx.peek();
#ifdef VX_CFG_FORZA_VM_ENABLE
    if (ack.actor == ACTORS + 1) {
      auto pending = vm_replies_.find(ack.sequence);
      if (ack.dest_zone != core_id_ || ack.mailbox != 1 || pending == vm_replies_.end())
        throw std::runtime_error("FORZA VM reply acknowledgement mismatch");
      if (!vm_result_ack_out.full()) {
        const auto& original = pending->second;
        vm_result_ack_out.send(ForzaAck{original.source_zone, original.source_actor,
                                       original.mailbox, original.sequence, ack.error}, 1);
        vm_replies_.erase(pending);
        ack_rx.pop();
      }
    } else {
#endif
#ifdef VX_CFG_FORZA_RECORD_ENABLE
    if (ack.actor == ACTORS) {
      auto reply = record_replies_.find(ack.sequence);
      if (ack.dest_zone != core_id_ || ack.mailbox != FORZA_RECORD_REPLY_MAILBOX
          || reply == record_replies_.end()
          || record_contexts_[reply->second].stage != RecordStage::ReplyFlight) {
        throw std::runtime_error("FORZA record reply acknowledgement identity mismatch");
      }
      auto& context = record_contexts_[reply->second];
      context.reply_error = ack.error;
      context.stage = RecordStage::AckReady;
      record_replies_.erase(reply);
      ack_rx.pop();
    } else {
#endif
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
#ifdef VX_CFG_FORZA_RECORD_ENABLE
    }
#endif
#ifdef VX_CFG_FORZA_VM_ENABLE
    }
#endif
  }
#ifdef VX_CFG_FORZA_VM_ENABLE
  tick_vm();
#endif
#ifdef VX_CFG_FORZA_UPDATE_ENABLE
#ifdef VX_CFG_FORZA_RECORD_ENABLE
  if (record_memory_turn_) {
    bool issued = tick_record(true);
    tick_update(!issued);
  } else {
    bool issued = tick_update(true);
    tick_record(!issued);
  }
  record_memory_turn_ = !record_memory_turn_;
#else
  tick_update();
#endif
#endif
  tick_receive();
#ifdef VX_CFG_FORZA_ISA_ENABLE
#ifdef VX_CFG_FORZA_MMIO_VECTOR_ENABLE
  command_budget_ = VX_CFG_NUM_LSU_LANES;
#else
  command_budget_ = 1;
#endif
  if (isa_turn_) {
    tick_csr();
    tick_memory();
  } else {
    tick_memory();
    tick_csr();
  }
  isa_turn_ = !isa_turn_;
#else
  tick_memory();
  tick_csr();
#endif
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
