// Host harness for the mesh master's side of forwarding for a bond partner:
// the production code of System_ESPNow_ForwardRx.cpp between
// FORWARD_RX_HOST_BEGIN and FORWARD_RX_HOST_END (the link table, the
// FORWARD_LINK handler, espnowForwardResolve and the readers,
// espnowforwardfrom and espnowforwardlinks), compiled as it is with the
// production rules header System_ESPNow_ForwardCore.h.
//
// Host boundaries, all in this file: the uptime, the role, this device's
// address and name, the paired devices (address, key, name), the setting
// espnowforwardfrom, the radio (a recording queue), the console, the reply
// buffer, the allocator and the lock (it records whether anything that may
// block, print, send or allocate runs under it).
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "System_ESPNow_ForwardCore.h"
#include "System_ESPNow_HandshakeBackoff.h"
#include "System_ESPNow_PowerEventCore.h"
#include "System_MacText.h"

class String {
 public:
  String() = default;
  String(const char* text) : text_(text ? text : "") {}
  const char* c_str() const { return text_.c_str(); }
 private:
  std::string text_;
};

#define EXT_RAM_BSS_ATTR
#define RETURN_VALID_IF_VALIDATE_CSTR()
static const char kCommandReplyOutOfMemory[] = "Error: out of memory";

static int gLockDepth = 0;
static unsigned gLockViolations = 0;
#define FWD_LOCK()   (++gLockDepth)
#define FWD_UNLOCK() (--gLockDepth)
static void notUnderLock() {
  if (gLockDepth != 0) ++gLockViolations;
}

// ---- The device -------------------------------------------------------------------------------
static uint32_t gNowMs = 1000;
static uint8_t gRole = 1;
static const uint8_t kSelf[6] = { 0x02, 0xC0, 0, 0, 0, 0x01 };
static std::string gSelfName = "hub";
static std::string gAllowed;
static unsigned gSettingWrites = 0;
struct PairedPeer {
  uint8_t mac[6];
  uint8_t key[32];
  std::string name;
};
static std::vector<PairedPeer> gPaired;
static const PairedPeer* pairedOf(const uint8_t* mac) {
  for (const auto& p : gPaired) {
    if (std::memcmp(p.mac, mac, 6) == 0) return &p;
  }
  return nullptr;
}
struct Sent {
  uint8_t dst[6];
  espnowfwd::LinkFrame f;
};
static std::vector<Sent> gSent;
static std::vector<std::string> gConsole;
static std::vector<espnowfwd::LinkFrame> gToBondSide;
static bool gBondSideTakes = false;
static bool gAllocFails = false;
static unsigned gAllocs = 0;
static char gReply[4096];
static size_t gReplyCap = sizeof(gReply);

static uint32_t fwdNow() { return gNowMs; }
static uint8_t fwdRole() { return gRole; }
static void fwdSelfMac(uint8_t out[6]) { std::memcpy(out, kSelf, 6); }
static void fwdAllowedText(char* out, size_t cap) { std::snprintf(out, cap, "%s", gAllowed.c_str()); }
static void fwdSetAllowedText(const char* text) {
  notUnderLock();
  gAllowed = text;
  ++gSettingWrites;
}
static bool fwdPairedKey(const uint8_t mac[6], uint8_t key[32]) {
  notUnderLock();
  const PairedPeer* p = pairedOf(mac);
  if (!p) return false;
  std::memcpy(key, p->key, 32);
  return true;
}
static bool fwdIsPaired(const uint8_t mac[6]) {
  notUnderLock();
  return pairedOf(mac) != nullptr;
}
static void fwdPeerName(const uint8_t mac[6], char* out, size_t cap) {
  notUnderLock();
  const PairedPeer* p = pairedOf(mac);
  std::snprintf(out, cap, "%s", p ? p->name.c_str() : "");
}
static bool fwdNameTaken(const char* name) {
  notUnderLock();
  if (espnowfwd::nameSameIgnoringCase(name, gSelfName.c_str())) return true;
  for (const auto& p : gPaired) {
    if (espnowfwd::nameSameIgnoringCase(name, p.name.c_str())) return true;
  }
  return false;
}
static bool fwdResolveName(const char* who, uint8_t mac[6]) {
  for (const auto& p : gPaired) {
    char text[18];
    macToDisplay(p.mac, text, sizeof(text));
    if (p.name == who || std::string(text) == who) {
      std::memcpy(mac, p.mac, 6);
      return true;
    }
  }
  return false;
}
static bool fwdSend(const uint8_t mac[6], const uint8_t* payload, uint16_t len) {
  notUnderLock();
  Sent s{};
  std::memcpy(s.dst, mac, 6);
  if (len != espnowfwd::kLinkFrameLen || !espnowfwd::parseLinkFrame(payload, len, s.f)) std::abort();
  gSent.push_back(s);
  return true;
}
static void* fwdAlloc(size_t bytes) {
  notUnderLock();
  ++gAllocs;
  return gAllocFails ? nullptr : std::malloc(bytes);
}
static void fwdConsole(const char* fmt, ...) {
  notUnderLock();
  char line[256];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(line, sizeof(line), fmt, args);
  va_end(args);
  gConsole.push_back(line);
}
static char* fwdReply(size_t* cap) {
  *cap = gReplyCap;
  std::memset(gReply, 0x7E, sizeof(gReply));
  return gReply;
}
static bool fwdToBondSide(const uint8_t[6], const espnowfwd::LinkFrame& f) {
  notUnderLock();
  gToBondSide.push_back(f);
  return gBondSideTakes;
}

// INSERT_PRODUCTION

// ---- Cases -------------------------------------------------------------------------------------
static int failures = 0;
static int checks = 0;
static void check(bool ok, const std::string& what) {
  ++checks;
  if (ok) return;
  ++failures;
  std::fprintf(stderr, "FAIL: %s\n", what.c_str());
}
static bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

static const uint8_t kGlasses[6] = { 0x02, 0xB0, 0, 0, 0, 0x01 };  // a bond master (the via)
static const uint8_t kShed[6] = { 0x02, 0xB0, 0, 0, 0, 0x02 };     // another paired device
static const uint8_t kPorch[6] = { 0x02, 0xA0, 0, 0, 0, 0x01 };    // glasses' partner, not paired here
static const uint8_t kPorch2[6] = { 0x02, 0xA0, 0, 0, 0, 0x02 };

static void pair(const uint8_t mac[6], const char* name, uint8_t keyByte) {
  PairedPeer p{};
  std::memcpy(p.mac, mac, 6);
  std::memset(p.key, keyByte, sizeof(p.key));
  p.name = name;
  gPaired.push_back(p);
}

static void resetAll() {
  std::free(sFwd.links);
  std::memset((void*)&sFwd, 0, sizeof(sFwd));
  gNowMs += 1000000;
  gRole = 1;
  gAllowed.clear();
  gPaired.clear();
  pair(kGlasses, "glasses", 0x11);
  pair(kShed, "shed", 0x22);
  gSent.clear();
  gConsole.clear();
  gToBondSide.clear();
  gBondSideTakes = false;
  gAllocFails = false;
  gReplyCap = sizeof(gReply);
}

static espnowfwd::LinkFrame announceOf(uint8_t gen, uint16_t bootTag, const uint8_t partner[6], const char* name,
                                       uint8_t keyByte = 0x55) {
  espnowfwd::LinkFrame f{};
  f.op = espnowfwd::kOpAnnounce;
  f.linkGen = gen;
  f.viaBootTag = bootTag;
  f.streams = espnowfwd::kStreamsAll;
  f.sensorMask = espnowfwd::kSensorMaskDefault;
  std::memcpy(f.partnerMac, partner, 6);
  std::memset(f.partnerKey, keyByte, sizeof(f.partnerKey));
  espnowsensor::eventFieldPut(f.partnerName, sizeof(f.partnerName), name, std::strlen(name) + 1);
  return f;
}
static void rx(const uint8_t from[6], const char* name, const espnowfwd::LinkFrame& f) {
  uint8_t payload[espnowfwd::kLinkFrameLen];
  espnowfwd::packLinkFrame(payload, f);
  forwardLinkIngest(from, name, payload, sizeof(payload));
}
static uint8_t lastResult() { return gSent.empty() ? 0 : gSent.back().f.result; }
static std::string cmd(const char* (*fn)(const String&), const char* args) { return fn(String(args)); }
static void tick() {
  gNowMs += 1000;
  espnowForwardRxTick(gNowMs);
}
static ForwardResolve resolve(const uint8_t src[6], bool bitSet, uint8_t fwdLink, uint8_t stream, ForwardOrigin* out,
                              bool hasBit = true) {
  return espnowForwardResolve(src, "glasses", false, hasBit, bitSet, fwdLink, stream, out);
}

static void testAnnounce() {
  resetAll();
  // A worker keeps nothing: not_receiver.
  gRole = 0;
  rx(kGlasses, "glasses", announceOf(1, 7, kPorch, "porch"));
  check(gSent.size() == 1 && std::memcmp(gSent[0].dst, kGlasses, 6) == 0 && gSent[0].f.op == espnowfwd::kOpAnswer &&
        gSent[0].f.answers == espnowfwd::kOpAnnounce && lastResult() == espnowfwd::kResultNotReceiver,
        "a worker answers not_receiver");
  // A master that lists nobody: not_allowed, and it says so (a minute later:
  // refusals are said once a minute).
  gRole = 1;
  gNowMs += 60000;
  rx(kGlasses, "glasses", announceOf(1, 7, kPorch, "porch"));
  check(lastResult() == espnowfwd::kResultNotAllowed && !gConsole.empty() &&
        has(gConsole.back(), "glasses asked to forward for porch (02:A0:00:00:00:01), refused: it does not accept"),
        "by default nobody may forward: not_allowed, said on the console");
  // espnowforwardfrom add glasses.
  check(cmd(cmd_espnow_forwardfrom, "add glasses").rfind("OK: glasses may forward", 0) == 0 &&
        gAllowed == "02:B0:00:00:00:01/1111111111111111", "espnowforwardfrom add stores the address and its key");
  check(cmd(cmd_espnow_forwardfrom, "add glasses") == "OK: glasses may forward to this device already",
        "adding it again says so");
  check(cmd(cmd_espnow_forwardfrom, "add nobody") == "Error: no such device (use its name or its MAC address)",
        "an unknown device is refused");
  check(cmd(cmd_espnow_forwardfrom, "") ==
            "OK: these devices may forward their bond partner's data to this device: glasses (02:B0:00:00:00:01)",
        "no argument lists them");
  check(cmd(cmd_espnow_forwardfrom, "bogus x") ==
            "Usage: espnowforwardfrom [add <name_or_mac> | remove <name_or_mac> | clear]",
        "a wrong word is the usage");
  gConsole.clear();
  rx(kGlasses, "glasses", announceOf(1, 7, kPorch, "porch"));
  const espnowfwd::LinkFrame a = gSent.back().f;
  check(a.result == espnowfwd::kResultAccepted && a.linkGen == 1 && a.viaBootTag == 7 && a.streams == espnowfwd::kStreamsAll &&
        a.sensorMask == espnowfwd::kSensorMaskDefault && std::memcmp(a.partnerMac, kPorch, 6) == 0,
        "accepted: the answer echoes the link number, boot tag, streams, sensors and partner");
  check(gConsole.size() == 1 &&
        gConsole[0] == "[ESP-NOW] glasses forwards the readings, battery state and events of its bond partner porch (02:A0:00:00:00:01)",
        "the console says it once");
  rx(kGlasses, "glasses", announceOf(1, 7, kPorch, "porch"));
  check(lastResult() == espnowfwd::kResultAccepted && gConsole.size() == 1, "a repeat: accepted again, said no more");
  // Bad partners.
  rx(kGlasses, "glasses", announceOf(2, 7, kGlasses, "me"));
  check(lastResult() == espnowfwd::kResultBadPartner, "the partner is the sender itself: bad_partner");
  rx(kGlasses, "glasses", announceOf(2, 7, kSelf, "you"));
  check(lastResult() == espnowfwd::kResultBadPartner, "the partner is this device: bad_partner");
  espnowfwd::LinkFrame zeroKey = announceOf(2, 7, kPorch2, "porch2", 0);
  rx(kGlasses, "glasses", zeroKey);
  check(lastResult() == espnowfwd::kResultBadPartner, "no key: bad_partner");
  // A name another paired device (or this device) has: name_taken.
  rx(kGlasses, "glasses", announceOf(2, 7, kPorch2, "SHED"));
  check(lastResult() == espnowfwd::kResultNameTaken, "the name of a paired device: name_taken");
  rx(kGlasses, "glasses", announceOf(2, 7, kPorch2, "Hub"));
  check(lastResult() == espnowfwd::kResultNameTaken, "this device's own name: name_taken");
  // A partner paired here: its key must match, and this device's own name is shown.
  pair(kPorch2, "garden", 0x77);
  rx(kGlasses, "glasses", announceOf(2, 7, kPorch2, "shed", 0x78));
  check(lastResult() == espnowfwd::kResultKeyMismatch, "paired under another key: key_mismatch");
  rx(kGlasses, "glasses", announceOf(2, 7, kPorch2, "shed", 0x77));
  check(lastResult() == espnowfwd::kResultAccepted, "paired under the same key: accepted (the announced name is not used)");
  char shown[33];
  espnowForwardOriginName(kGlasses, kPorch2, shown, sizeof(shown));
  check(std::string(shown) == "garden", "this device's own name for a paired partner is shown");
  // The link replaced the old one: the old partner's frames are refused.
  ForwardOrigin o;
  check(resolve(kGlasses, true, 1, REMOTE_SENSOR_GPS, &o) == ForwardResolve::Refused, "the old link's frames are refused");
  // A held announcement of the old partner arrives late: stale, not applied.
  rx(kGlasses, "glasses", announceOf(1, 7, kPorch, "porch"));
  check(lastResult() == espnowfwd::kResultStale && resolve(kGlasses, true, 2, REMOTE_SENSOR_GPS, &o) == ForwardResolve::Forwarded &&
        std::memcmp(o.mac, kPorch2, 6) == 0, "a late announcement does not bring the old partner back");
  // Malformed and unknown frames.
  const uint8_t shortFrame[10] = { 1 };
  forwardLinkIngest(kGlasses, "glasses", shortFrame, sizeof(shortFrame));
  uint8_t bad[espnowfwd::kLinkFrameLen] = { 9 };
  forwardLinkIngest(kGlasses, "glasses", bad, sizeof(bad));
  check(sFwd.refused.malformed == 2, "a short frame and an unknown op are counted malformed");
  // A query or an answer is for the bond master's side.
  espnowfwd::LinkFrame q{};
  q.op = espnowfwd::kOpQuery;
  const size_t sentBefore = gSent.size();
  rx(kGlasses, "glasses", q);
  check(gToBondSide.size() == 1 && sFwd.refused.ignored == 1 && gSent.size() == sentBefore,
        "a query goes to the bond master's side (none here: counted ignored), unanswered");
  gBondSideTakes = true;
  rx(kGlasses, "glasses", q);
  check(gToBondSide.size() == 2 && sFwd.refused.ignored == 1, "a bond build takes it");
}

static void testResolveAndWithdraw() {
  resetAll();
  gAllowed = "02:B0:00:00:00:01/1111111111111111,02:B0:00:00:00:02/2222222222222222";
  rx(kGlasses, "glasses", announceOf(3, 9, kPorch, "porch"));
  check(lastResult() == espnowfwd::kResultAccepted, "accepted");
  ForwardOrigin o;
  // Nothing forwarded: own, without a lookup.
  check(resolve(kGlasses, false, 0, REMOTE_SENSOR_GPS, &o) == ForwardResolve::Own && std::memcmp(o.mac, kGlasses, 6) == 0 &&
        o.link == espnowfwd::kLinkMesh && std::string(o.name) == "glasses", "an own frame is the sender's");
  check(espnowForwardResolve(kGlasses, "glasses", true, true, false, 0, REMOTE_SENSOR_GPS, &o) == ForwardResolve::Own &&
        o.link == espnowfwd::kLinkBond, "over the bond link: the sender's, link bond");
  // Forwarded: the partner, via glasses, named as the link shows, the resume pending.
  check(resolve(kGlasses, true, 3, REMOTE_SENSOR_GPS, &o) == ForwardResolve::Forwarded && std::memcmp(o.mac, kPorch, 6) == 0 &&
        std::memcmp(o.via, kGlasses, 6) == 0 && o.link == espnowfwd::kLinkForwarded && std::string(o.name) == "porch" &&
        o.resumePending, "a forwarded frame: the partner via glasses, the first frame of its stream a resume");
  espnowForwardResumeTaken(kGlasses, REMOTE_SENSOR_GPS);
  check(resolve(kGlasses, true, 3, REMOTE_SENSOR_GPS, &o) == ForwardResolve::Forwarded && !o.resumePending,
        "the resume is taken once");
  check(resolve(kGlasses, false, 3, espnowfwd::kStreamIdBattery, &o, false) == ForwardResolve::Forwarded && o.resumePending,
        "the battery frame's byte alone");
  check(espnowForwardLinkActive(kGlasses, kPorch) && !espnowForwardLinkActive(kGlasses, kPorch2) &&
        !espnowForwardLinkActive(kShed, kPorch), "the link names its partner");
  // Malformed marks are counted here and refused.
  check(resolve(kGlasses, true, 0, REMOTE_SENSOR_GPS, &o) == ForwardResolve::Malformed &&
        espnowForwardResolve(kGlasses, "glasses", true, true, true, 3, REMOTE_SENSOR_GPS, &o) == ForwardResolve::Malformed,
        "a bit without the byte, or a mark on the bond link: malformed");
  // Another device without a link: refused, asked which link it has, once every 5 s.
  gSent.clear();
  check(resolve(kShed, true, 1, REMOTE_SENSOR_GPS, &o) == ForwardResolve::Refused && gSent.size() == 1 &&
        std::memcmp(gSent[0].dst, kShed, 6) == 0 && gSent[0].f.op == espnowfwd::kOpQuery, "unannounced: refused, a query");
  resolve(kShed, true, 1, REMOTE_SENSOR_GPS, &o);
  gNowMs += 4999;
  resolve(kShed, true, 1, REMOTE_SENSOR_GPS, &o);
  check(gSent.size() == 1, "no second query within 5 s");
  gNowMs += 1;
  resolve(kShed, true, 1, REMOTE_SENSOR_GPS, &o);
  check(gSent.size() == 2, "the next after 5 s");
  // An old link number: refused and asked as well; a stream not announced: refused, not asked.
  gSent.clear();
  check(resolve(kGlasses, true, 2, REMOTE_SENSOR_GPS, &o) == ForwardResolve::Refused && gSent.size() == 1 &&
        gSent[0].f.op == espnowfwd::kOpQuery, "an old link number: refused, a query");
  check(resolve(kGlasses, true, 3, REMOTE_SENSOR_INPUT, &o) == ForwardResolve::Refused && gSent.size() == 1,
        "input is not announced by default: refused, no query");
  check(sFwd.refused.unannounced == 4 && sFwd.refused.staleLink == 1 && sFwd.refused.streamNotAnnounced == 1,
        "every refusal is counted");
  // A worker refuses forwarded data too, and asks nothing.
  gRole = 0;
  gSent.clear();
  gNowMs += 6000;
  check(resolve(kShed, true, 1, REMOTE_SENSOR_GPS, &o) == ForwardResolve::Refused && gSent.empty(),
        "a worker does not ask");
  gRole = 1;

  // Withdraw: answered, the link ends, its frames are refused, said once.
  gConsole.clear();
  espnowfwd::LinkFrame w{};
  w.op = espnowfwd::kOpWithdraw;
  w.linkGen = 3;
  w.viaBootTag = 9;
  std::memcpy(w.partnerMac, kPorch, 6);
  rx(kGlasses, "glasses", w);
  check(lastResult() == espnowfwd::kResultWithdrawn && gSent.back().f.answers == espnowfwd::kOpWithdraw &&
        !espnowForwardLinkActive(kGlasses, kPorch) &&
        resolve(kGlasses, true, 3, REMOTE_SENSOR_GPS, &o) == ForwardResolve::Refused,
        "withdrawn: answered, ended, its frames refused");
  check(gConsole.size() == 1 && gConsole[0] == "[ESP-NOW] glasses no longer forwards for porch", "said once");
  rx(kGlasses, "glasses", w);
  check(lastResult() == espnowfwd::kResultWithdrawn && gConsole.size() == 1, "a repeated withdraw is harmless");
  // The same number again (a held announcement): stale; a newer one: accepted.
  rx(kGlasses, "glasses", announceOf(3, 9, kPorch, "porch"));
  check(lastResult() == espnowfwd::kResultStale, "an announcement of the ended link: stale");
  rx(kGlasses, "glasses", announceOf(4, 9, kPorch, "porch"));
  check(lastResult() == espnowfwd::kResultAccepted && espnowForwardLinkActive(kGlasses, kPorch), "a newer one: accepted");
  // B restarted with nothing to forward: the withdrawal of everything ends it.
  espnowfwd::LinkFrame all{};
  all.op = espnowfwd::kOpWithdraw;
  all.viaBootTag = 10;
  rx(kGlasses, "glasses", all);
  check(lastResult() == espnowfwd::kResultWithdrawn && !espnowForwardLinkActive(kGlasses, kPorch),
        "a withdrawal of everything from B's next boot ends the link");
  check(espnowForwardLinkCount() == 0, "no active link");

  // espnowforwardfrom remove: the link ends within a second.
  rx(kGlasses, "glasses", announceOf(1, 10, kPorch, "porch"));
  check(espnowForwardLinkActive(kGlasses, kPorch) && espnowForwardLinkCount() == 1, "active again");
  check(cmd(cmd_espnow_forwardfrom, "remove glasses").rfind("OK: glasses may no longer forward", 0) == 0 &&
        gAllowed == "02:B0:00:00:00:02/2222222222222222", "remove takes it off the list");
  tick();
  check(!espnowForwardLinkActive(kGlasses, kPorch), "and its link ends");
  // The setting changed by any editor is read again by the tick.
  gAllowed = "02:B0:00:00:00:01/1111111111111111";
  tick();
  rx(kGlasses, "glasses", announceOf(2, 10, kPorch, "porch"));
  check(lastResult() == espnowfwd::kResultAccepted, "a list changed elsewhere is read once a second");
  check(cmd(cmd_espnow_forwardfrom, "clear").rfind("OK: no device may forward", 0) == 0 && gAllowed.empty(), "clear empties it");
  tick();
  check(!espnowForwardLinkActive(kGlasses, kPorch), "and every link ends");
  // Becoming a worker ends the links too.
  gAllowed = "02:B0:00:00:00:01/1111111111111111";
  tick();
  rx(kGlasses, "glasses", announceOf(3, 10, kPorch, "porch"));
  gRole = 0;
  tick();
  check(!espnowForwardLinkActive(kGlasses, kPorch), "a device that no longer keeps data ends its links");
  gRole = 1;
}

static void testTableAndReplies() {
  resetAll();
  // Sixteen vias, each listed (four at most) - the table holds sixteen entries:
  // a seventeenth is refused only while every holder is paired.
  std::string allowed;
  for (uint8_t i = 0; i < 4; ++i) {
    uint8_t mac[6] = { 0x02, 0xE0, 0, 0, 0, i };
    char name[16];
    std::snprintf(name, sizeof(name), "b%u", i);
    pair(mac, name, (uint8_t)(0x30 + i));
    char text[18];
    macToDisplay(mac, text, sizeof(text));
    char key[20];
    std::snprintf(key, sizeof(key), "/%02X%02X%02X%02X%02X%02X%02X%02X", 0x30 + i, 0x30 + i, 0x30 + i, 0x30 + i, 0x30 + i,
                  0x30 + i, 0x30 + i, 0x30 + i);
    allowed += (allowed.empty() ? "" : ",") + std::string(text) + key;
  }
  gAllowed = allowed;
  check(cmd(cmd_espnow_forwardfrom, "add glasses") == "Error: 4 devices are listed already; remove one first",
        "at most four devices are listed");
  for (uint8_t i = 0; i < 4; ++i) {
    uint8_t mac[6] = { 0x02, 0xE0, 0, 0, 0, i };
    uint8_t partner[6] = { 0x02, 0xF0, 0, 0, 0, i };
    char name[16];
    std::snprintf(name, sizeof(name), "p%u", i);
    rx(mac, "b", announceOf(1, 1, partner, name));
    check(lastResult() == espnowfwd::kResultAccepted, "each listed device gets its link");
  }
  // The rest of the table filled by devices that only sent forwarded data (queried).
  for (uint8_t i = 0; i < 12; ++i) {
    uint8_t mac[6] = { 0x02, 0xE1, 0, 0, 0, i };
    pair(mac, "x", 0x40);
    ForwardOrigin o;
    resolve(mac, true, 1, REMOTE_SENSOR_GPS, &o);
  }
  uint8_t late[6] = { 0x02, 0xE0, 0, 0, 0, 3 };
  (void)late;
  ForwardOrigin o;
  uint8_t seventeenth[6] = { 0x02, 0xE2, 0, 0, 0, 1 };
  pair(seventeenth, "y", 0x41);
  const size_t sent = gSent.size();
  resolve(seventeenth, true, 1, REMOTE_SENSOR_GPS, &o);
  check(gSent.size() == sent, "no entry left for a seventeenth device: no query either");
  // An entry of a device that is no longer paired is reused.
  gPaired.erase(gPaired.end() - 2);  // the last 'x'
  resolve(seventeenth, true, 1, REMOTE_SENSOR_GPS, &o);
  check(gSent.size() == sent + 1 && std::memcmp(gSent.back().dst, seventeenth, 6) == 0,
        "the entry of an unpaired device is reused");

  // espnowforwardlinks: the text and the JSON, paged.
  gConsole.clear();
  const std::string text = cmd(cmd_espnow_forwardlinks, "");
  check(text == "OK: 16 links" && gConsole.size() == 17 &&
        gConsole[0] == "[ESP-NOW] Devices that forward their bond partner's data to this device" &&
        has(gConsole[1], "b0 forwards for p0 (02:F0:00:00:00:00): readings, battery, events; link 1, since ") &&
        has(gConsole[5], "sent forwarded data without a link; asked which link it has"),
        "the text form: one line per entry");
  const std::string json = cmd(cmd_espnow_forwardlinks, "json");
  check(json.rfind("{\"schema\":1,\"role\":\"master\",\"allowed\":[\"02:E0:00:00:00:00\",", 0) == 0 &&
        has(json, "{\"via\":\"02:E0:00:00:00:00\",\"viaName\":\"b0\",\"partner\":\"02:F0:00:00:00:00\",\"name\":\"p0\","
                  "\"namePaired\":false,\"linkGen\":1,\"viaBootTag\":1,\"state\":\"active\",\"confirmed\":true,"
                  "\"streams\":[\"readings\",\"battery\",\"events\"],\"sensors\":[\"thermal\",\"tof\",\"imu\",\"gps\"") &&
        has(json, "\"refused\":{\"malformed\":0,\"unannounced\":14,"), "the JSON form");
  // Sixteen do not fit one 4 KB reply: 'next' pages through all of them, each once.
  unsigned links = 0, queried = 0, pages = 0;
  std::string page = json;
  for (;;) {
    ++pages;
    check(page.size() < sizeof(gReply) && page.back() == '}', "each page is whole");
    for (size_t at = page.find("{\"via\":"); at != std::string::npos; at = page.find("{\"via\":", at + 1)) ++links;
    for (size_t at = page.find("\"state\":\"queried\""); at != std::string::npos;
         at = page.find("\"state\":\"queried\"", at + 1)) {
      ++queried;
    }
    if (has(page, "\"next\":null}") || pages > 8) break;
    const int next = std::atoi(page.c_str() + page.find("\"next\":") + 7);
    char more[32];
    std::snprintf(more, sizeof(more), "json from %d", next);
    page = cmd(cmd_espnow_forwardlinks, more);
    check(has(page, "\"from\":" + std::to_string(next)), "'from' continues where the last page stopped");
  }
  check(pages >= 2 && links == 16 && queried == 12, "all sixteen entries, over the pages, each once");
  check(cmd(cmd_espnow_forwardlinks, "from x") == "Usage: espnowforwardlinks [json] [from <n>]", "a bad number is the usage");
  // Thirty minutes without an announcement: shown "not confirmed".
  gNowMs += espnowfwd::kConfirmedForMs;
  check(has(cmd(cmd_espnow_forwardlinks, "json"), "\"confirmed\":false"), "not confirmed after 30 minutes");
  // Nothing at all.
  resetAll();
  check(cmd(cmd_espnow_forwardlinks, "") == "OK: no device forwards its bond partner's data to this device", "none");
  gRole = 0;
  check(cmd(cmd_espnow_forwardlinks, "") == "OK: this device keeps no forwarded data (only a mesh master or backup master does)",
        "a worker keeps none");
  // No memory for the table: full, and nothing allocated twice per frame.
  gRole = 1;
  gAllowed = "02:B0:00:00:00:01/1111111111111111";
  gAllocFails = true;
  rx(kGlasses, "glasses", announceOf(1, 1, kPorch, "porch"));
  check(lastResult() == espnowfwd::kResultFull, "no memory for the table: full");
  gAllocFails = false;
}

// Review findings of 2026-11-18.
static void testReviewCases() {
  // A refused announcement is said once a minute at most: any paired device
  // can send them at frame rate.
  resetAll();
  for (int i = 0; i < 100; ++i) rx(kShed, "shed", announceOf((uint8_t)(i + 1), 3, kPorch, "porch"));
  check(gSent.size() == 100 && lastResult() == espnowfwd::kResultNotAllowed, "every refused announcement is answered");
  check(gConsole.size() == 1 && !has(gConsole[0], "more refused"), "but said once");
  gNowMs += 59999;
  rx(kShed, "shed", announceOf(101, 3, kPorch, "porch"));
  check(gConsole.size() == 1, "not again within the minute");
  gNowMs += 1;
  rx(kShed, "shed", announceOf(102, 3, kPorch, "porch"));
  check(gConsole.size() == 2 && has(gConsole[1], "refused: it does not accept forwarded data from this device "
                                                "(espnowforwardfrom there) (100 more refused since the last such line)"),
        "then once more, with the count held back");

  // The allow list names a device by its address and its key: another
  // identity paired later under the same address is not allowed.
  resetAll();
  check(cmd(cmd_espnow_forwardfrom, "add glasses").rfind("OK: glasses may forward", 0) == 0, "glasses listed");
  rx(kGlasses, "glasses", announceOf(1, 7, kPorch, "porch"));
  check(lastResult() == espnowfwd::kResultAccepted, "accepted under the key it was listed with");
  gPaired.erase(gPaired.begin());  // unpaired ...
  pair(kGlasses, "glasses", 0x99);  // ... and another identity paired under the same address
  tick();
  check(!espnowForwardLinkActive(kGlasses, kPorch), "the link of the listed identity ends when another holds its address");
  rx(kGlasses, "glasses", announceOf(2, 7, kPorch, "porch"));
  check(lastResult() == espnowfwd::kResultNotAllowed, "the new identity may not forward");
  check(cmd(cmd_espnow_forwardfrom, "") ==
            "OK: these devices may forward their bond partner's data to this device: glasses (02:B0:00:00:00:01), paired "
            "again under another key: not allowed until added again",
        "the list says why");
  check(cmd(cmd_espnow_forwardfrom, "add glasses") == "OK: glasses is listed again under the key it is paired with now." &&
            gAllowed == "02:B0:00:00:00:01/9999999999999999",
        "adding it again lists it under its key now");
  rx(kGlasses, "glasses", announceOf(3, 7, kPorch, "porch"));
  check(lastResult() == espnowfwd::kResultAccepted, "and it may forward");
  gPaired.erase(gPaired.end() - 1);
  check(cmd(cmd_espnow_forwardfrom, "") ==
            "OK: these devices may forward their bond partner's data to this device: 02:B0:00:00:00:01 (02:B0:00:00:00:01), "
            "not paired",
        "listed while unpaired, and said so");
  rx(kGlasses, "glasses", announceOf(4, 7, kPorch, "porch"));
  check(lastResult() == espnowfwd::kResultNotAllowed, "not paired: not allowed");
  // An address alone (typed into the setting by hand) allows nobody.
  resetAll();
  gAllowed = "02:B0:00:00:00:01";
  tick();
  rx(kGlasses, "glasses", announceOf(1, 7, kPorch, "porch"));
  check(lastResult() == espnowfwd::kResultNotAllowed, "an address without its key allows nobody");
}

int main() {
  testAnnounce();
  testResolveAndWithdraw();
  testTableAndReplies();
  testReviewCases();
  check(gLockViolations == 0, "nothing that may block, print, send or allocate ran under the lock");
  check(gLockDepth == 0, "every lock was released");
  if (failures != 0) {
    std::fprintf(stderr, "%d of %d forward receive checks failed\n", failures, checks);
    return 1;
  }
  std::printf("ESP-NOW forwarding, mesh master side: %d checks passed\n", checks);
  return 0;
}
