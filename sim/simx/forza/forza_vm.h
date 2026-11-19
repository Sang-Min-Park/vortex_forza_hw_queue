#pragma once

#include <deque>
#include <forza_vm_abi.h>
#include "forza_types.h"

namespace vortex {

struct ForzaVmConfig {
  uint32_t program;
  uint32_t pc;
  uint64_t instruction;
  uint64_t sequence;
};

struct ForzaVmResult {
  ForzaMessage request;
  std::array<uint64_t, 3> values;
};

class ForzaVm : public SimObject<ForzaVm> {
public:
  ForzaVm(const SimContext& ctx, const char* name, uint32_t core_id);

  SimChannel<ForzaMessage> request_in;
  SimChannel<ForzaVmResult> result_out;
  SimChannel<ForzaAck> result_ack_in;
  SimChannel<ForzaAck> completion_out;
  SimChannel<ForzaVmConfig> config_in;
  SimChannel<uint64_t> config_ack_out;
  SimChannel<MemReq> memory_req_out;
  SimChannel<MemRsp> memory_rsp_in;

protected:
  void on_reset();
  void on_tick();

private:
  friend class SimObject<ForzaVm>;
  enum class Stage { Empty, Fetch, Execute, Memory, Output, Reply, Complete };
  struct Context {
    ForzaMessage request;
    std::array<uint64_t, FORZA_VM_REGISTERS> regs{};
    std::array<uint64_t, 3> result{};
    Stage stage = Stage::Empty;
    uint64_t instruction = 0;
    uint64_t memory_uuid = 0;
    uint64_t memory_address = 0;
    uint32_t pc = 0;
    uint32_t steps = 0;
    uint32_t load_register = 0;
    bool memory_load = false;
    bool reply_error = false;
  };

  uint32_t hart(uint32_t context) const;
  void execute(uint32_t index);
  uint32_t core_id_;
  uint32_t execute_rr_ = 0;
  uint32_t fetch_rr_ = 0;
  std::deque<ForzaMessage> requests_;
  std::array<Context, FORZA_VM_CONTEXTS> contexts_{};
  std::array<std::array<uint64_t, FORZA_VM_PROGRAM_WORDS>, FORZA_VM_PROGRAMS> programs_{};
  std::array<uint32_t, FORZA_VM_PROGRAMS> valid_{};
};

}
