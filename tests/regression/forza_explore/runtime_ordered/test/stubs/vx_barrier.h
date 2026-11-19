#pragma once
namespace vortex {
struct gbarrier {
  explicit gbarrier(int) {}
  void arrive_and_wait() {}
};
}
