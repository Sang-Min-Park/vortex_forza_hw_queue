// Copyright © 2019-2023
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "local_mem_switch.h"

using namespace vortex;

LocalMemSwitch::LocalMemSwitch(
  const SimContext& ctx,
  const char* name,
  uint32_t delay
) : SimObject<LocalMemSwitch>(ctx, name)
  // Single-entry ingress: the switch accepts a new request only as its
  // current one drains, so upstream issue feels downstream cadence
  // immediately instead of running ahead into queue slack.
  , ReqIn(this, 1)
  , RspOut(this)
  , ReqOutLmem(this)
  , RspInLmem(this)
  , ReqOutDC(this)
  , RspInDC(this)
#ifdef VX_CFG_EXT_FORZA_ENABLE
  , ReqOutForza(this)
  , RspInForza(this)
#endif
  , delay_(delay)
#ifdef VX_CFG_EXT_FORZA_ENABLE
  , rsp_cursor_(0)
#endif
{}

#ifdef VX_CFG_EXT_FORZA_ENABLE
void LocalMemSwitch::on_reset() {
  rsp_cursor_ = 0;
}
#endif

void LocalMemSwitch::on_tick() {
#ifdef VX_CFG_EXT_FORZA_ENABLE
  std::array<SimChannel<LsuRsp>*, 3> rsp_inputs = {&RspInLmem, &RspInDC, &RspInForza};
  for (uint32_t i = 0; i < rsp_inputs.size(); ++i) {
    uint32_t idx = (rsp_cursor_ + i) % rsp_inputs.size();
    auto& rsp_in = *rsp_inputs.at(idx);
    if (rsp_in.empty()) {
      continue;
    }
    auto& out_rsp = rsp_in.peek();
    if (RspOut.try_send(out_rsp, 1)) {
      DT(4, this->name() << " rsp" << idx << ": " << out_rsp);
      rsp_in.pop();
      rsp_cursor_ = (idx + 1) % rsp_inputs.size();
    }
    break;
  }
#else
  // process outgoing responses
  if (!RspInLmem.empty()) {
    auto& out_rsp = RspInLmem.peek();
    if (RspOut.try_send(out_rsp, 1)) {
      DT(4, this->name() << " lmem-rsp: " << out_rsp);
      RspInLmem.pop();
    }
  }
  if (!RspInDC.empty()) {
    auto& out_rsp = RspInDC.peek();
    if (RspOut.try_send(out_rsp, 1)) {
      DT(4, this->name() << " dc-rsp: " << out_rsp);
      RspInDC.pop();
    }
  }
#endif

  // process incoming requests
  if (!ReqIn.empty()) {
    auto& in_req = ReqIn.peek();

    LsuReq out_dc_req = in_req;
    out_dc_req.mask.reset();
    LsuReq out_lmem_req = in_req;
    out_lmem_req.mask.reset();
  #ifdef VX_CFG_EXT_FORZA_ENABLE
    LsuReq out_forza_req = in_req;
    out_forza_req.mask.reset();
  #endif

    for (uint32_t i = 0; i < in_req.mask.size(); ++i) {
      if (!in_req.mask.test(i)) {
        continue;
      }
      auto type = get_addr_type(in_req.addrs.at(i));
      auto* out_req = &out_dc_req;
      if (type == AddrType::Shared) {
        out_req = &out_lmem_req;
      }
    #ifdef VX_CFG_EXT_FORZA_ENABLE
      if (type == AddrType::Forza) {
        out_req = &out_forza_req;
      }
    #endif
      out_req->mask.set(i);
    }

    bool send_to_dc = !out_dc_req.mask.none();
    bool send_to_lmem = !out_lmem_req.mask.none();
  #ifdef VX_CFG_EXT_FORZA_ENABLE
    bool send_to_forza = !out_forza_req.mask.none();
  #endif

    // check DC backpressure
    if (send_to_dc && ReqOutDC.full()) {
      return;
    }

    // check LMem backpressure
    if (send_to_lmem && ReqOutLmem.full()) {
      return;
    }

  #ifdef VX_CFG_EXT_FORZA_ENABLE
    if (send_to_forza && ReqOutForza.full()) {
      return;
    }
  #endif

    if (send_to_dc) {
      ReqOutDC.send(out_dc_req, delay_);
      DT(4, this->name() << " dc-req: " << out_dc_req);
    }

    if (send_to_lmem) {
      ReqOutLmem.send(out_lmem_req, delay_);
      DT(4, this->name() << " lmem-req: " << out_lmem_req);
    }
  #ifdef VX_CFG_EXT_FORZA_ENABLE
    if (send_to_forza) {
      ReqOutForza.send(out_forza_req, delay_);
      DT(4, this->name() << " forza-req: " << out_forza_req);
    }
  #endif
    ReqIn.pop();
  }

  // Sleep only when no queued or in-flight inputs can need a retry.
  bool idle = (ReqIn.size() == 0 && RspInLmem.size() == 0 && RspInDC.size() == 0);
#ifdef VX_CFG_EXT_FORZA_ENABLE
  idle = idle && (RspInForza.size() == 0);
#endif
  if (idle) {
    this->tick_sleep();
  }
}
