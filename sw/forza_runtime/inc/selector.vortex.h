#pragma once

#if defined(__VORTEX__) && !defined(FORZA_VORTEX_DIVERGENCE_OK)
#error "FORZA Vortex device code must include vortex.mk to configure SIMT divergence handling"
#endif

// Vortex adaptation of selector.forza.h at upstream 138df12fbe56d2bc248c596841ac23703217e6d8.
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <new>
#include <type_traits>
#ifndef FORZA_VORTEX_TRANSPORT_HEADER
#define FORZA_VORTEX_TRANSPORT_HEADER "forza_message_lib.vortex.h"
#endif
#include FORZA_VORTEX_TRANSPORT_HEADER

#ifndef FORZA_VORTEX_MAX_PES
#define FORZA_VORTEX_MAX_PES 128
#endif
#ifndef FORZA_VORTEX_OUTBOX_DEPTH
#define FORZA_VORTEX_OUTBOX_DEPTH 8
#endif

namespace hclib {

enum VortexRuntimeError {
  RUNTIME_INVALID_ARGUMENT = 16,
  RUNTIME_MISSING_HANDLER,
  RUNTIME_PROTOCOL_ERROR
};

struct Packet56b {
  uint64_t v1, v2, v3, v4, v5, v6, v7;
};

inline uint64_t forza_n_pes() { return forza_vortex_n_pes(); }
inline uint64_t forza_my_pe() { return forza_vortex_my_pe(); }

inline void forza_get_physical_ids_by_global_pe(uint64_t pe, uint64_t* precinct,
                                               uint64_t* zone, uint64_t* local_pe) {
  *precinct = 0;
  *zone = pe / forza_vortex_pes_per_zone();
  *local_pe = pe % forza_vortex_pes_per_zone();
}

inline void forza_runtime_init() {
  if (forza_vortex_init() != SUCCESS) {
    __builtin_trap();
  }
}

template <typename F>
inline void launch(const char** deps, int ndeps, F&& body) {
  (void)deps;
  (void)ndeps;
  forza_runtime_init();
  body();
}

template <typename F>
inline void finish(F&& body) {
  // The upstream adapter's finish is a callback scope, without HClib futures.
  body();
}

template <typename T>
class InlineProcess {
public:
  InlineProcess() = default;
  InlineProcess(const InlineProcess&) = delete;
  InlineProcess& operator=(const InlineProcess&) = delete;

  template <typename F>
  InlineProcess& operator=(F body) {
    static_assert(sizeof(F) <= sizeof(storage_), "handler capture exceeds inline storage");
    static_assert(alignof(F) <= alignof(max_align_t), "handler capture alignment is unsupported");
    static_assert(std::is_trivially_copyable<F>::value,
                  "handler captures must be trivially copyable");
    static_assert(std::is_trivially_destructible<F>::value,
                  "handler captures must be trivially destructible");
    new (storage_) F(body);
    call_ = [](void* storage, T packet, int sender) {
      (*static_cast<F*>(storage))(packet, sender);
    };
    return *this;
  }

  explicit operator bool() const { return call_ != nullptr; }

  void operator()(T packet, int sender) {
    call_(storage_, packet, sender);
  }

private:
  alignas(max_align_t) unsigned char storage_[128];
  void (*call_)(void*, T, int) = nullptr;
};

template <int N, typename T> class Selector;

template <typename T>
class Mailbox {
public:
  uint64_t mb_id = 0;
  InlineProcess<T> process;

  bool send(T packet, int rank) {
    return send_(owner_, mb_id, packet, rank);
  }

  uint64_t done_received() const { return done_received_; }
  uint64_t outstanding() const { return outstanding_(owner_, mb_id); }
  bool isDone() const { return finished_; }

  uint32_t receive(bool return_on_done) {
    do {
      progress_(owner_);
      if (!return_on_done || vx_vote_all(finished_)) {
        break;
      }
    } while (!vx_vote_any(!ok_(owner_)));
    return finished_ ? uint32_t(DONE_MSG) : status_(owner_, mb_id);
  }

  void receiveUntil(uint64_t threshold) {
    while (!vx_vote_all(outstanding() <= threshold && !queued_(owner_, mb_id))) {
      progress_(owner_);
      if (vx_vote_any(!ok_(owner_))) {
        return;
      }
    }
  }

  void done() { done_(owner_); }

  void safeCopyPacket56btoT(const Packet56b& packet, T& target) {
    static_assert(sizeof(T) <= 56, "actor payload exceeds 56 bytes");
    static_assert(std::is_trivially_copyable<T>::value,
                  "actor payload must be trivially copyable");
    memcpy(&target, &packet, sizeof(T));
  }

private:
  template <int N, typename U> friend class Selector;
  void* owner_ = nullptr;
  bool (*send_)(void*, uint32_t, T, int) = nullptr;
  bool (*progress_)(void*) = nullptr;
  void (*done_)(void*) = nullptr;
  bool (*ok_)(const void*) = nullptr;
  uint64_t (*outstanding_)(const void*, uint32_t) = nullptr;
  bool (*queued_)(const void*, uint32_t) = nullptr;
  uint32_t (*status_)(const void*, uint32_t) = nullptr;
  uint64_t done_received_ = 0;
  bool closing_ = false;
  bool data_drained_ = false;
  bool finished_ = false;
  uint32_t done_destination_ = 0;
  uint64_t seen_[(FORZA_VORTEX_MAX_PES + 63) / 64]{};
};

template <int N, typename T = int64_t>
class Selector {
public:
  static_assert(N > 0 && N <= 8, "Selector requires one to eight mailboxes");
  static_assert(sizeof(T) <= 56, "actor payload exceeds 56 bytes");
  static_assert(std::is_trivially_copyable<T>::value,
                "actor payload must be trivially copyable");
  static_assert(FORZA_VORTEX_OUTBOX_DEPTH > 0, "outbox must have positive capacity");

  Mailbox<T> mb[N];

  explicit Selector(uint64_t epoch = 0)
    : epoch_(epoch), pes_(forza_n_pes()) {
    if (pes_ == 0 || pes_ > FORZA_VORTEX_MAX_PES) {
      error_ = RUNTIME_INVALID_ARGUMENT;
    }
    for (uint32_t m = 0; m < N; ++m) {
      auto& mailbox = mb[m];
      mailbox.mb_id = m;
      mailbox.owner_ = this;
      mailbox.send_ = [](void* owner, uint32_t id, T packet, int rank) {
        return static_cast<Selector*>(owner)->send(id, packet, rank);
      };
      mailbox.progress_ = [](void* owner) { return static_cast<Selector*>(owner)->progress(); };
      mailbox.done_ = [](void* owner) { static_cast<Selector*>(owner)->done(); };
      mailbox.ok_ = [](const void* owner) { return static_cast<const Selector*>(owner)->ok(); };
      mailbox.outstanding_ = [](const void* owner, uint32_t id) {
        return static_cast<const Selector*>(owner)->outstanding(id);
      };
      mailbox.queued_ = [](const void* owner, uint32_t id) {
        return static_cast<const Selector*>(owner)->has_queued(id);
      };
      mailbox.status_ = [](const void* owner, uint32_t id) {
        return static_cast<const Selector*>(owner)->last_receive_[id];
      };
      last_receive_[m] = NO_MSG;
    }
  }

  Selector(const Selector&) = delete;
  Selector& operator=(const Selector&) = delete;

  void start() {}
  bool ok() const { return error_ == SUCCESS; }
  uint32_t error() const { return error_; }
  uint64_t epoch() const { return epoch_; }

  uint64_t done_received(uint32_t mailbox) const {
    return mailbox < N ? mb[mailbox].done_received_ : 0;
  }

  uint64_t outstanding(uint32_t mailbox) const {
    uint64_t count = 0;
    uint32_t result = forza_get_outstanding_msg_count(mailbox, &count);
    if (result != SUCCESS) {
      error_ = result;
    }
    return count;
  }

  bool try_send(int mailbox, T packet, int rank) {
    if (!validate_send(mailbox, rank) || queued_count_ != 0) {
      return false;
    }
    Payload payload{};
    memcpy(payload.words, &packet, sizeof(T));
    return send_payload(mailbox, payload, rank);
  }

  bool send(int mailbox, T packet, int rank) {
    if (!validate_send(mailbox, rank) || queued_count_ == FORZA_VORTEX_OUTBOX_DEPTH) {
      return false;
    }
    uint32_t tail = (queued_head_ + queued_count_) % FORZA_VORTEX_OUTBOX_DEPTH;
    auto& pending = outbox_[tail];
    pending.mailbox = mailbox;
    pending.rank = rank;
    memset(pending.payload.words, 0, sizeof(pending.payload.words));
    memcpy(pending.payload.words, &packet, sizeof(T));
    ++queued_count_;
    return true;
  }

  bool progress() {
    if (!ok() || in_progress_) {
      return false;
    }
    in_progress_ = true;
    bool made_progress = false;
    // All mailboxes progress even while an earlier mailbox is closing. A
    // request handler can therefore enqueue replies on a later mailbox.
    for (uint32_t m = 0; m < N; ++m) {
      last_receive_[m] = NO_MSG;
      uint32_t available = forza_message_available(m);
      if (available == NO_MSG) {
        continue;
      }
      if (available != SUCCESS) {
        error_ = available;
        break;
      }
      Payload payload{};
      uint32_t received = forza_message_receive(m, payload.words, sizeof(payload.words));
      last_receive_[m] = received;
      if (received == SUCCESS) {
        if (mb[m].finished_) {
          error_ = RUNTIME_PROTOCOL_ERROR;
          break;
        }
        if (!mb[m].process) {
          error_ = RUNTIME_MISSING_HANDLER;
          break;
        }
        T packet;
        memcpy(&packet, payload.words, sizeof(T));
        mb[m].process(packet, -1);
        made_progress = true;
      } else if (received == DONE_MSG) {
        accept_done(m, payload);
        made_progress = true;
      } else if (received != NO_MSG) {
        error_ = received;
        break;
      }
      if (!ok()) {
        break;
      }
    }
    if (ok() && queued_count_ != 0) {
      auto& pending = outbox_[queued_head_];
      if (send_payload(pending.mailbox, pending.payload, pending.rank)) {
        queued_head_ = (queued_head_ + 1) % FORZA_VORTEX_OUTBOX_DEPTH;
        --queued_count_;
        made_progress = true;
      }
    }
    if (ok() && close_requested_ && closing_mailbox_ < N) {
      made_progress = progress_close() || made_progress;
    }
    in_progress_ = false;
    return made_progress;
  }

  void request_done() {
    if (!close_requested_) {
      close_requested_ = true;
      mb[0].closing_ = true;
    }
  }

  bool is_done() const {
    bool complete = close_requested_ && closing_mailbox_ == N && queued_count_ == 0;
    for (uint32_t m = 0; m < N; ++m) {
      complete = (outstanding(m) == 0) && complete;
    }
    return complete && ok();
  }

  void done(int mailbox = 0) {
    if (mailbox != 0) {
      error_ = RUNTIME_INVALID_ARGUMENT;
      return;
    }
    request_done();
    // A completed lane must keep serving peers until its whole warp finishes.
    while (!vx_vote_all(is_done())) {
      progress();
      if (vx_vote_any(!ok())) {
        return;
      }
    }
  }

private:
  struct Payload {
    uint64_t words[7];
  };
  struct Pending {
    uint32_t mailbox;
    uint32_t rank;
    Payload payload;
  };

  bool validate_send(int mailbox, int rank) {
    if (!ok()) {
      return false;
    }
    if (mailbox < 0 || mailbox >= N || rank < 0 || uint64_t(rank) >= pes_) {
      error_ = RUNTIME_INVALID_ARGUMENT;
      return false;
    }
    if (mb[mailbox].closing_) {
      error_ = RUNTIME_PROTOCOL_ERROR;
      return false;
    }
    return true;
  }

  bool has_queued(uint32_t mailbox) const {
    bool found = false;
    for (uint32_t i = 0; i < queued_count_; ++i) {
      found = found || outbox_[(queued_head_ + i) % FORZA_VORTEX_OUTBOX_DEPTH].mailbox == mailbox;
    }
    return found;
  }

  bool send_payload(uint32_t mailbox, const Payload& payload, uint32_t rank) {
    uint64_t precinct, zone, local_pe;
    forza_get_physical_ids_by_global_pe(rank, &precinct, &zone, &local_pe);
    uint32_t result = forza_send(mailbox, payload.words, precinct, zone, local_pe);
    if (result != SUCCESS && result != ZEN_NOT_READY) {
      error_ = result;
    }
    return result == SUCCESS;
  }

  void accept_done(uint32_t mailbox, const Payload& payload) {
    uint64_t sender = payload.words[0];
    if (sender >= pes_ || payload.words[1] != epoch_) {
      error_ = RUNTIME_PROTOCOL_ERROR;
      return;
    }
    uint64_t bit = 1ull << (sender % 64);
    auto& word = mb[mailbox].seen_[sender / 64];
    if (!(word & bit)) {
      word |= bit;
      ++mb[mailbox].done_received_;
    }
  }

  bool progress_close() {
    auto& mailbox = mb[closing_mailbox_];
    if (!mailbox.data_drained_ && !has_queued(closing_mailbox_)
        && outstanding(closing_mailbox_) == 0) {
      mailbox.data_drained_ = true;
    }
    bool made_progress = false;
    if (mailbox.data_drained_ && mailbox.done_destination_ < pes_) {
      uint64_t precinct, zone, local_pe;
      forza_get_physical_ids_by_global_pe(mailbox.done_destination_, &precinct, &zone, &local_pe);
      uint32_t result = forza_send_done_epoch(closing_mailbox_, precinct, zone, local_pe, epoch_);
      if (result == SUCCESS) {
        ++mailbox.done_destination_;
        made_progress = true;
      } else if (result != ZEN_NOT_READY) {
        error_ = result;
      }
    }
    if (mailbox.done_destination_ == pes_ && mailbox.done_received_ == pes_
        && outstanding(closing_mailbox_) == 0) {
      mailbox.finished_ = true;
      ++closing_mailbox_;
      if (closing_mailbox_ < N) {
        mb[closing_mailbox_].closing_ = true;
      }
      made_progress = true;
    }
    return made_progress;
  }

  uint64_t epoch_;
  uint64_t pes_;
  mutable uint32_t error_ = SUCCESS;
  Pending outbox_[FORZA_VORTEX_OUTBOX_DEPTH];
  uint32_t queued_head_ = 0;
  uint32_t queued_count_ = 0;
  uint32_t last_receive_[N];
  uint32_t closing_mailbox_ = 0;
  bool close_requested_ = false;
  bool in_progress_ = false;
};

} // namespace hclib
