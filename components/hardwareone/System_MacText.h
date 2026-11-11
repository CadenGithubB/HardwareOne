#pragma once
// MAC address text: the one place that knows how a 6-byte MAC is written,
// compared and parsed. No Arduino types and no project headers, so firmware
// modules and host tests include it directly.
//
// A MAC is written in exactly two forms, both UPPER-case. Pick the function
// that names where the text goes:
//
//   DISPLAY     "AA:BB:CC:DD:EE:FF"  colon-separated
//               -> logs, OLED/web UI, devices.json, settings, CLI output.
//   PATH TOKEN  "AABBCCDDEEFF"       no separator
//               -> filesystem paths (/system/espnow/peers/<TOKEN>/) and MQTT
//                  client id, topics and Home Assistant entity ids.
//
// Inside a longer format string use the two macros instead of spelling the
// six conversions out:
//
//   snprintf(cmd, sizeof(cmd), "espnowunpair " MAC_FMT, MAC_ARGS(peer.mac));
//   snprintf(path, sizeof(path), "/peers/" MAC_TOKEN_FMT ".json", MAC_ARGS(mac));
//
// String forms (macToDisplayStr, macToPathTokenStr) are in System_Utils.h.

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// The two forms as printf formats; both take the same six arguments.
#define MAC_FMT "%02X:%02X:%02X:%02X:%02X:%02X"
#define MAC_TOKEN_FMT "%02X%02X%02X%02X%02X%02X"
#define MAC_ARGS(mac) (mac)[0], (mac)[1], (mac)[2], (mac)[3], (mac)[4], (mac)[5]

// DISPLAY form into a caller buffer of at least 18 bytes. A smaller buffer
// gets the text cut short and terminated.
inline void macToDisplay(const uint8_t* mac, char* buf, size_t bufSize) {
  snprintf(buf, bufSize, MAC_FMT, MAC_ARGS(mac));
}

// PATH TOKEN form. out must hold at least 13 bytes.
inline void macToPathToken(const uint8_t* mac, char* out13) {
  snprintf(out13, 13, MAC_TOKEN_FMT, MAC_ARGS(mac));
}

// True when two 6-byte MAC addresses are equal.
inline bool macEquals(const uint8_t* a, const uint8_t* b) {
  return memcmp(a, b, 6) == 0;
}

// Lenient inbound parser. Accepts ':' '-' or ' ' separators or none; any
// case. Requires exactly 12 hex digits. Returns true and fills mac[6] on
// success, false on any malformed input.
inline bool macParse(const char* s, uint8_t mac[6]) {
  if (!s) return false;
  auto nyb = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  int byteIdx = 0;
  int hi = -1;
  for (const char* p = s; *p && byteIdx < 6; ++p) {
    char c = *p;
    if (c == ':' || c == '-' || c == ' ') continue;  // skip separators
    int v = nyb(c);
    if (v < 0) return false;
    if (hi < 0) { hi = v; }
    else { mac[byteIdx++] = (uint8_t)((hi << 4) | v); hi = -1; }
  }
  return (byteIdx == 6 && hi < 0);
}
