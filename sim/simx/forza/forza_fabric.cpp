#include "forza_fabric.h"

using namespace vortex;

ForzaFabric::ForzaFabric(const SimContext& ctx, const char* name, uint32_t zones)
  : SimObject<ForzaFabric>(ctx, name)
  , msg_in(zones, SimChannel<ForzaMessage>(this, 2))
  , msg_out(zones, SimChannel<ForzaMessage>(this))
#if defined(VX_CFG_FORZA_RECORD_ENABLE) || defined(VX_CFG_FORZA_VM_ENABLE)
  , record_reply_in(zones, SimChannel<ForzaMessage>(this, 2))
  , record_reply_out(zones, SimChannel<ForzaMessage>(this))
#endif
  , ack_in(zones, SimChannel<ForzaAck>(this, 2))
  , ack_out(zones, SimChannel<ForzaAck>(this))
  , zones_(zones)
  , msg_rr_(zones)
  , ack_rr_(zones) {
}

void ForzaFabric::on_reset() {
  std::fill(msg_rr_.begin(), msg_rr_.end(), 0);
  std::fill(ack_rr_.begin(), ack_rr_.end(), 0);
}

void ForzaFabric::on_tick() {
  std::vector<bool> msg_used(zones_, false);
  std::vector<bool> ack_used(zones_, false);
  for (uint32_t output = 0; output < zones_; ++output) {
#if defined(VX_CFG_FORZA_RECORD_ENABLE) || defined(VX_CFG_FORZA_VM_ENABLE)
    // Separate credits let a reply pass a blocked request without adding link bandwidth.
    for (uint32_t offset = 0; offset < zones_ * 2; ++offset) {
      uint32_t candidate = (msg_rr_[output] + offset) % (zones_ * 2);
      uint32_t input = candidate / 2;
      bool reply = candidate & 1;
      auto& incoming = reply ? record_reply_in[input] : msg_in[input];
      auto& outgoing = reply ? record_reply_out[output] : msg_out[output];
      if (!msg_used[input] && !incoming.empty() && !outgoing.full()
          && incoming.peek().dest_zone == output) {
        outgoing.send(incoming.peek(), 1);
        incoming.pop();
        msg_rr_[output] = (candidate + 1) % (zones_ * 2);
        msg_used[input] = true;
        break;
      }
    }
#else
    if (!msg_out[output].full()) {
      for (uint32_t offset = 0; offset < zones_; ++offset) {
        uint32_t input = (msg_rr_[output] + offset) % zones_;
        if (!msg_used[input] && !msg_in[input].empty()
            && msg_in[input].peek().dest_zone == output) {
          msg_out[output].send(msg_in[input].peek(), 1);
          msg_in[input].pop();
          msg_rr_[output] = (input + 1) % zones_;
          msg_used[input] = true;
          break;
        }
      }
    }
#endif
    if (!ack_out[output].full()) {
      for (uint32_t offset = 0; offset < zones_; ++offset) {
        uint32_t input = (ack_rr_[output] + offset) % zones_;
        if (!ack_used[input] && !ack_in[input].empty()
            && ack_in[input].peek().dest_zone == output) {
          ack_out[output].send(ack_in[input].peek(), 1);
          ack_in[input].pop();
          ack_rr_[output] = (input + 1) % zones_;
          ack_used[input] = true;
          break;
        }
      }
    }
  }
}
