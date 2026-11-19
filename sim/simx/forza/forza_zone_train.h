// CSR 학습용 복사본. ISA/MMIO/UPDATE/RECORD/VM 확장을 제외한 기본 경로.
// 원본을 대체하거나 빌드에 함께 추가하지 않는다. 클래스 이름은 원본과 같다.
#pragma once

#include <deque>
#include <unordered_map>
#include "forza_types.h"

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
  };

  uint64_t read_csr(uint32_t actor, uint32_t addr) const;
  bool write_csr(uint32_t actor, uint32_t addr, uint64_t value);
  void tick_csr();
  void tick_memory();
  void tick_receive();
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
};

}
