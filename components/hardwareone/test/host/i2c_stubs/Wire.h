#pragma once

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

// Address-only probe boundary. A target can ACK its address while refusing
// reads until its driver selects a register (or refusing reads altogether).
class TwoWire {
public:
  bool* lockHeld = nullptr;
  std::array<uint8_t, 128> statuses{};
  std::vector<uint8_t> responses;
  std::vector<uint8_t> addresses;
  std::vector<uint32_t> clocks;
  uint32_t clock = 0;
  unsigned endings = 0;
  unsigned reads = 0;
  unsigned writes = 0;

  void setClock(uint32_t hz) {
    assert(lockHeld && *lockHeld);
    clock = hz;
    clocks.push_back(hz);
  }
  void beginTransmission(uint8_t address) {
    assert(lockHeld && *lockHeld);
    assert(address < statuses.size());
    addresses.push_back(address);
  }
  uint8_t endTransmission(bool stop = true) {
    assert(lockHeld && *lockHeld && stop && !addresses.empty());
    ++endings;
    if (endings <= responses.size()) return responses[endings - 1];
    return statuses[addresses.back()];
  }
  size_t requestFrom(uint16_t, size_t) {
    ++reads;
    return 0;
  }
  size_t write(uint8_t) {
    ++writes;
    return 1;
  }
};

extern TwoWire Wire;
extern TwoWire Wire1;
