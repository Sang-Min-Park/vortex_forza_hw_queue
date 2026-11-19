#pragma once

#include "forza_types.h"

namespace vortex {

// Lossless crossbar: one granted input per output per cycle, separate ACK plane.
class ForzaFabric : public SimObject<ForzaFabric> {
public:
  ForzaFabric(const SimContext& ctx, const char* name, uint32_t zones);
  std::vector<SimChannel<ForzaMessage>> msg_in;
  std::vector<SimChannel<ForzaMessage>> msg_out;
#if defined(VX_CFG_FORZA_RECORD_ENABLE) || defined(VX_CFG_FORZA_VM_ENABLE)
  std::vector<SimChannel<ForzaMessage>> record_reply_in;
  std::vector<SimChannel<ForzaMessage>> record_reply_out;
#endif
  std::vector<SimChannel<ForzaAck>> ack_in;
  std::vector<SimChannel<ForzaAck>> ack_out;

protected:
  void on_reset();
  void on_tick();

private:
  friend class SimObject<ForzaFabric>;
  uint32_t zones_;
  std::vector<uint32_t> msg_rr_;
  std::vector<uint32_t> ack_rr_;
};

}
