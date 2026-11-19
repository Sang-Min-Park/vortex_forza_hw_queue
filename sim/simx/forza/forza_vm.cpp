#include "forza_vm.h"

#include <stdexcept>

namespace vortex {

ForzaVm::ForzaVm(const SimContext& ctx, const char* name, uint32_t core_id)
  : SimObject<ForzaVm>(ctx, name)
  , request_in(this, 2), result_out(this, 2), result_ack_in(this, 2)
  , completion_out(this, 2), config_in(this, 2), config_ack_out(this, 2)
  , memory_req_out(this, 2), memory_rsp_in(this, 2), core_id_(core_id) {
  static_assert(FORZA_VM_REGIONS * FORZA_VM_REGION_BYTES == (1u << VX_CFG_LMEM_LOG_SIZE),
                "FORZA VM regions must fit the configured LocalMem");
}

void ForzaVm::on_reset() {
  requests_.clear();
  contexts_.fill(Context{});
  for (auto& program : programs_) program.fill(0);
  valid_.fill(0);
  execute_rr_ = fetch_rr_ = 0;
}

uint32_t ForzaVm::hart(uint32_t context) const {
  return make_hart_id(VX_CFG_NUM_CORES * VX_CFG_NUM_CLUSTERS + 1, 0, 0)
       + core_id_ * FORZA_VM_CONTEXTS + context;
}

void ForzaVm::execute(uint32_t index) {
  auto& c = contexts_[index];
  uint64_t code = c.instruction;
  uint32_t op = code & 255;
  uint32_t dst = (code >> 8) & 7, a = (code >> 12) & 7, b = (code >> 16) & 7;
  int64_t imm = int32_t(code >> 32);
  if (++c.steps > FORZA_VM_STEP_LIMIT)
    throw std::runtime_error("FORZA VM instruction budget exceeded");
  switch (op) {
  case FORZA_VM_LD:
  case FORZA_VM_ST: {
    if (memory_req_out.full()) {
      --c.steps;
      return;
    }
    uint64_t address = c.regs[a] + uint64_t(imm);
    uint64_t base = VX_MEM_LMEM_BASE_ADDR + uint64_t(c.request.dest_pe) * FORZA_VM_REGION_BYTES;
    if ((address & 7) || address < base || address - base > FORZA_VM_REGION_BYTES - 8)
      throw std::runtime_error("FORZA VM access outside reserved region");
    MemReq request;
    request.op = op == FORZA_VM_LD ? MemOp::LD : MemOp::ST;
    request.addr = address;
    request.tag = index;
    request.hart_id = hart(index);
    request.uuid = (c.request.sequence << 16) | (uint64_t(c.request.source_zone) << 12)
                 | (uint64_t(c.steps) << 3) | index;
    request.flags.local = true;
    request.flags.strsp = op == FORZA_VM_ST;
    uint32_t offset = address & (VX_CFG_MEM_BLOCK_SIZE - 1);
    request.byteen = 0xffull << offset;
    if (op == FORZA_VM_ST) {
      request.data = std::make_shared<mem_block_t>();
      request.data->fill(0);
      for (uint32_t byte = 0; byte < 8; ++byte)
        request.data->at(offset + byte) = c.regs[dst] >> (byte * 8);
    }
    memory_req_out.send(request, 1);
    c.memory_uuid = request.uuid;
    c.memory_address = address;
    c.memory_load = op == FORZA_VM_LD;
    c.load_register = dst;
    c.stage = Stage::Memory;
    return;
  }
  case FORZA_VM_ADD: c.regs[dst] = c.regs[a] + c.regs[b]; break;
  case FORZA_VM_XOR: c.regs[dst] = c.regs[a] ^ c.regs[b]; break;
  case FORZA_VM_AND: c.regs[dst] = c.regs[a] & c.regs[b]; break;
  case FORZA_VM_ADDI: c.regs[dst] = c.regs[a] + uint64_t(imm); break;
  case FORZA_VM_IMM: c.regs[dst] = uint64_t(imm); break;
  case FORZA_VM_SEXT32: c.regs[dst] = uint64_t(int64_t(int32_t(c.regs[a]))); break;
  case FORZA_VM_BGE:
  case FORZA_VM_BEQ: {
    bool taken = op == FORZA_VM_BGE ? int64_t(c.regs[a]) >= int64_t(c.regs[b])
                                   : c.regs[a] == c.regs[b];
    if (taken) {
      if (imm < 0 || imm >= FORZA_VM_PROGRAM_WORDS)
        throw std::runtime_error("FORZA VM branch target outside program");
      c.pc = imm;
      c.stage = Stage::Fetch;
      return;
    }
    break;
  }
  case FORZA_VM_RET:
    c.result = {c.regs[dst], c.regs[a], c.regs[b]};
    c.stage = Stage::Output;
    return;
  default:
    throw std::runtime_error("FORZA VM unsupported instruction");
  }
  ++c.pc;
  c.stage = Stage::Fetch;
}

void ForzaVm::on_tick() {
  std::array<bool, FORZA_VM_CONTEXTS> fetchable{};
  for (uint32_t i = 0; i < contexts_.size(); ++i)
    fetchable[i] = contexts_[i].stage == Stage::Fetch;

  if (!config_in.empty() && !config_ack_out.full()) {
    bool busy = !requests_.empty() || !request_in.empty();
    for (const auto& c : contexts_) busy |= c.stage != Stage::Empty;
    const auto& config = config_in.peek();
    if (busy || config.program >= FORZA_VM_PROGRAMS || config.pc >= FORZA_VM_PROGRAM_WORDS)
      throw std::runtime_error("FORZA VM invalid or concurrent program configuration");
    programs_[config.program][config.pc] = config.instruction;
    valid_[config.program] |= 1u << config.pc;
    config_ack_out.send(config.sequence, 1);
    config_in.pop();
  }

  if (!memory_rsp_in.empty()) {
    const auto& response = memory_rsp_in.peek();
    if (response.tag >= contexts_.size())
      throw std::runtime_error("FORZA VM unknown memory tag");
    uint32_t index = response.tag;
    auto& c = contexts_[index];
    if (c.stage != Stage::Memory || response.hart_id != hart(index)
        || response.uuid != c.memory_uuid || (c.memory_load && !response.data))
      throw std::runtime_error("FORZA VM memory response identity mismatch");
    if (c.memory_load) {
      uint32_t offset = c.memory_address & (VX_CFG_MEM_BLOCK_SIZE - 1);
      uint64_t value = 0;
      for (uint32_t byte = 0; byte < 8; ++byte)
        value |= uint64_t(response.data->at(offset + byte)) << (byte * 8);
      c.regs[c.load_register] = value;
    }
    ++c.pc;
    c.stage = Stage::Fetch;
    memory_rsp_in.pop();
  }

  if (!result_ack_in.empty()) {
    const auto& ack = result_ack_in.peek();
    bool matched = false;
    for (auto& c : contexts_) {
      if (c.stage == Stage::Reply && c.request.sequence == ack.sequence
          && c.request.source_zone == ack.dest_zone && c.request.source_actor == ack.actor
          && c.request.mailbox == ack.mailbox) {
        c.reply_error = ack.error;
        c.stage = Stage::Complete;
        matched = true;
        break;
      }
    }
    if (!matched) throw std::runtime_error("FORZA VM unmatched result acknowledgement");
    result_ack_in.pop();
  }

  for (auto& c : contexts_) {
    if (c.stage == Stage::Complete && !completion_out.full()) {
      completion_out.send(ForzaAck{c.request.source_zone, c.request.source_actor,
                                  c.request.mailbox, c.request.sequence, c.reply_error}, 1);
      c = Context{};
      break;
    }
  }
  for (auto& c : contexts_) {
    if (c.stage == Stage::Output && !result_out.full()) {
      result_out.send(ForzaVmResult{c.request, c.result}, 1);
      c.stage = Stage::Reply;
      break;
    }
  }

  if (!request_in.empty() && requests_.size() < FORZA_VM_QUEUE_DEPTH) {
    const auto& message = request_in.peek();
    uint32_t opcode = (message.words[0] >> FORZA_CONTROL_OPCODE_SHIFT) & 255;
    bool direct = opcode == FORZA_VM_DIRECT_REQUEST_OPCODE;
    if (message.dest_zone != core_id_ || message.dest_pe >= FORZA_VM_REGIONS
        || message.words[3] >= FORZA_VM_PROGRAMS || message.source_actor >= VX_CFG_NUM_WARPS * VX_CFG_NUM_THREADS
        || message.source_zone >= VX_CFG_NUM_CORES * VX_CFG_NUM_CLUSTERS || message.mailbox != 0
        || (opcode != FORZA_VM_REQUEST_OPCODE && !direct)
        || message.words[2] >= (direct ? VX_CFG_NUM_WARPS * VX_CFG_NUM_THREADS : 2048)
        || (direct && message.words[2] != message.source_actor))
      throw std::runtime_error("FORZA VM malformed request");
    requests_.push_back(message);
    request_in.pop();
  }
  if (!requests_.empty()) {
    const auto& request = requests_.front();
    uint32_t free = contexts_.size();
    bool conflict = false;
    for (uint32_t i = 0; i < contexts_.size(); ++i) {
      const auto& c = contexts_[i];
      if (c.stage == Stage::Empty && free == contexts_.size()) free = i;
      if ((c.stage == Stage::Fetch || c.stage == Stage::Execute || c.stage == Stage::Memory)
          && c.request.dest_pe == request.dest_pe) conflict = true;
    }
    if (free < contexts_.size() && !conflict) {
      auto& c = contexts_[free];
      c = Context{};
      c.request = request;
      c.regs[0] = VX_MEM_LMEM_BASE_ADDR + uint64_t(request.dest_pe) * FORZA_VM_REGION_BYTES;
      c.regs[1] = uint32_t(request.words[1]);
      c.regs[2] = uint32_t(request.words[1] >> 32);
      c.stage = Stage::Fetch;
      requests_.pop_front();
    }
  }

  for (uint32_t n = 0; n < contexts_.size(); ++n) {
    uint32_t i = (execute_rr_ + n) % contexts_.size();
    if (contexts_[i].stage == Stage::Execute) {
      execute(i);
      execute_rr_ = (i + 1) % contexts_.size();
      break;
    }
  }
  for (uint32_t n = 0; n < contexts_.size(); ++n) {
    uint32_t i = (fetch_rr_ + n) % contexts_.size();
    auto& c = contexts_[i];
    if (fetchable[i] && c.stage == Stage::Fetch) {
      uint32_t program = c.request.words[3];
      if (c.pc >= FORZA_VM_PROGRAM_WORDS || !(valid_[program] & (1u << c.pc)))
        throw std::runtime_error("FORZA VM fetch from unconfigured instruction");
      c.instruction = programs_[program][c.pc];
      c.stage = Stage::Execute;
      fetch_rr_ = (i + 1) % contexts_.size();
      break;
    }
  }
}

}
