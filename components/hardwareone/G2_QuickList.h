#ifndef G2_QUICKLIST_H
#define G2_QUICKLIST_H

// Five control rows in the G2 glasses' own QuickList (sid 0x0C, right temple,
// firmware 2.3.2.14 pair only): "Wear Detection: On/Off" (index 0), "Silent
// Mode: On/Off" (index 1), "Auto Brightness: On/Off" (index 2), "Head-Up
// Display: On/Off" (index 3) and "Brightness: Auto|<level>%" (index 4). With
// `g2 list feed on` (saved, off by default) HardwareOne reads all of them in
// one fresh BASIC read a few seconds after each connect and sends the rows with
// the current state (replacing the glasses' list, see below). When the wearer ticks a row and leaves the list, the
// glasses report the tick; the G2 control owner hands the write to cmd_exec
// (the only task that may wait for the settings replies g2_owner parses),
// which writes the opposite value (Brightness: turns auto brightness off and
// writes 25, or steps 25 -> 50 -> 75 -> 100 -> 25), reads it back, and the
// owner then shows the new state on that row (and on the other brightness row
// when it changed). A silent change made elsewhere (two-temple long-press,
// the Even app, the CLI) arrives as a settings push and changes the silent
// row too; no such push is known for the other rows, so they change only on a
// read or a tick.
// Every push is one cmd 2 FULL_UPDATE carrying all five rows at index 0..4
// with their current titles (G2_QuickListCore.h kPushAckWaitMs): it replaces
// the whole list on the glasses, so the rows keep their order 0..4 from the
// top and items added in the Even app are removed from the lens. One ACK
// covers all five rows.
// `g2 list push` does the same read and push once for the current connection.
// One job at a time for all rows, at most one tick-triggered job per 5 s. When
// a read goes unanswered and rows show "?", the owner reads again after 2 s,
// 5 s, 15 s, 30 s, then every 60 s (feed on or a manual push, exact pair, no
// voice/QuickNote session) until the rows have a state. Three failed sends or
// missing ACKs in a row use the same schedule: each answered re-read sends the
// list again until one is ACKed. Wire, whitelist and policy live in
// G2_QuickListCore.h.

#include "System_BuildConfig.h"
#include <stddef.h>
#include <stdint.h>
#include "G2_QuickListCore.h"   // WearResult / ReadResult in the boundary below

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES && ENABLE_G2_QUICKLIST

// G2 control owner only (g2_owner, after g2StockFeedTick in the owner lap):
// link state, the job result, a stashed tick, the settle timer and the push.
void g2QuickListTick();
// G2 control owner only: the longest the owner may sleep before the next lap
// (UINT32_MAX = no limit).
uint32_t g2QuickListOwnerWaitMs();
// G2 control owner only, once as it exits (closeg2).
void g2QuickListOwnerStopped();
// g2_owner only: sid 0x0C from the RIGHT temple at `rightGeneration`. Never
// blocks. Returns true when consumed (the default hex log then skips it).
bool g2QuickListOnRx(uint32_t rightGeneration, uint8_t flag, const uint8_t* pb, size_t len);
// g2_owner only: a silent-mode change push (sid 0x09 flag 0x01, cmd 3 f5{f2})
// from the RIGHT temple at `rightGeneration`. Never blocks; the owner applies
// the newest value once no job runs.
void g2QuickListOnSilentPush(uint32_t rightGeneration, bool on);

// Transport boundary, implemented in G2_Glasses.cpp (any task unless noted).
struct G2QuickListLinkState {
  bool bothReady, busy, firmwareKnown, firmwareExact;
  uint32_t leftGeneration, rightGeneration;
  char leftFw[16], rightFw[16];
};
// Both temples' readiness, generations and firmware strings, and whether an
// exclusive glasses session (Even AI, Conversate, native terminal/note,
// QuickNote) is running. Returns bothReady.
bool g2QuickListLink(G2QuickListLinkState* out);
// g2_owner: re-validates `pb` against the FULL_UPDATE whitelist
// (validateFullUpdatePb) and the exact 2.3.2.14 pair, then writes it to the
// right temple at exactly `rightGeneration` (sid 0x0C, flag 0x20, fragmented
// if ever needed; about 190 B is one fragment). False if refused or not sent.
bool g2QuickListWireSend(uint32_t rightGeneration, const uint8_t* pb, size_t len);
// Wakes the G2 control owner (any task).
void g2QuickListWake();
// cmd_exec ONLY (blocks about 3 s when unanswered, up to ~6.8 s with TX gate
// waits; refuse on g2_owner / BLE callback).
// One fresh correlated settings read of the right temple, every switch: the
// captured BASIC read, resent up to twice (new magic each) when the 1 s wait
// for its reply runs out; `diag` records attempts and reply time.
// A switch is known when the reply carries it, or when a full 2.3.2.14 reply
// (`fullProfile`) leaves it out (= off; for auto brightness f18 and head-up f7
// still to confirm on the device). The Brightness row is "Auto" with auto
// brightness on, else the level (f2). Returns true when the read was answered.
bool g2QuickListSettingsRead(uint32_t rightGeneration, hw1_g2_quicklist::ReadResult* out);
// cmd_exec ONLY: cmd_g2glasses' 2.3.2.14 wear write with a typed result
// (pre-read, write, ACK wait, then a read-back 500 ms later and, only when it
// does not show `on`, one more 1.5 s after that; every read as
// g2QuickListSettingsRead's, recorded in preRead/readBack/recheck; timing in
// G2_QuickListCore.h kReadbackDelayMs). True when the read-back confirms `on`.
bool g2QuickListWearWrite(uint32_t rightGeneration, bool on, hw1_g2_quicklist::WearResult* out);
// cmd_exec ONLY: the same for silent mode (write f6{f1}, readback BASIC f14).
bool g2QuickListSilentWrite(uint32_t rightGeneration, bool on, hw1_g2_quicklist::WearResult* out);
// cmd_exec ONLY: the same for auto brightness (write f1{f1}, the ACK echoes an
// explicit zero; readback BASIC f18; the result also carries the level, f2).
bool g2QuickListAutoBrightWrite(uint32_t rightGeneration, bool on, hw1_g2_quicklist::WearResult* out);
// cmd_exec ONLY: the same for the head-up display switch (write f4{f1}, the ACK
// echoes an explicit zero; readback BASIC f7, off = left out of a full reply).
bool g2QuickListHeadUpWrite(uint32_t rightGeneration, bool on, hw1_g2_quicklist::WearResult* out);
// cmd_exec ONLY: the Brightness row's step. Pre-read; auto brightness on ->
// write it off (f1{f1:0}), and only once that is ACKed write level 25;
// auto brightness off -> write the next of 25/50/75/100 (f1{f2}). Then the
// read-back as above, confirming f2 == the level (and f18 off when it was
// turned off). True when confirmed; out->target is the level chosen.
bool g2QuickListBrightnessWrite(uint32_t rightGeneration, hw1_g2_quicklist::WearResult* out);

class String;
const char* cmd_g2liststatus(const String& args);
const char* cmd_g2listfeed(const String& args);
const char* cmd_g2listpush(const String& args);
const char* cmd_g2listpreview(const String& args);

#else

inline void g2QuickListTick() {}
inline uint32_t g2QuickListOwnerWaitMs() { return UINT32_MAX; }
inline void g2QuickListOwnerStopped() {}
inline bool g2QuickListOnRx(uint32_t, uint8_t, const uint8_t*, size_t) { return false; }
inline void g2QuickListOnSilentPush(uint32_t, bool) {}

#endif

#endif  // G2_QUICKLIST_H
