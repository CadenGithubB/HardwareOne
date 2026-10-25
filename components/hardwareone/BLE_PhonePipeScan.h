#pragma once
// Phone-pipe reconnect scan: its runtime profile and per-scan diagnostics.
// No Arduino, ESP-IDF or project dependencies, so host tests include it as is.
// Nothing here holds an address.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace hw1scan {

// Scan interval and window in BLE units of 0.625 ms; the longest scan in
// seconds (it stops at the first pipe advertisement).
//
// Why the default (firmware-007): with Wi-Fi associated, the C6's
// coexistence scheme gives a running BLE scan about 40% of each ~102 ms
// beacon period. The old 30 ms window every 100 ms, for 2 s, then hears a
// 100 ms advertiser only about half the time. Listening continuously removes
// the phase question, and 4 s lets the phone's advertising drift through
// Wi-Fi's slice; with the early stop it costs time only while the phone is
// absent. 60 ms (not 100) keeps channel switches out of step with the beacon.
struct Profile {
  uint16_t interval=96, window=96;  // 60 ms, continuous
  bool duplicates=false;            // every report; the callback coalesces
  uint8_t seconds=4;
};
constexpr Profile kDefault{};
// Before firmware-007: 30 ms of every 100 ms, the duplicate filter, 2 s.
constexpr Profile kLegacy{160,48,true,2};
constexpr unsigned kMinMs=3,kMaxIntervalMs=1000,kMaxSeconds=8;

inline bool same(const Profile& a,const Profile& b) {
  return a.interval==b.interval && a.window==b.window && a.duplicates==b.duplicates && a.seconds==b.seconds;
}
inline uint16_t unitsFromMs(unsigned ms) { return uint16_t((ms*8u+2u)/5u); }

// "default", "legacy", or "<interval_ms> <window_ms> dup|nodup [<seconds>]"
// (seconds defaults to the default profile's). Returns nullptr on success,
// otherwise the reason; `out` changes only on success.
inline const char* parse(const char* text,Profile& out) {
  while(*text==' ')++text;
  if(!std::strcmp(text,"default")){out=kDefault;return nullptr;}
  if(!std::strcmp(text,"legacy")){out=kLegacy;return nullptr;}
  char* end=nullptr;
  const unsigned long interval=std::strtoul(text,&end,10);
  if(end==text || *end!=' ')return "expected <interval_ms> <window_ms> dup|nodup, default or legacy";
  text=end;
  const unsigned long window=std::strtoul(text,&end,10);
  if(end==text || *end!=' ')return "expected <interval_ms> <window_ms> dup|nodup, default or legacy";
  while(*end==' ')++end;
  bool duplicates;
  if(!std::strncmp(end,"dup",3) && (!end[3] || end[3]==' ')){duplicates=true;end+=3;}
  else if(!std::strncmp(end,"nodup",5) && (!end[5] || end[5]==' ')){duplicates=false;end+=5;}
  else return "the third word must be dup or nodup";
  while(*end==' ')++end;
  unsigned long seconds=kDefault.seconds;
  if(*end) {
    text=end;seconds=std::strtoul(text,&end,10);
    if(end==text || *end)return "the optional last word is the scan length in seconds";
  }
  if(interval<kMinMs || interval>kMaxIntervalMs)return "interval must be 3-1000 ms";
  if(window<kMinMs || window>interval)return "window must be 3 ms up to the interval";
  if(seconds<1 || seconds>kMaxSeconds)return "scan length must be 1-8 seconds";
  out=Profile{unitsFromMs(unsigned(interval)),unitsFromMs(unsigned(window)),duplicates,uint8_t(seconds)};
  return nullptr;
}

// "interval_ms=60.00 window_ms=60.00 duplicates=off up_to_s=4"
inline int format(char* out,size_t cap,const Profile& p) {
  const unsigned long i=p.interval*625ul,w=p.window*625ul;  // microseconds
  return std::snprintf(out,cap,"interval_ms=%lu.%02lu window_ms=%lu.%02lu duplicates=%s up_to_s=%u",
      i/1000,i%1000/10,w/1000,w%1000/10,p.duplicates?"on":"off",unsigned(p.seconds));
}

// The scan's tally, records and log are shared with the other BLE peers:
// hw1coex in BLE_CoexRecords.h.

}  // namespace hw1scan
