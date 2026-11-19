#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <vector>

#include "forza/forza_fabric.h"

using namespace vortex;

namespace {

constexpr uint32_t ZONES = 4;

[[noreturn]] void fail(const char* message) {
  std::cerr << "FAILED: " << message << std::endl;
  std::exit(1);
}

void check(bool condition, const char* message) {
  if (!condition) {
    fail(message);
  }
}

struct MessageRecord {
  ForzaMessage packet;
  uint64_t cycle;
};

struct AckRecord {
  ForzaAck packet;
  uint64_t cycle;
};

struct ProbeControl {
  bool drain_messages;
  bool drain_acks;
};

class ChannelProbe : public SimObject<ChannelProbe> {
public:
  ChannelProbe(const SimContext& ctx, const char* name)
    : SimObject<ChannelProbe>(ctx, name)
    , msg_tx(this)
    , msg_rx(this, 1)
    , ack_tx(this)
    , ack_rx(this, 1)
    , submit_messages(this, 32)
    , submit_acks(this, 32)
    , observed_messages(this, 64)
    , observed_acks(this, 64)
    , control_in(this) {
  }

  SimChannel<ForzaMessage> msg_tx;
  SimChannel<ForzaMessage> msg_rx;
  SimChannel<ForzaAck> ack_tx;
  SimChannel<ForzaAck> ack_rx;
  SimChannel<ForzaMessage> submit_messages;
  SimChannel<ForzaAck> submit_acks;
  SimChannel<MessageRecord> observed_messages;
  SimChannel<AckRecord> observed_acks;
  SimChannel<ProbeControl> control_in;

protected:
  void on_reset() {
    drain_messages_ = true;
    drain_acks_ = true;
  }

  void on_tick() {
    if (!control_in.empty()) {
      drain_messages_ = control_in.peek().drain_messages;
      drain_acks_ = control_in.peek().drain_acks;
      control_in.pop();
    }
    if (!submit_messages.empty() && msg_tx.try_send(submit_messages.peek(), 1)) {
      submit_messages.pop();
    }
    if (!submit_acks.empty() && ack_tx.try_send(submit_acks.peek(), 1)) {
      submit_acks.pop();
    }
    auto cycle = SimPlatform::instance().cycles();
    if (drain_messages_ && !msg_rx.empty()
        && observed_messages.try_send(MessageRecord{msg_rx.peek(), cycle}, 1)) {
      msg_rx.pop();
    }
    if (drain_acks_ && !ack_rx.empty()
        && observed_acks.try_send(AckRecord{ack_rx.peek(), cycle}, 1)) {
      ack_rx.pop();
    }
  }

private:
  bool drain_messages_ = true;
  bool drain_acks_ = true;
  friend class SimObject<ChannelProbe>;
};

using Probes = std::array<ChannelProbe::Ptr, ZONES>;

struct Observations {
  std::array<std::vector<MessageRecord>, ZONES> messages;
  std::array<std::vector<AckRecord>, ZONES> acks;

  uint32_t message_count() const {
    uint32_t count = 0;
    for (const auto& output : messages) {
      count += output.size();
    }
    return count;
  }

  uint32_t ack_count() const {
    uint32_t count = 0;
    for (const auto& output : acks) {
      count += output.size();
    }
    return count;
  }
};

ForzaMessage message(uint32_t source, uint32_t destination, uint64_t sequence) {
  ForzaMessage packet;
  packet.source_zone = source;
  packet.source_actor = 11 + source;
  packet.dest_zone = destination;
  packet.dest_pe = 31 + source;
  packet.mailbox = sequence % 8;
  packet.sequence = sequence;
  for (uint32_t word = 0; word < packet.words.size(); ++word) {
    packet.words[word] = (uint64_t(source) << 56) | (sequence << 8) | word;
  }
  return packet;
}

void check_message(const ForzaMessage& packet, uint32_t source,
                   uint32_t destination, uint64_t sequence) {
  auto expected = message(source, destination, sequence);
  if (packet.source_zone != expected.source_zone
      || packet.source_actor != expected.source_actor
      || packet.dest_zone != expected.dest_zone || packet.dest_pe != expected.dest_pe
      || packet.mailbox != expected.mailbox || packet.sequence != expected.sequence
      || packet.words != expected.words) {
    std::cerr << "expected source=" << source << ", destination=" << destination
              << ", sequence=" << sequence << "; received source=" << packet.source_zone
              << ", destination=" << packet.dest_zone << ", sequence=" << packet.sequence
              << std::endl;
    fail("message identity or payload changed in transit");
  }
}

void tick(const Probes& probes, Observations& observed, uint32_t cycles = 1) {
  for (uint32_t cycle = 0; cycle < cycles; ++cycle) {
    SimPlatform::instance().tick();
    for (uint32_t output = 0; output < ZONES; ++output) {
      auto& probe = *probes[output];
      while (!probe.observed_messages.empty()) {
        observed.messages[output].push_back(probe.observed_messages.peek());
        probe.observed_messages.pop();
      }
      while (!probe.observed_acks.empty()) {
        observed.acks[output].push_back(probe.observed_acks.peek());
        probe.observed_acks.pop();
      }
    }
  }
}

void wait_for(const Probes& probes, Observations& observed,
              uint32_t messages, uint32_t acks) {
  for (uint32_t cycle = 0; cycle < 256; ++cycle) {
    tick(probes, observed);
    if (observed.message_count() >= messages && observed.ack_count() >= acks) {
      return;
    }
  }
  fail("fabric traffic did not complete within the cycle bound");
}

void check_drained(const Probes& probes, Observations& observed,
                   uint32_t messages, uint32_t acks) {
  tick(probes, observed, 8);
  check(observed.message_count() == messages && observed.ack_count() == acks,
        "fabric duplicated or misrouted traffic");
  check(SimPlatform::instance().idle(), "fabric or probe retained undelivered traffic");
}

void test_distinct_outputs(const Probes& probes) {
  SimPlatform::instance().reset();
  Observations observed;
  for (uint32_t source = 0; source < ZONES; ++source) {
    probes[source]->submit_messages.send(message(source, (source + 1) % ZONES, 1), 1);
  }
  wait_for(probes, observed, ZONES, 0);
  for (uint32_t output = 0; output < ZONES; ++output) {
    check(observed.messages[output].size() == 1, "distinct destination missed its message");
  }
  uint64_t first_cycle = observed.messages[0].front().cycle;
  for (uint32_t output = 0; output < ZONES; ++output) {
    const auto& record = observed.messages[output].front();
    check_message(record.packet, (output + ZONES - 1) % ZONES, output, 1);
    check(record.cycle == first_cycle, "independent destinations did not transfer concurrently");
  }
  check_drained(probes, observed, ZONES, 0);
}

void test_contention_fairness(const Probes& probes) {
  SimPlatform::instance().reset();
  Observations observed;
  constexpr uint32_t PER_SOURCE = 5;
  for (uint32_t source = 0; source < ZONES; ++source) {
    for (uint32_t sequence = 0; sequence < PER_SOURCE; ++sequence) {
      probes[source]->submit_messages.send(message(source, 0, sequence), sequence + 1);
      probes[source]->submit_acks.send(
          ForzaAck{0, source, sequence % 8, sequence, (sequence & 1) != 0}, sequence + 1);
    }
  }
  wait_for(probes, observed, ZONES * PER_SOURCE, ZONES * PER_SOURCE);
  check(observed.messages[0].size() == ZONES * PER_SOURCE
        && observed.acks[0].size() == ZONES * PER_SOURCE,
        "contended traffic did not reach its destination");
  std::array<uint32_t, ZONES> next_sequence{};
  std::array<uint32_t, ZONES> next_ack_sequence{};
  for (uint32_t group = 0; group < PER_SOURCE; ++group) {
    std::array<bool, ZONES> seen{};
    std::array<bool, ZONES> ack_seen{};
    for (uint32_t grant = 0; grant < ZONES; ++grant) {
      const auto& record = observed.messages[0][group * ZONES + grant];
      uint32_t source = record.packet.source_zone;
      check(source < ZONES && !seen[source], "a contending source missed its bounded fair grant");
      seen[source] = true;
      check_message(record.packet, source, 0, next_sequence[source]++);
      if (grant != 0) {
        check(record.cycle > observed.messages[0][group * ZONES + grant - 1].cycle,
              "one destination accepted multiple messages in one cycle");
      }
      const auto& ack_record = observed.acks[0][group * ZONES + grant];
      const auto& ack = ack_record.packet;
      uint32_t ack_source = ack.actor;
      check(ack_source < ZONES && !ack_seen[ack_source],
            "a contending acknowledgement source missed its bounded fair grant");
      ack_seen[ack_source] = true;
      uint32_t sequence = next_ack_sequence[ack_source]++;
      check(ack.dest_zone == 0 && ack.mailbox == sequence % 8
            && ack.sequence == sequence && ack.error == ((sequence & 1) != 0),
            "contended acknowledgement identity or order changed in transit");
      check(ack_record.cycle == record.cycle,
            "independent message and acknowledgement planes did not transfer concurrently");
    }
  }
  check_drained(probes, observed, ZONES * PER_SOURCE, ZONES * PER_SOURCE);
}

void test_backpressure_and_ack_progress(const Probes& probes) {
  SimPlatform::instance().reset();
  Observations observed;
  constexpr uint32_t PACKETS = 8;
  probes[0]->control_in.send(ProbeControl{false, true}, 1);
  tick(probes, observed, 2);
  for (uint32_t sequence = 0; sequence < PACKETS; ++sequence) {
    probes[0]->submit_messages.send(message(0, 0, sequence), sequence + 1);
    probes[1]->submit_messages.send(message(1, 1, sequence), sequence + 1);
    probes[0]->submit_acks.send(ForzaAck{0, 17, sequence % 8, sequence, (sequence & 1) != 0}, sequence + 1);
  }
  wait_for(probes, observed, PACKETS, PACKETS);
  check(observed.messages[0].empty(), "blocked destination consumed a message");
  check(probes[0]->submit_messages.size() != 0,
        "downstream blockage failed to backpressure the source");
  check(observed.messages[1].size() == PACKETS,
        "blocked destination stopped an unrelated destination");
  check(observed.acks[0].size() == PACKETS,
        "blocked message destination stopped independent acknowledgements");
  for (uint32_t sequence = 0; sequence < PACKETS; ++sequence) {
    check_message(observed.messages[1][sequence].packet, 1, 1, sequence);
    const auto& ack = observed.acks[0][sequence].packet;
    check(ack.dest_zone == 0 && ack.actor == 17 && ack.mailbox == sequence % 8
          && ack.sequence == sequence && ack.error == ((sequence & 1) != 0),
          "acknowledgement identity or order changed in transit");
  }
  probes[0]->control_in.send(ProbeControl{true, true}, 1);
  wait_for(probes, observed, 2 * PACKETS, PACKETS);
  check(observed.messages[0].size() == PACKETS, "blocked destination lost messages on resume");
  for (uint32_t sequence = 0; sequence < PACKETS; ++sequence) {
    check_message(observed.messages[0][sequence].packet, 0, 0, sequence);
  }
  check_drained(probes, observed, 2 * PACKETS, PACKETS);
}

}

int main() {
  auto fabric = ForzaFabric::Create("fabric", ZONES);
  Probes probes;
  for (uint32_t zone = 0; zone < ZONES; ++zone) {
    char name[32];
    std::snprintf(name, sizeof(name), "probe%u", zone);
    probes[zone] = ChannelProbe::Create(name);
    probes[zone]->msg_tx.bind(&fabric->msg_in[zone]);
    fabric->msg_out[zone].bind(&probes[zone]->msg_rx);
    probes[zone]->ack_tx.bind(&fabric->ack_in[zone]);
    fabric->ack_out[zone].bind(&probes[zone]->ack_rx);
  }
  test_distinct_outputs(probes);
  test_contention_fairness(probes);
  test_backpressure_and_ack_progress(probes);
  std::cout << "PASSED: FORZA fabric concurrency, fairness, backpressure and ACK progress" << std::endl;
  return 0;
}
