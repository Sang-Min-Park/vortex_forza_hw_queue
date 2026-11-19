#pragma once

#include <deque>
#include <unordered_map>
#include "forza_types.h"
#ifdef VX_CFG_FORZA_VM_ENABLE
#include "forza_vm.h"
#endif

namespace vortex {

// One zone owns its ZEN assembly, ZQM state and directly loadable mailbox SRAM.
class ForzaZone : public SimObject<ForzaZone> {
public:
  ForzaZone(const SimContext& ctx, const char* name, uint32_t core_id);

  SimChannel<ForzaCsrRequest> csr_req_in;
  SimChannel<ForzaCsrRequest> csr_rsp_out;
  std::vector<SimChannel<LsuReq>> mem_req_in;
  std::vector<SimChannel<LsuRsp>> mem_rsp_out;
  SimChannel<ForzaMessage> net_tx;
  SimChannel<ForzaMessage> net_rx;
  SimChannel<ForzaAck> ack_tx;
  SimChannel<ForzaAck> ack_rx;
#if defined(VX_CFG_FORZA_RECORD_ENABLE) || defined(VX_CFG_FORZA_VM_ENABLE)
  SimChannel<ForzaMessage> reply_net_tx;
  SimChannel<ForzaMessage> reply_net_rx;
#endif
#ifdef VX_CFG_FORZA_UPDATE_ENABLE
  SimChannel<MemReq> update_req_out;
  SimChannel<MemRsp> update_rsp_in;
#endif

#ifdef VX_CFG_FORZA_VM_ENABLE
  SimChannel<ForzaMessage> vm_request_out;
  SimChannel<ForzaVmResult> vm_result_in;
  SimChannel<ForzaAck> vm_result_ack_out;
  SimChannel<ForzaAck> vm_completion_in;
  SimChannel<ForzaVmConfig> vm_config_out;
  SimChannel<uint64_t> vm_config_ack_in;
#endif

protected:
  void on_reset();
  void on_tick();

private:
  friend class SimObject<ForzaZone>;
  static constexpr uint32_t ACTORS = VX_CFG_NUM_WARPS * VX_CFG_NUM_THREADS;
  static constexpr uint32_t TX_DEPTH = 64;
  static constexpr uint32_t RX_DEPTH = 64;
  enum class SlotState { Empty, Filling, Ready };
  struct Mailbox {
    std::array<SlotState, 2> slots{};
    uint32_t read_slot = 0;
    uint32_t write_slot = 0;
  };
  struct Actor {
    std::array<uint64_t, 7> payload{};
    uint32_t payload_size = 0;
    uint32_t registration = 0;
    uint8_t send_errors = 0;
    bool write_error = false;
#if defined(VX_CFG_FORZA_RECORD_ENABLE) || defined(VX_CFG_FORZA_VM_ENABLE)
    bool record_direct = false;
#endif
    std::array<uint8_t, 8> outstanding{};
    std::array<Mailbox, 8> mailboxes{};
  };
  struct Filling {
    ForzaMessage message;
    uint32_t actor;
    uint32_t slot;
  };
  struct CsrWork {
    ForzaCsrRequest request;
    uint32_t lane = 0;
#ifdef VX_CFG_FORZA_ISA_ENABLE
    ThreadMask completed{VX_CFG_NUM_THREADS};
    bool wait_any_ready = false;
#endif
  };

  uint64_t read_csr(uint32_t actor, uint32_t addr) const;
  bool write_csr(uint32_t actor, uint32_t addr, uint64_t value);
  uint64_t enqueue_token(uint32_t actor, uint32_t zone, uint32_t pe,
                         uint32_t mailbox, uint64_t token);
  uint64_t pop_token(uint32_t actor, uint32_t mailbox);
  void tick_csr();
#ifdef VX_CFG_FORZA_ISA_ENABLE
  void tick_isa(CsrWork work);
  uint64_t isa_command(uint32_t actor, ForzaType op, uint64_t first, uint64_t second);
  uint32_t command_budget_ = 0;
  bool isa_turn_ = false;
#endif
  void tick_memory();
#ifdef VX_CFG_FORZA_MMIO_ENABLE
  bool tick_mmio();
  uint64_t mmio_command(const LsuReq& request, uint32_t lane);
  std::array<uint32_t, VX_CFG_NUM_LSU_BLOCKS> mmio_lane_{};
  uint32_t mmio_port_rr_ = 0;
#endif
#ifdef VX_CFG_FORZA_VM_ENABLE
  void tick_vm();
  std::unordered_map<uint64_t, ForzaMessage> vm_replies_;
  std::deque<uint64_t> vm_configs_;
  uint64_t vm_config_sequence_ = 0;
#endif
  void tick_receive();
#ifdef VX_CFG_FORZA_UPDATE_ENABLE
  bool tick_update(bool allow_issue = true);
#ifdef VX_CFG_FORZA_RECORD_ENABLE
  bool tick_record(bool allow_issue);
#endif
#endif
  uint64_t slot_address(uint32_t actor, uint32_t mailbox, uint32_t slot) const;

  uint32_t core_id_;
  std::vector<Actor> actors_;
  std::unordered_map<uint32_t, uint32_t> pe_to_actor_;
  std::vector<uint8_t> sram_;
  std::deque<ForzaMessage> local_tx_;
  std::deque<ForzaMessage> external_tx_;
  std::array<std::deque<ForzaMessage>, 8> incoming_;
  std::deque<Filling> filling_;
  // Sequence ownership also makes duplicate/invalid ACKs unable to free credit.
  std::unordered_map<uint64_t, std::pair<uint32_t, uint32_t>> pending_;
  uint64_t sequence_ = 0;
  uint32_t receive_rr_ = 0;
  uint32_t transmit_rr_ = 0;
  std::deque<CsrWork> csr_work_;
#ifdef VX_CFG_FORZA_UPDATE_ENABLE
  std::deque<ForzaMessage> update_queue_;
  HashTable<ForzaMessage> update_pending_{FORZA_UPDATE_INFLIGHT};
#ifdef VX_CFG_FORZA_RECORD_ENABLE
  enum class RecordStage { Empty, Reading, Compute, Writing, ReplyReady, ReplyFlight, AckReady };
  struct RecordContext {
    ForzaMessage request;
    RecordStage stage = RecordStage::Empty;
    std::array<uint64_t, 3> old_values{};
    std::array<uint64_t, 3> new_values{};
    uint8_t issued = 0;
    uint8_t completed = 0;
    bool reply_error = false;
  };
  std::deque<ForzaMessage> record_queue_;
  std::array<RecordContext, FORZA_RECORD_CONTEXTS> record_contexts_{};
  std::unordered_map<uint64_t, uint32_t> record_replies_;
  uint32_t record_issue_rr_ = 0;
  bool record_memory_turn_ = false;
#endif
#endif
};

}
