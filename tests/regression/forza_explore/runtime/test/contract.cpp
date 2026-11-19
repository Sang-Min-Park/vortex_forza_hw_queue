#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <deque>
#include <vector>
#include <stdint.h>
#include <forza_abi.h>

uint64_t test_commit(uint64_t control);
void test_read_fence();
#define FORZA_VORTEX_TEST_COMMIT test_commit
#define FORZA_VORTEX_TEST_READ_FENCE test_read_fence
#include "selector.vortex.h"

struct Frame { std::array<uint64_t, 8> words{}; };
struct Model {
  std::array<uint64_t, 7> assembly{};
  uint32_t cursor = 0;
  bool overflow = false, hint_busy = false;
  uint32_t commit_busy = 0;
  uint64_t data_writes = 0, commit_attempts = 0, polls = 0, releases = 0, fences = 0;
  std::deque<Frame> inbox[8];
  std::vector<Frame> transmitted;
};
static Model model;

uint64_t csr_read(uint32_t address) {
  if (address == FORZA_CSR_ZENSTAT) {
    return (UINT64_C(1) << 63) | (model.hint_busy ? 255 : 0)
         | (model.overflow ? (UINT64_C(1) << 8) : 0);
  }
  if (address == FORZA_CSR_ZENCOUNT) return 0;
  if (address == FORZA_CSR_ZQMSTAT) {
    ++model.polls;
    uint64_t valid = 0;
    for (uint32_t mailbox = 0; mailbox < 8; ++mailbox) {
      valid |= uint64_t(!model.inbox[mailbox].empty()) << mailbox;
    }
    return valid;
  }
  if (address >= FORZA_CSR_ZQMDQ0 && address < FORZA_CSR_ZQMDQ0 + 8) {
    auto& queue = model.inbox[address - FORZA_CSR_ZQMDQ0];
    return queue.empty() ? 0 : reinterpret_cast<uintptr_t>(queue.front().words.data());
  }
  return 0;
}

void csr_write(uint32_t address, uint64_t value) {
  if (address == FORZA_CSR_ZENEQD) {
    ++model.data_writes;
    if (model.cursor < 7) model.assembly[model.cursor++] = value;
    else model.overflow = true;
  } else if (address >= FORZA_CSR_ZQMDQ0 && address < FORZA_CSR_ZQMDQ0 + 8) {
    assert(value == 0 && model.fences > model.releases);
    auto& queue = model.inbox[address - FORZA_CSR_ZQMDQ0];
    assert(!queue.empty());
    queue.front().words.fill(UINT64_C(0xdeadbeefdeadbeef));
    queue.pop_front();
    ++model.releases;
  }
}

uint64_t test_commit(uint64_t control) {
  ++model.commit_attempts;
  if (model.overflow) {
    model.cursor = 0;
    model.assembly.fill(0);
    return FORZA_TRY_INVALID;
  }
  if (model.commit_busy != 0) {
    --model.commit_busy;
    model.cursor = 0;
    model.assembly.fill(0);
    return FORZA_TRY_BUSY;
  }
  Frame frame;
  frame.words[0] = control;
  for (uint32_t word = 0; word < 7; ++word) frame.words[word + 1] = model.assembly[word];
  model.transmitted.push_back(frame);
  uint32_t pe = control & 0x7ff;
  uint32_t zone = (control >> FORZA_DEST_ZONE_SHIFT) & 15;
  uint32_t mailbox = (control >> FORZA_CONTROL_MBOX_SHIFT) & 7;
  if (zone == 0 && pe == 0) model.inbox[mailbox].push_back(frame);
  model.cursor = 0;
  model.assembly.fill(0);
  return FORZA_TRY_ACCEPTED;
}

void test_read_fence() { ++model.fences; }
static void reset() { model = Model{}; }
static void inject_done(uint32_t mailbox, uint64_t sender, uint64_t epoch) {
  Frame frame;
  frame.words[0] = forza_message_control(0, 0, mailbox, DONE_OP);
  frame.words[1] = sender;
  frame.words[2] = epoch;
  model.inbox[mailbox].push_back(frame);
}

template <size_t N> struct Blob { unsigned char bytes[N]; };
template <size_t N> static Blob<N> blob(uint8_t base) {
  Blob<N> result{};
  for (size_t i = 0; i < N; ++i) result.bytes[i] = uint8_t(base + i);
  return result;
}

template <size_t N> static void backend_size() {
  reset();
  auto input = blob<N>(17);
  assert(forza_send_sized(0, &input, N, 0, 0, 0) == SUCCESS);
  assert(model.data_writes == (N + 7) / 8 && model.commit_attempts == 1);
  auto frame = model.transmitted.back();
  assert(std::memcmp(frame.words.data() + 1, &input, N) == 0);
  auto physical = reinterpret_cast<const unsigned char*>(frame.words.data() + 1);
  for (size_t byte = N; byte < 56; ++byte) assert(physical[byte] == 0);
  assert(forza_message_available(0) == SUCCESS);
  uint64_t packet[7];
  for (auto& word : packet) word = UINT64_C(0xfeedfacefeedface);
  assert(forza_message_receive_ready(0, packet, N) == SUCCESS);
  assert(model.polls == 1 && model.releases == 1);
  assert(std::memcmp(packet, &input, N) == 0);
  for (size_t word = (N + 7) / 8; word < 7; ++word) assert(packet[word] == UINT64_C(0xfeedfacefeedface));
  assert(model.inbox[0].empty());
}

template <size_t N> static void selector_size() {
  reset();
  hclib::Selector<1, Blob<N>> selector(19);
  auto input = blob<N>(31);
  uint32_t handled = 0;
  selector.mb[0].process = [&](Blob<N> packet, int sender) {
    assert(sender == -1 && model.releases == 1);
    assert(std::memcmp(&packet, &input, N) == 0);
    ++handled;
  };
  assert(selector.try_send(0, input, 0));
#ifdef FORZA_VORTEX_SHORT_MESSAGES
  assert(model.data_writes == (N + 7) / 8);
#else
  assert(model.data_writes == 7);
#endif
  selector.progress();
  assert(handled == 1 && selector.ok());
#ifdef FORZA_VORTEX_SHORT_MESSAGES
  assert(model.polls == 1);
#else
  assert(model.polls == 2);
#endif
  auto bytes = reinterpret_cast<const unsigned char*>(model.transmitted[0].words.data() + 1);
  for (size_t i = N; i < 56; ++i) assert(bytes[i] == 0);
}

static void retry_and_errors() {
  reset();
  auto large = blob<56>(1);
  model.commit_busy = 1;
  assert(forza_send_sized(0, &large, 56, 0, 0, 0) == ZEN_NOT_READY);
  assert(model.cursor == 0 && model.transmitted.empty());
  auto short_packet = blob<1>(77);
  assert(forza_send_sized(0, &short_packet, 1, 0, 0, 0) == SUCCESS);
  assert(model.data_writes == 8 && model.commit_attempts == 2);
  auto bytes = reinterpret_cast<const uint8_t*>(model.transmitted[0].words.data() + 1);
  assert(bytes[0] == 77);
  for (size_t i = 1; i < 56; ++i) assert(bytes[i] == 0);
  model.hint_busy = true;
  assert(forza_send_sized(0, &short_packet, 1, 0, 0, 0) == ZEN_NOT_READY);
  assert(model.data_writes == 8 && model.commit_attempts == 2);
  model.hint_busy = false;
  for (uint32_t i = 0; i < 8; ++i) csr_write(FORZA_CSR_ZENEQD, i);
  assert(model.overflow);
  assert(forza_send_sized(0, &short_packet, 1, 0, 0, 0) == TRANSPORT_ERROR);
  assert(forza_send_sized(0, &short_packet, 0, 0, 0, 0) == INVALID_PKT_SZ);
  assert(forza_send_sized(0, &short_packet, 57, 0, 0, 0) == INVALID_PKT_SZ);
  assert(forza_send_sized(0, nullptr, 1, 0, 0, 0) == INVALID_PTR);
  assert(forza_send_sized(8, &short_packet, 1, 0, 0, 0) == INVALID_MBOX);
  assert(forza_send_sized(0, &short_packet, 1, 1, 0, 0) == INVALID_PRECINCT);
  assert(forza_send_sized(0, &short_packet, 1, 0, 2, 0) == INVALID_ZONE);
  assert(forza_send_sized(0, &short_packet, 1, 0, 0, 8) == INVALID_LPE);
}

static void done_payload_and_checked_api() {
  reset();
  assert(forza_send_done_epoch(0, 0, 0, 0, 123) == SUCCESS);
#ifdef FORZA_VORTEX_SHORT_MESSAGES
  assert(model.data_writes == 2);
#else
  assert(model.data_writes == 7);
#endif
  uint64_t packet[7];
  for (auto& word : packet) word = UINT64_MAX;
  assert(forza_message_available(0) == SUCCESS);
  assert(forza_message_receive_ready(0, packet, 1) == DONE_MSG);
  assert(packet[0] == 0 && packet[1] == 123 && packet[2] == UINT64_MAX);
  reset();
  uint64_t source[7] = {1, 2, 3, 4, 5, 6, 7};
  assert(forza_send(0, source, 0, 0, 0) == SUCCESS);
  assert(forza_message_receive(0, packet, 8) == INVALID_PKT_SZ);
  assert(forza_message_receive(0, packet, 56) == SUCCESS);
  assert(std::memcmp(packet, source, sizeof(source)) == 0);
  assert(forza_message_receive(0, packet, 56) == NO_MSG);
  assert(forza_send_done_epoch(0, 0, 0, 0, 124) == SUCCESS);
  assert(forza_message_receive(0, packet, 56) == DONE_MSG);
  assert(packet[0] == 0 && packet[1] == 124 && packet[2] == 0);
  reset();
  Frame invalid;
  invalid.words[0] = forza_message_control(0, 0, 0, 255);
  model.inbox[0].push_back(invalid);
  assert(forza_message_receive_ready(0, packet, 8) == PROTOCOL_ERROR);
  assert(model.releases == 1 && model.polls == 0);
  assert(forza_message_receive_ready(8, packet, 8) == INVALID_MBOX);
  assert(forza_message_receive_ready(0, nullptr, 8) == INVALID_PTR);
  assert(forza_message_receive_ready(0, packet, 0) == INVALID_PKT_SZ);
  assert(forza_message_receive_ready(0, packet, 57) == INVALID_PKT_SZ);
}

static void selector_protocol() {
  reset();
  hclib::Selector<2, uint64_t> selector(7);
  uint32_t requests = 0;
  uint64_t reply = 0;
  selector.mb[0].process = [&](uint64_t value, int sender) {
    assert(sender == -1);
    ++requests;
    assert(selector.send(1, value + 1, 0));
  };
  selector.mb[1].process = [&](uint64_t value, int sender) { assert(sender == -1); reply = value; };
  assert(selector.send(0, 42, 0));
  for (uint32_t mailbox = 0; mailbox < 2; ++mailbox) {
    for (uint64_t sender = 1; sender < 16; ++sender) {
      inject_done(mailbox, sender, 7);
      inject_done(mailbox, sender, 7);
    }
  }
  selector.request_done();
  for (uint32_t tick = 0; tick < 1000 && !selector.is_done(); ++tick) selector.progress();
  assert(selector.ok() && selector.is_done() && requests == 1 && reply == 43);
  assert(selector.done_received(0) == 16 && selector.done_received(1) == 16);
  reset();
  hclib::Selector<1, Blob<40>> bounded(8);
  uint32_t handled = 0;
  bounded.mb[0].process = [&](Blob<40>, int) { ++handled; };
  model.hint_busy = true;
  assert(!bounded.try_send(0, blob<40>(0), 0));
  for (uint32_t i = 0; i < FORZA_VORTEX_OUTBOX_DEPTH; ++i) assert(bounded.send(0, blob<40>(i), 0));
  assert(!bounded.send(0, blob<40>(99), 0) && bounded.ok());
  bounded.progress();
  assert(model.data_writes == 0);
  model.hint_busy = false;
  for (uint32_t tick = 0; tick < 100; ++tick) bounded.progress();
  assert(bounded.ok() && handled == FORZA_VORTEX_OUTBOX_DEPTH);
  reset();
  hclib::Selector<1, uint64_t> wrong_epoch(9);
  inject_done(0, 1, 8);
  wrong_epoch.progress();
  assert(!wrong_epoch.ok() && wrong_epoch.error() == hclib::RUNTIME_PROTOCOL_ERROR);
}

int main() {
  backend_size<1>(); backend_size<8>(); backend_size<16>(); backend_size<40>(); backend_size<56>();
  selector_size<1>(); selector_size<8>(); selector_size<16>(); selector_size<40>(); selector_size<56>();
  retry_and_errors(); done_payload_and_checked_api(); selector_protocol();
#ifdef FORZA_VORTEX_SHORT_MESSAGES
  std::puts("PASSED: short runtime actual backend CSR counts/sizes, single poll, padding, BUSY reset/overflow, copied-before-release, DONE/epoch/dedup and staged replies");
#else
  std::puts("PASSED: full runtime legacy receive/send, padded typed messages, BUSY reset/overflow, copied-before-release, DONE/epoch/dedup and staged replies");
#endif
}
