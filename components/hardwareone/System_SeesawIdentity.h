#pragma once

// Identity of the Adafruit seesaw boards this firmware drives. Several of them
// share addresses: 0x49 is the ANO rotary encoder's default, the Mini I2C
// Gamepad's alternate and the I2C Quad Rotary Encoder's default. An address
// alone does not say which board is there, so a driver reads the product id
// the board reports before it claims it (and before any reset), and a bus
// scan names an ambiguous address by that product id (owner decisions
// 2026-11-21).
//
// Header-only and free of Arduino types: the host tests use the same values.

#include <stddef.h>
#include <stdint.h>

// Product ids, as getVersion() >> 16 reports them.
constexpr uint16_t kSeesawProductRotaryEncoder = 4991;  // Rotary Encoder with NeoPixel
constexpr uint16_t kSeesawProductAnoEncoder = 5740;
constexpr uint16_t kSeesawProductGamepad = 5743;
constexpr uint16_t kSeesawProductQuadEncoder = 5752;    // I2C Quad Rotary Encoder

// The status VERSION register an identity read selects (seesaw module base,
// function), and the time the board needs before the reply can be read. The
// register select is the only write an identity read makes.
constexpr uint8_t kSeesawStatusBase = 0x00;
constexpr uint8_t kSeesawStatusVersion = 0x02;
constexpr uint32_t kSeesawReadSettleUs = 1000;

// The product id in a status VERSION value (four bytes, most significant first).
constexpr uint16_t seesawProductFromVersion(uint32_t version) {
  return (uint16_t)((version >> 16) & 0xFFFF);
}

constexpr uint32_t seesawVersionFromBytes(const uint8_t bytes[4]) {
  return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
         ((uint32_t)bytes[2] << 8) | (uint32_t)bytes[3];
}

// A driver claims a board only when it reports the driver's own product id.
constexpr bool seesawProductMatches(uint16_t expected, uint32_t version) {
  return seesawProductFromVersion(version) == expected;
}

// Whether a scan has to read the product id at an address: the products of
// the compiled table rows that can claim that address on that bus (0 for a
// row that is not a seesaw board) name at least two different devices, one of
// them a seesaw board. One claiming product, or only non-seesaw rows, keeps
// the address-only label and the bus sees no write.
inline bool seesawIdentityNeeded(const uint16_t* claimingProducts, size_t count) {
  if (!claimingProducts) return false;
  bool seesaw = false;
  bool differs = false;
  for (size_t i = 0; i < count; ++i) {
    if (claimingProducts[i] != 0) seesaw = true;
    if (claimingProducts[i] != claimingProducts[0]) differs = true;
  }
  return seesaw && differs;
}

// Which of `count` table rows names the board that reported `product`: the
// first row carrying that product id, or -1. A row without one (0) never
// matches, so an unknown or unread product is never named by its address.
inline int seesawRowForProduct(const uint16_t* rowProducts, size_t count, uint16_t product) {
  if (!rowProducts || product == 0) return -1;
  for (size_t i = 0; i < count; ++i) {
    if (rowProducts[i] == product) return (int)i;
  }
  return -1;
}
