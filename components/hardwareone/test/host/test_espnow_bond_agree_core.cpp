// The bond agreement rules (System_ESPNow_BondAgreeCore.h, compiled as is)
// between modelled devices over a channel that can lose frames. The device
// glue mirrors bondAgreementTick in System_ESPNow.cpp: take what the handlers
// kept, send the answers, tell a peer whose bond frames were ignored, ask the
// partner. test_espnow_bond_agreement.py runs the production glue itself.
//
// Cases: two devices bonded to each other, a one-sided bond over an hour, a
// partner bonded elsewhere, ENDED and bonding again, a request overtaken by
// its sender's ENDED, the same role on both sides, notices that must not
// count, lost frames, the inbox and peer table
// limits (more peers than entries, a peer that asks without pause), the
// request timing (session wait, the side that answered the handshake, failed
// send, declined partner with and without a session, a decline after weeks of
// agreement, clock wrap), the heartbeat of a partner without these frames, and
// the texts. The role: bondconnect keeps a chosen role, a device whose role is
// automatic takes the other role than a partner whose user chose (from its
// request or its notice, also when a request is lost), both chose the same:
// a conflict, neither chose: nothing changes, a partner without the flag
// counts as not chosen, an ENDED says the role but changes nothing, a peer's
// request is remembered for a later bondconnect to it.
#include "System_ESPNow_BondAgreeCore.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

static unsigned gChecks = 0;
#define CHECK(c)                                                                 \
  do {                                                                           \
    ++gChecks;                                                                   \
    if (!(c)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); \
      std::exit(1);                                                              \
    }                                                                            \
  } while (0)

using namespace bondagree;

struct Frame {
  bool isRequest;
  int from, to;
  uint8_t role, code;
  uint16_t seq;
  uint8_t flags = 0;  // a notice's flags
};

struct Device {
  uint8_t mac[6];
  State s;
  Local local;
  std::vector<Line> lines;
  std::vector<uint8_t> lineReasons;
  int declinedEdges = 0;
  int handshakeKicks = 0;
  int roleSwitches = 0;
  bool queueFull = false;
  bool oldFirmware = false;  // firmware from before role choice: sends no role flags, never yields
};

static const int kDevices = 3;
static Device gDev[kDevices];
static bool gSession[kDevices][kDevices];
static std::vector<Frame> gAir;       // sent, not yet delivered
static std::vector<Frame> gSentLog;   // everything ever sent
static std::function<bool(const Frame&)> gLose;
static uint32_t gNow = 1000;

static int indexOf(const uint8_t mac[6]) {
  for (int i = 0; i < kDevices; ++i) {
    if (macIs(gDev[i].mac, mac)) return i;
  }
  return -1;
}

static void resetWorld(uint32_t startMs = 1000) {
  gNow = startMs;
  gAir.clear();
  gSentLog.clear();
  gLose = nullptr;
  for (int i = 0; i < kDevices; ++i) {
    gDev[i] = Device();
    const uint8_t mac[6] = {0x02, 0x11, 0x22, 0x33, 0x44, (uint8_t)(i + 1)};
    std::memcpy(gDev[i].mac, mac, 6);
    for (int j = 0; j < kDevices; ++j) gSession[i][j] = true;
  }
}

// bondconnect on `who` toward `partner`: the setting, then the reset every
// partner change makes.
static void bond(int who, int partner, uint8_t role) {
  Device& d = gDev[who];
  d.local.bonded = true;
  std::memcpy(d.local.partner, gDev[partner].mac, 6);
  d.local.role = role;
  reset(d.s);
}

static void send(int from, int to, const Frame& f) {
  gSentLog.push_back(f);
  if (!gSession[from][to]) return;                 // sealed with a session nobody has
  if (gLose && gLose(f)) return;
  gAir.push_back(f);
}

// bondAgreementTick.
static void tick(int who) {
  Device& d = gDev[who];
  if (!hasWork(d.s, gNow)) return;
  for (;;) {
    const Outcome out = takeInbound(d.s, d.local, gNow);
    if (!out.handled) break;
    const int peer = indexOf(out.mac);
    if (out.sendNotice && gSession[who][peer]) {
      const bool withRole = out.noticeReason == kReasonAccepted || out.noticeReason == kReasonRoleConflict;
      if (d.oldFirmware) {
        // Its answer: its own role, and a conflict whenever the roles match.
        const uint8_t reason = out.noticeReason == kReasonAccepted && gDev[peer].local.role == d.local.role
                                   ? kReasonRoleConflict : out.noticeReason;
        send(who, peer, Frame{false, who, peer, d.local.role, reason, out.noticeSeq, 0});
      } else {
        send(who, peer, Frame{false, who, peer, out.noticeRole, out.noticeReason, out.noticeSeq,
                              (uint8_t)(withRole && d.local.roleChosen ? kNoticeFlagRoleChosen : 0)});
      }
    }
    if (out.switchRole && !d.oldFirmware) {
      d.local.role = out.newRole;
      d.roleSwitches++;
    }
    if (out.becameDeclined) d.declinedEdges++;
    if (out.line != Line::None) {
      d.lines.push_back(out.line);
      d.lineReasons.push_back(out.lineReason);
    }
  }
  for (;;) {
    uint8_t to[6];
    uint8_t reason = 0;
    uint16_t seq = 0;
    if (!takeOwedNotice(d.s, d.local, gNow, to, &reason, &seq)) break;
    const int peer = indexOf(to);
    if (gSession[who][peer]) send(who, peer, Frame{false, who, peer, d.local.role, reason, seq});
  }
  const int partner = d.local.bonded ? indexOf(d.local.partner) : -1;
  const bool sessionActive = partner >= 0 && gSession[who][partner];
  const RequestPlan plan = planRequest(d.s, d.local, gNow, sessionActive);
  if (plan.send) {
    const bool queued = !d.queueFull;
    const uint8_t flags = d.oldFirmware ? plan.flags
                                        : (uint8_t)(plan.flags | kRequestFlagRoleYields |
                                                    (d.local.roleChosen ? kRequestFlagRoleChosen : 0));
    if (queued) send(who, partner, Frame{true, who, partner, d.local.role, flags, plan.seq});
    requestResult(d.s, gNow, queued);
  } else if (plan.kickHandshake) {
    d.handshakeKicks++;
  }
}

// The receive handlers: keep the frame for the tick.
static void deliver() {
  std::vector<Frame> air;
  air.swap(gAir);
  for (const Frame& f : air) {
    pushInbound(gDev[f.to].s, f.isRequest, gDev[f.from].mac, f.role, f.code, f.seq, f.flags);
  }
}

static void run(uint32_t ms) {
  for (uint32_t t = 0; t < ms; t += 100) {
    gNow += 100;
    deliver();
    for (int i = 0; i < kDevices; ++i) tick(i);
  }
}

static size_t sentCount(int from, bool isRequest, int code = -1, size_t since = 0) {
  size_t n = 0;
  for (size_t i = since; i < gSentLog.size(); ++i) {
    const Frame& f = gSentLog[i];
    n += f.from == from && f.isRequest == isRequest && (code < 0 || f.code == code);
  }
  return n;
}
static size_t lineCount(int who, Line line) {
  size_t n = 0;
  for (Line l : gDev[who].lines) n += l == line;
  return n;
}

// Both run bondconnect: each asks, each agrees from the other's request, and
// then the air is quiet.
static void caseMutual() {
  resetWorld();
  bond(0, 1, kRoleMaster);
  bond(1, 0, kRoleWorker);
  run(2000);
  CHECK(gDev[0].s.agreement == Agreement::Agreed && gDev[1].s.agreement == Agreement::Agreed);
  CHECK(!gDev[0].s.roleConflict && !gDev[1].s.roleConflict);
  CHECK(sentCount(0, true) == 1 && sentCount(1, true) == 1);
  CHECK(sentCount(0, false, kReasonAccepted) == 1 && sentCount(1, false, kReasonAccepted) == 1);
  CHECK(lineCount(0, Line::Accepted) == 1 && lineCount(1, Line::Accepted) == 1);
  CHECK(gSentLog[0].isRequest && (gSentLog[0].code & kRequestFlagFresh) && gSentLog[0].seq != 0);
  const size_t sent = gSentLog.size();
  run(10 * 60000);
  CHECK(gSentLog.size() == sent);
  CHECK(gDev[0].lines.size() == 1 && gDev[1].lines.size() == 1);
}

// Only device 0 is bonded. Device 1 answers NOT_BONDED, tells its user who
// asked, and its own settings stay as they were. Device 0 stops and asks once
// a minute.
static void caseOneSided() {
  resetWorld();
  bond(0, 1, kRoleMaster);
  run(1000);
  CHECK(gDev[0].s.agreement == Agreement::Declined && gDev[0].s.reason == kReasonNotBonded);
  CHECK(gDev[0].declinedEdges == 1 && lineCount(0, Line::Declined) == 1);
  CHECK(!gDev[1].local.bonded && gDev[1].s.agreement == Agreement::Unknown);
  CHECK(lineCount(1, Line::WantsBond) == 1 && gDev[1].lineReasons[0] == kReasonNotBonded);
  run(60 * 60000);
  CHECK(sentCount(0, true) == 61);                                // the first and one a minute
  CHECK(sentCount(1, false, kReasonNotBonded) == 61);             // every one answered
  CHECK(gDev[0].declinedEdges == 1 && gDev[0].lines.size() == 1); // told once
  CHECK(lineCount(1, Line::WantsBond) == 5);                      // now and every 15 minutes
  CHECK(!gDev[1].local.bonded && gDev[1].s.agreement == Agreement::Unknown && gDev[1].declinedEdges == 0);
  CHECK(gDev[0].handshakeKicks == 0);

  // The other user accepts: bondconnect there. Both agree within the second.
  bond(1, 0, kRoleWorker);
  run(1000);
  CHECK(gDev[0].s.agreement == Agreement::Agreed && gDev[1].s.agreement == Agreement::Agreed);
  CHECK(lineCount(0, Line::Accepted) == 1 && lineCount(1, Line::Accepted) == 1);
}

// Device 1 is bonded to device 2. Device 0 is told BONDED_ELSEWHERE; the pair
// 1 and 2 is not disturbed.
static void caseBondedElsewhere() {
  resetWorld();
  bond(1, 2, kRoleMaster);
  bond(2, 1, kRoleWorker);
  run(1000);
  bond(0, 1, kRoleWorker);
  run(1000);
  CHECK(gDev[0].s.agreement == Agreement::Declined && gDev[0].s.reason == kReasonBondedElsewhere);
  CHECK(gDev[1].s.agreement == Agreement::Agreed && macIs(gDev[1].local.partner, gDev[2].mac));
  CHECK(lineCount(1, Line::WantsBond) == 1 && gDev[1].lineReasons.back() == kReasonBondedElsewhere);
  CHECK(gDev[2].s.agreement == Agreement::Agreed && gDev[2].lines.size() == 1);
}

// bondTellPartnerEnded and the settings change after it: `who` tells its
// partner ENDED, with the number of its last request, and its bond goes.
static void endBond(int who) {
  Device& d = gDev[who];
  const int partner = indexOf(d.local.partner);
  const uint16_t lastRequest = beginEnding(d.s, gNow);
  send(who, partner, Frame{false, who, partner, d.local.role, kReasonEnded, lastRequest});
  d.local.bonded = false;
  reset(d.s);
}

// Device 1 ends the bond (ENDED, then its settings go). Device 0 keeps its
// own setting, knows why, and agrees again when device 1 bonds again.
static void caseEndedAndBack() {
  resetWorld();
  bond(0, 1, kRoleMaster);
  bond(1, 0, kRoleWorker);
  run(1000);
  endBond(1);
  CHECK(gSentLog.back().code == kReasonEnded && gSentLog.back().seq == gDev[1].s.lastSeq && gDev[1].s.lastSeq != 0);
  run(1000);
  CHECK(gDev[0].s.agreement == Agreement::Declined && gDev[0].s.reason == kReasonEnded);
  CHECK(gDev[0].local.bonded && gDev[0].declinedEdges == 1 && gDev[0].lineReasons.back() == kReasonEnded);
  run(3 * 60000);
  // The answers to the requests that follow say NOT_BONDED: the same news.
  // "Ended" stays the reason and nothing more is printed.
  CHECK(sentCount(1, false, kReasonNotBonded) == 3);
  CHECK(gDev[0].s.reason == kReasonEnded && lineCount(0, Line::Declined) == 1 && gDev[0].declinedEdges == 1);
  bond(1, 0, kRoleWorker);
  run(1000);
  CHECK(gDev[0].s.agreement == Agreement::Agreed && gDev[1].s.agreement == Agreement::Agreed);
  CHECK(lineCount(0, Line::Accepted) == 2);
}

// A request goes through the send queue, ENDED is sent directly: a request
// device 1 made just before it ended the bond reaches device 0 after the
// ENDED. Read as a new request it would agree the bond again ("ended the
// bond", then "accepted the bond") and bring the heartbeat back until the next
// answer said NOT_BONDED. The ENDED names the last request made; device 0
// takes the requests up to it as void.
static void caseRequestOvertakenByEnded() {
  resetWorld();
  bond(0, 1, kRoleMaster);
  bond(1, 0, kRoleWorker);
  run(1000);
  CHECK(gDev[0].s.agreement == Agreement::Agreed && lineCount(0, Line::Accepted) == 1);
  // Device 1 asks once more (its session came up again); the request is
  // still in its send queue when the user ends the bond.
  CHECK(onSessionUp(gDev[1].s, gNow, false));
  tick(1);
  CHECK(gAir.size() == 1 && gAir[0].isRequest && gAir[0].from == 1);
  const Frame queued = gAir[0];
  gAir.clear();
  endBond(1);
  CHECK(gSentLog.back().seq == queued.seq);
  deliver();            // the ENDED first ...
  gAir.push_back(queued);
  const size_t sent = gSentLog.size();
  run(1000);            // ... then the request from before it
  CHECK(gDev[0].s.agreement == Agreement::Declined && gDev[0].s.reason == kReasonEnded);
  CHECK(gDev[0].s.requestsVoided == 1 && lineCount(0, Line::Accepted) == 1 && lineCount(0, Line::Declined) == 1);
  CHECK(sentCount(0, false, -1, sent) == 0);                     // the void request is not answered
  CHECK(gDev[0].declinedEdges == 1);
  // The bond comes back when device 1 bonds again: its new request is newer.
  run(2 * 60000);
  CHECK(gDev[0].s.agreement == Agreement::Declined && gDev[0].s.reason == kReasonEnded);
  bond(1, 0, kRoleWorker);
  run(1000);
  CHECK(gDev[0].s.agreement == Agreement::Agreed && gDev[1].s.agreement == Agreement::Agreed);
  CHECK(!gDev[0].s.partnerVoidSet && lineCount(0, Line::Accepted) == 2);

  // The rule by itself.
  State s = {};
  Local local = {};
  local.bonded = true;
  local.role = kRoleMaster;
  std::memcpy(local.partner, gDev[1].mac, 6);
  reset(s);
  s.lastSeq = 3;                                                  // our own numbers play no part
  pushInbound(s, false, gDev[1].mac, kRoleWorker, kReasonEnded, 40);
  Outcome out = takeInbound(s, local, 1000);
  CHECK(out.becameDeclined && !out.ignored && s.partnerVoidSet && s.partnerVoidSeq == 40);
  for (uint16_t old : {(uint16_t)40, (uint16_t)39, (uint16_t)1}) {
    pushInbound(s, true, gDev[1].mac, kRoleWorker, kRequestFlagFresh, old);
    out = takeInbound(s, local, 1100);
    CHECK(out.handled && out.voided && !out.sendNotice && out.line == Line::None && !out.becameDeclined);
    CHECK(s.agreement == Agreement::Declined && s.partnerVoidSet);
  }
  CHECK(s.requestsVoided == 3 && s.noticesSent == 0);
  // A newer request counts and ends the mark: one that is older than the
  // mark and arrives after it is then an ordinary repeat.
  pushInbound(s, true, gDev[1].mac, kRoleWorker, kRequestFlagFresh, 41);
  out = takeInbound(s, local, 1200);
  CHECK(!out.voided && out.sendNotice && out.noticeReason == kReasonAccepted && out.noticeSeq == 41);
  CHECK(s.agreement == Agreement::Agreed && !s.partnerVoidSet);

  // An ENDED that names no request (the sender never asked) voids nothing.
  reset(s);
  pushInbound(s, false, gDev[1].mac, kRoleWorker, kReasonEnded, 0);
  CHECK(takeInbound(s, local, 2000).becameDeclined && !s.partnerVoidSet);
  pushInbound(s, true, gDev[1].mac, kRoleWorker, 0, 1);
  CHECK(takeInbound(s, local, 2100).sendNotice && s.agreement == Agreement::Agreed);

  // Two ENDED out of order: the mark does not go back. An ENDED without a
  // number leaves it too.
  reset(s);
  pushInbound(s, false, gDev[1].mac, kRoleWorker, kReasonEnded, 50);
  takeInbound(s, local, 3000);
  pushInbound(s, false, gDev[1].mac, kRoleWorker, kReasonEnded, 45);
  takeInbound(s, local, 3100);
  pushInbound(s, false, gDev[1].mac, kRoleWorker, kReasonEnded, 0);
  takeInbound(s, local, 3200);
  CHECK(s.partnerVoidSet && s.partnerVoidSeq == 50);

  // The numbers go round at 16 bit: a request is older or newer by the
  // shorter way round (0 is never a request number).
  CHECK(seqNewer(1, 0xFFFF) && !seqNewer(0xFFFF, 1) && !seqNewer(7, 7) && seqNewer(8, 7) && !seqNewer(7, 8));
  CHECK(seqNewer(0x8006, 7) && !seqNewer(0x8007, 7) && !seqNewer(0x8008, 7));
  reset(s);
  pushInbound(s, false, gDev[1].mac, kRoleWorker, kReasonEnded, 0xFFFE);
  takeInbound(s, local, 4000);
  pushInbound(s, true, gDev[1].mac, kRoleWorker, 0, 0xFFFD);
  CHECK(takeInbound(s, local, 4100).voided);
  pushInbound(s, true, gDev[1].mac, kRoleWorker, 0, 1);            // 0xFFFF, then 1
  CHECK(takeInbound(s, local, 4200).sendNotice && !s.partnerVoidSet);

  // The mark is the partner's. A partner change forgets it, and so does a new
  // session: a partner that restarted counts from 1 again and would otherwise
  // be void until it passed the old number.
  reset(s);
  pushInbound(s, false, gDev[1].mac, kRoleWorker, kReasonEnded, 500);
  takeInbound(s, local, 5000);
  CHECK(s.partnerVoidSet);
  reset(s);
  CHECK(!s.partnerVoidSet);
  pushInbound(s, false, gDev[1].mac, kRoleWorker, kReasonEnded, 500);
  takeInbound(s, local, 5100);
  CHECK(!onSessionUp(s, 5200, false) && !s.partnerVoidSet);          // still declined, the mark gone
  pushInbound(s, true, gDev[1].mac, kRoleWorker, kRequestFlagFresh, 1);
  CHECK(takeInbound(s, local, 5300).sendNotice && s.agreement == Agreement::Agreed);
  // From a peer that is not the partner an ENDED sets nothing.
  reset(s);
  pushInbound(s, false, gDev[2].mac, kRoleWorker, kReasonEnded, 9);
  CHECK(takeInbound(s, local, 6000).ignored && !s.partnerVoidSet && s.agreement == Agreement::Unknown);

  // A waiting ENDED is not replaced by the notice that follows it: its mark
  // would be lost, and the request it voids would count after all.
  State q = {};
  reset(q);
  pushInbound(q, false, gDev[1].mac, kRoleWorker, kReasonEnded, 12);
  pushInbound(q, false, gDev[1].mac, kRoleWorker, kReasonNotBonded, 0);
  pushInbound(q, true, gDev[1].mac, kRoleWorker, 0, 12);
  CHECK(takeInbound(q, local, 7000).becameDeclined && q.partnerVoidSet);
  out = takeInbound(q, local, 7000);
  CHECK(out.handled && !out.becameDeclined && q.reason == kReasonEnded);   // NOT_BONDED after ENDED: the same news
  CHECK(takeInbound(q, local, 7000).voided && !takeInbound(q, local, 7000).handled);
  // A second ENDED, or a second other notice, still replaces its like.
  q = State();
  pushInbound(q, false, gDev[1].mac, kRoleWorker, kReasonEnded, 12);
  pushInbound(q, false, gDev[1].mac, kRoleWorker, kReasonEnded, 13);
  pushInbound(q, false, gDev[1].mac, kRoleWorker, kReasonNotBonded, 0);
  pushInbound(q, false, gDev[1].mac, kRoleWorker, kReasonBondedElsewhere, 0);
  size_t waiting = 0;
  for (const auto& e : q.inbox) waiting += e.inUse;
  CHECK(waiting == 2);
}

// The device that ends a bond makes no request between its ENDED and the
// settings change: that request would carry a number the ENDED does not void.
static void caseNoRequestWhileEnding() {
  State s = {};
  Local local = {};
  local.bonded = true;
  local.role = kRoleMaster;
  std::memcpy(local.partner, gDev[1].mac, 6);
  reset(s);
  CHECK(beginEnding(s, 500) == 0);                                // never asked: the ENDED names no request
  reset(s);
  CHECK(planRequest(s, local, 1000, true).send);
  requestResult(s, 1000, true);
  pushInbound(s, false, gDev[1].mac, kRoleWorker, kReasonNotBonded, s.lastSeq);
  CHECK(takeInbound(s, local, 1100).becameDeclined);
  // A request is due (the declined partner's heartbeat set it off) when the
  // user ends the bond.
  CHECK(!onPartnerHeartbeat(s, kRoleWorker, kRoleMaster, 7000).proceed && s.requestDue && hasWork(s, 7000));
  CHECK(beginEnding(s, 7000) == 1);
  CHECK(!hasWork(s, 7000) && !planRequest(s, local, 7000, true).send && s.lastSeq == 1);
  CHECK(!onPartnerHeartbeat(s, kRoleWorker, kRoleMaster, 7100).proceed);
  RequestPlan plan = planRequest(s, local, 7100, false);
  CHECK(!plan.send && !plan.kickHandshake);
  // The settings change follows: the next partner is asked at once.
  std::memcpy(local.partner, gDev[2].mac, 6);
  reset(s);
  plan = planRequest(s, local, 7200, true);
  CHECK(plan.send && plan.seq == 2);
  requestResult(s, 7200, true);
  // A change that never came: asking goes on after kEndingQuietMs.
  CHECK(beginEnding(s, 10000) == 2);
  CHECK(onSessionUp(s, 10100, false) && s.requestDue);            // a request is due at once
  CHECK(!hasWork(s, 10000 + kEndingQuietMs - 1) && !planRequest(s, local, 10000 + kEndingQuietMs - 1, true).send);
  CHECK(hasWork(s, 10000 + kEndingQuietMs));
  plan = planRequest(s, local, 10000 + kEndingQuietMs, true);
  CHECK(plan.send && plan.seq == 3 && s.endingSinceMs == 0);
  requestResult(s, 10000 + kEndingQuietMs, true);

  // The change never came and the bond stayed agreed for a month: nothing
  // looked at the time of the ENDED meanwhile, and it must not hold the next
  // request back when the partner goes silent and its session returns.
  const uint32_t kDay = 24UL * 3600UL * 1000UL;
  pushInbound(s, false, gDev[2].mac, kRoleWorker, kReasonAccepted, s.lastSeq);
  CHECK(takeInbound(s, local, 16000).line == Line::Accepted);
  CHECK(beginEnding(s, 20000) == 3);
  for (uint32_t days : {25u, 30u, 49u}) {
    const uint32_t later = 20000 + days * kDay;
    onLinkLost(s);
    CHECK(onSessionUp(s, later, false) && hasWork(s, later));
    plan = planRequest(s, local, later, true);
    CHECK(plan.send);
    requestResult(s, later, true);
    pushInbound(s, false, gDev[2].mac, kRoleWorker, kReasonAccepted, s.lastSeq);
    CHECK(takeInbound(s, local, later + 100).handled && s.agreement == Agreement::Agreed);
  }
}

// Both ends have the same role: each answers the other ROLE_CONFLICT, both
// know and say it once, and the bond counts as agreed.
static void caseRoleConflict() {
  resetWorld();
  bond(0, 1, kRoleMaster);
  bond(1, 0, kRoleMaster);
  run(1000);
  for (int i = 0; i < 2; ++i) {
    CHECK(gDev[i].s.agreement == Agreement::Agreed && gDev[i].s.roleConflict);
    CHECK(lineCount(i, Line::RoleConflict) == 1 && lineCount(i, Line::Accepted) == 0);
    CHECK(sentCount(i, false, kReasonRoleConflict) == 1 && sentCount(i, false, kReasonAccepted) == 0);
  }
  // Device 1 becomes worker; its heartbeat tells device 0, and device 0's
  // tells device 1.
  gDev[1].local.role = kRoleWorker;
  Beat b = onPartnerHeartbeat(gDev[0].s, kRoleWorker, gDev[0].local.role, gNow);
  CHECK(b.proceed && b.line == Line::RoleConflictOver && !gDev[0].s.roleConflict);
  b = onPartnerHeartbeat(gDev[1].s, kRoleMaster, gDev[1].local.role, gNow);
  CHECK(b.proceed && b.line == Line::RoleConflictOver && !gDev[1].s.roleConflict);
  // A conflict that appears later needs three heartbeats in a row.
  CHECK(onPartnerHeartbeat(gDev[0].s, kRoleMaster, kRoleMaster, gNow).line == Line::None);
  CHECK(onPartnerHeartbeat(gDev[0].s, kRoleMaster, kRoleMaster, gNow).line == Line::None);
  CHECK(onPartnerHeartbeat(gDev[0].s, kRoleMaster, kRoleMaster, gNow).line == Line::RoleConflict);
  CHECK(onPartnerHeartbeat(gDev[0].s, kRoleMaster, kRoleMaster, gNow).line == Line::None);
  CHECK(gDev[0].s.roleConflict);
}

// What must not count: a notice from a peer that is not the partner, an
// answer to an older request, a reason from newer firmware.
static void caseNoticesThatDoNotCount() {
  resetWorld();
  bond(0, 1, kRoleMaster);
  bond(1, 0, kRoleWorker);
  run(1000);
  Device& d = gDev[0];
  for (uint8_t reason : {kReasonEnded, kReasonNotBonded, kReasonBondedElsewhere, kReasonAccepted}) {
    pushInbound(d.s, false, gDev[2].mac, kRoleWorker, reason, 0);
    const Outcome out = takeInbound(d.s, d.local, gNow);
    CHECK(out.handled && out.ignored && !out.sendNotice && !out.becameDeclined && out.line == Line::None);
  }
  CHECK(d.s.agreement == Agreement::Agreed && d.s.noticesIgnored == 4);
  pushInbound(d.s, false, gDev[1].mac, kRoleWorker, kReasonNotBonded, (uint16_t)(d.s.lastSeq + 1));
  CHECK(takeInbound(d.s, d.local, gNow).ignored && d.s.agreement == Agreement::Agreed);
  pushInbound(d.s, false, gDev[1].mac, kRoleWorker, 200, 0);
  CHECK(takeInbound(d.s, d.local, gNow).ignored && d.s.agreement == Agreement::Agreed);
  // Unasked (number 0) and the answer to the last request both count. ENDED
  // is never an answer and counts with any number (caseRequestOvertakenByEnded).
  pushInbound(d.s, false, gDev[1].mac, kRoleWorker, kReasonEnded, (uint16_t)(d.s.lastSeq + 9));
  CHECK(takeInbound(d.s, d.local, gNow).becameDeclined);
  pushInbound(d.s, false, gDev[1].mac, kRoleWorker, kReasonAccepted, d.s.lastSeq);
  CHECK(takeInbound(d.s, d.local, gNow).line == Line::Accepted && d.s.agreement == Agreement::Agreed);
  // With no bond here nobody is the partner.
  Device& idle = gDev[2];
  pushInbound(idle.s, false, gDev[0].mac, kRoleMaster, kReasonAccepted, 0);
  CHECK(takeInbound(idle.s, idle.local, gNow).ignored && idle.s.agreement == Agreement::Unknown);
  // A partner change: the answer to a request made before it matches nothing.
  const uint16_t before = d.s.lastSeq;
  bond(0, 2, kRoleMaster);
  CHECK(d.s.agreement == Agreement::Unknown && d.s.lastSeq == before);
  pushInbound(d.s, false, gDev[1].mac, kRoleWorker, kReasonAccepted, before);
  CHECK(takeInbound(d.s, d.local, gNow).ignored && d.s.agreement == Agreement::Unknown);
}

// Lost frames: a lost request or a lost answer is made good by the repeat a
// minute later.
static void caseLostFrames() {
  resetWorld();
  int lost = 0;
  gLose = [&](const Frame& f) { return f.isRequest && lost++ < 2; };   // both first requests
  bond(0, 1, kRoleMaster);
  bond(1, 0, kRoleWorker);
  run(30000);
  CHECK(gDev[0].s.agreement == Agreement::Unknown && gDev[1].s.agreement == Agreement::Unknown);
  run(31000);
  CHECK(gDev[0].s.agreement == Agreement::Agreed && gDev[1].s.agreement == Agreement::Agreed);

  resetWorld();
  lost = 0;
  gLose = [&](const Frame& f) { return !f.isRequest && lost++ < 1; };  // the first NOT_BONDED
  bond(0, 1, kRoleMaster);
  run(30000);
  CHECK(gDev[0].s.agreement == Agreement::Unknown);
  run(31000);
  CHECK(gDev[0].s.agreement == Agreement::Declined && sentCount(0, true) == 2);
}

// The inbox keeps arrival order, one request and one notice per peer (the
// newer replaces the older), and drops what does not fit.
static void caseInbox() {
  resetWorld();
  State s = {};
  Local local = {};
  CHECK(pushInbound(s, true, gDev[0].mac, 0, 0, 1));
  CHECK(pushInbound(s, false, gDev[0].mac, 0, kReasonEnded, 0));
  CHECK(pushInbound(s, true, gDev[1].mac, 0, 0, 2));
  CHECK(pushInbound(s, true, gDev[2].mac, 0, 0, 3));
  const uint8_t other[6] = {9, 9, 9, 9, 9, 9};
  CHECK(!pushInbound(s, true, other, 0, 0, 4) && s.inboundDropped == 1);
  CHECK(pushInbound(s, true, gDev[1].mac, 0, 0, 22));                 // replaces number 2, moves to the end
  CHECK(hasWork(s, gNow));
  Outcome out = takeInbound(s, local, gNow);
  CHECK(out.handled && out.sendNotice && out.noticeSeq == 1 && macIs(out.mac, gDev[0].mac));
  out = takeInbound(s, local, gNow);
  CHECK(out.handled && out.ignored && macIs(out.mac, gDev[0].mac));   // the notice: no bond here
  out = takeInbound(s, local, gNow);
  CHECK(out.noticeSeq == 3 && macIs(out.mac, gDev[2].mac));
  out = takeInbound(s, local, gNow);
  CHECK(out.noticeSeq == 22 && macIs(out.mac, gDev[1].mac));
  CHECK(!takeInbound(s, local, gNow).handled);
}

// A peer whose bond frames are ignored: one notice a minute each, and a peer
// that became the partner is owed nothing.
static void casePeerLimits() {
  resetWorld();
  State s = {};
  Local local = {};
  uint8_t to[6];
  uint8_t reason = 0;
  uint16_t seq = 99;
  CHECK(noteIgnoredFrame(s, gDev[1].mac, gNow));
  CHECK(!noteIgnoredFrame(s, gDev[1].mac, gNow + 10));                // already due
  CHECK(hasWork(s, gNow));
  CHECK(takeOwedNotice(s, local, gNow, to, &reason, &seq) && reason == kReasonNotBonded && seq == 0 &&
        macIs(to, gDev[1].mac));
  CHECK(!takeOwedNotice(s, local, gNow, to, &reason, &seq));
  CHECK(!noteIgnoredFrame(s, gDev[1].mac, gNow + 59999));
  CHECK(noteIgnoredFrame(s, gDev[1].mac, gNow + 60000));
  CHECK(noteIgnoredFrame(s, gDev[2].mac, gNow + 60000));              // its own limit
  local.bonded = true;
  std::memcpy(local.partner, gDev[2].mac, 6);
  CHECK(takeOwedNotice(s, local, gNow + 60000, to, &reason, &seq) && reason == kReasonBondedElsewhere &&
        macIs(to, gDev[1].mac));
  CHECK(!takeOwedNotice(s, local, gNow + 60000, to, &reason, &seq));  // device 2 is the partner now
}

// One tick of a device that is nobody's partner; counts what it sends and prints.
struct Counts { size_t notices = 0, lines = 0; };
static void tickCounting(State& s, const Local& local, uint32_t now, Counts& c) {
  if (!hasWork(s, now)) return;
  for (;;) {
    const Outcome out = takeInbound(s, local, now);
    if (!out.handled) break;
    c.notices += out.sendNotice;
    c.lines += out.line == Line::WantsBond;
  }
  uint8_t to[6];
  uint8_t reason = 0;
  uint16_t seq = 0;
  while (takeOwedNotice(s, local, now, to, &reason, &seq)) c.notices++;
}

// More peers than the table has entries. Taking turns must not start
// anybody's limits over: the peers that do not fit share the last entry and
// its limits, and an entry is given to another peer only once its limits have
// run out.
static void caseMorePeersThanEntries() {
  // Six peers whose bond heartbeats (one every 5 s each, staggered) are
  // ignored for ten minutes. With limits of their own they would get 10
  // notices each; four have them, the other two share the fifth entry.
  State s = {};
  Local local = {};
  Counts c;
  uint8_t peers[6][6];
  for (uint8_t i = 0; i < 6; ++i) {
    const uint8_t mac[6] = {7, 7, 7, 7, 7, i};
    std::memcpy(peers[i], mac, 6);
  }
  for (uint32_t t = 1000; t < 1000 + 10 * 60000; t += 100) {
    for (uint32_t i = 0; i < 6; ++i) {
      if (t % 5000 == i * 500) noteIgnoredFrame(s, peers[i], t);
    }
    tickCounting(s, local, t, c);
  }
  CHECK(c.notices >= 40 && c.notices <= 51);   // 5 entries, one notice a minute each

  // Six peers that each repeat their request once a minute for an hour. The
  // "wants to bond" line is allowed once per 15 minutes per entry.
  s = State();
  c = Counts();
  for (uint32_t t = 1000; t < 1000 + 60 * 60000; t += 100) {
    for (uint32_t i = 0; i < 6; ++i) {
      if (t % 60000 == i * 1000) pushInbound(s, true, peers[i], kRoleMaster, 0, (uint16_t)(1 + t / 60000));
    }
    tickCounting(s, local, t, c);
  }
  CHECK(c.lines == 5 * 4);                     // not 6 * 60
  CHECK(c.notices <= 6 * 60);

  // Ten peers that each ask every two minutes. An entry is free for another
  // peer only when the 15 minutes of its line are over too, not already when
  // its minute between notices is: otherwise every request would find a new
  // entry and print its line.
  s = State();
  c = Counts();
  for (uint32_t t = 1000; t < 1000 + 60 * 60000; t += 100) {
    for (uint32_t i = 0; i < 10; ++i) {
      const uint8_t mac[6] = {7, 7, 7, 7, 8, (uint8_t)i};
      if (t % 120000 == i * 1000) pushInbound(s, true, mac, kRoleMaster, 0, (uint16_t)(1 + t / 60000));
    }
    tickCounting(s, local, t, c);
  }
  CHECK(c.lines == 5 * 4);

  // An entry goes to another peer only when its limits have run out, and the
  // first four peers keep theirs meanwhile.
  s = State();
  for (uint8_t i = 0; i < 5; ++i) noteIgnoredFrame(s, peers[i], 1000 + i * 100u);
  uint8_t to[6];
  uint8_t reason = 0;
  uint16_t seq = 0;
  size_t owed = 0;
  while (takeOwedNotice(s, local, 2000, to, &reason, &seq)) owed++;
  CHECK(owed == 5);
  CHECK(!noteIgnoredFrame(s, peers[5], 3000));                        // shares the fifth entry: told a second ago
  CHECK(peerFind(s, peers[0]) && peerFind(s, peers[3]) && !peerFind(s, peers[4]) && peerFind(s, peers[5]));
  CHECK(!noteIgnoredFrame(s, peers[4], 3100) && !peerFind(s, peers[5]));
  CHECK(!noteIgnoredFrame(s, peers[0], 3200));                        // its own limit, untouched by the others
  CHECK(noteIgnoredFrame(s, peers[5], 2000 + kNoticeGapMs));          // the minute is over: an entry is free again
  CHECK(takeOwedNotice(s, local, 2000 + kNoticeGapMs, to, &reason, &seq) && macIs(to, peers[5]));

  // An answer held back for the peer in the shared entry is not sent to the
  // peer that takes the entry over: the number in it is the first one's.
  s = State();
  for (uint8_t i = 0; i < 5; ++i) {
    pushInbound(s, true, peers[i], kRoleMaster, 0, (uint16_t)(10 + i));
    CHECK(takeInbound(s, local, 1000).sendNotice);
  }
  pushInbound(s, true, peers[4], kRoleMaster, 0, 99);
  CHECK(!takeInbound(s, local, 1500).sendNotice);                     // held until 5 s after its last answer
  CHECK(!noteIgnoredFrame(s, peers[5], 2000) && peerFind(s, peers[5]) && !peerFind(s, peers[4]));
  CHECK(!takeOwedNotice(s, local, 1000 + kAnswerGapMs, to, &reason, &seq));
}

// A peer that is not the partner and asks without pause is answered once per
// kAnswerGapMs, with the number of its newest request; the partner's requests
// are always answered.
static void caseRequestFlood() {
  resetWorld();
  State s = {};
  Local local = {};
  Counts c;
  for (uint32_t t = 1000; t < 1000 + 10000; t += 10) {                // 1000 requests in 10 s
    pushInbound(s, true, gDev[1].mac, kRoleMaster, kRequestFlagFresh, (uint16_t)(t / 10));
    tickCounting(s, local, t, c);
  }
  CHECK(c.notices == 2 && c.lines == 1);                              // at 0 s and at 5 s
  CHECK(s.noticesSent == 2);

  // Two requests in a row (a bond command given twice): the first is answered
  // at once, the second when the gap is over, with its own number.
  s = State();
  (void)planRequest(s, local, 1000, false);                           // no bond here: nobody to ask
  pushInbound(s, true, gDev[1].mac, kRoleMaster, kRequestFlagFresh, 7);
  Outcome out = takeInbound(s, local, 1000);
  CHECK(out.sendNotice && out.noticeSeq == 7 && out.noticeReason == kReasonNotBonded);
  pushInbound(s, true, gDev[1].mac, kRoleMaster, kRequestFlagFresh, 8);
  out = takeInbound(s, local, 1500);
  CHECK(out.handled && !out.sendNotice);
  uint8_t to[6];
  uint8_t reason = 0;
  uint16_t seq = 0;
  CHECK(!hasWork(s, 1000 + kAnswerGapMs - 1) && !takeOwedNotice(s, local, 1000 + kAnswerGapMs - 1, to, &reason, &seq));
  CHECK(hasWork(s, 1000 + kAnswerGapMs));
  CHECK(takeOwedNotice(s, local, 1000 + kAnswerGapMs, to, &reason, &seq) && seq == 8 &&
        reason == kReasonNotBonded && macIs(to, gDev[1].mac));
  CHECK(!takeOwedNotice(s, local, 1000 + kAnswerGapMs, to, &reason, &seq));

  // The peer becomes the partner before the held answer is due: it is not
  // sent (the peer is asked, and its own next request is answered ACCEPTED).
  s = State();
  pushInbound(s, true, gDev[1].mac, kRoleMaster, 0, 1);
  takeInbound(s, local, 1000);
  pushInbound(s, true, gDev[1].mac, kRoleMaster, 0, 2);
  takeInbound(s, local, 1100);
  local.bonded = true;
  local.role = kRoleWorker;
  std::memcpy(local.partner, gDev[1].mac, 6);
  CHECK(!takeOwedNotice(s, local, 1000 + kAnswerGapMs, to, &reason, &seq));

  // The partner is answered every time.
  size_t answered = 0;
  for (uint32_t t = 20000; t < 21000; t += 10) {
    pushInbound(s, true, gDev[1].mac, kRoleMaster, 0, (uint16_t)t);
    answered += takeInbound(s, local, t).sendNotice;
  }
  CHECK(answered == 100);
}

// When a request goes out.
static void caseRequestTiming() {
  // Not bonded: never.
  resetWorld();
  State s = {};
  Local local = {};
  RequestPlan plan = planRequest(s, local, gNow, true);
  CHECK(!plan.send && !plan.kickHandshake && !hasWork(s, gNow + 1000));

  // Bonded, no session: waits; with the session: at once, marked fresh.
  local.bonded = true;
  local.role = kRoleMaster;
  std::memcpy(local.partner, gDev[1].mac, 6);
  reset(s);
  plan = planRequest(s, local, gNow, false);
  CHECK(!plan.send && !plan.kickHandshake && !hasWork(s, gNow + 500) && hasWork(s, gNow + 1000));
  CHECK(onSessionUp(s, gNow + 100, false) && hasWork(s, gNow + 100));
  plan = planRequest(s, local, gNow + 100, true);
  CHECK(plan.send && plan.seq == 1 && (plan.flags & kRequestFlagFresh));
  CHECK(!plan.watchAck);                                               // the first request on a session

  // The send queue was full: again after kRequestRetryMs, still fresh, with
  // the next number.
  requestResult(s, gNow + 100, false);
  CHECK(!planRequest(s, local, gNow + 4000, true).send);
  plan = planRequest(s, local, gNow + 100 + kRequestRetryMs, true);
  CHECK(plan.send && plan.seq == 2 && (plan.flags & kRequestFlagFresh));
  requestResult(s, gNow + 6000, true);
  CHECK(s.requestsSent == 1 && !hasWork(s, gNow + 6000 + kRequestRepeatMs - 1));

  // Unanswered: once a minute, not fresh.
  plan = planRequest(s, local, gNow + 6000 + kRequestRepeatMs, true);
  CHECK(plan.send && plan.seq == 3 && plan.flags == 0 && plan.watchAck);
  requestResult(s, gNow + 66000, true);

  // Agreed: no repeat; a new session asks once more.
  pushInbound(s, false, gDev[1].mac, kRoleWorker, kReasonAccepted, 3);
  CHECK(takeInbound(s, local, gNow + 66100).line == Line::Accepted);
  CHECK(!hasWork(s, gNow + 66100 + 10 * kRequestRepeatMs));
  CHECK(onSessionUp(s, gNow + 700000, false));
  CHECK(planRequest(s, local, gNow + 700000, true).send);
  requestResult(s, gNow + 700000, true);

  // Declined: the session does not make the partner online; without a
  // session the once-a-minute turn asks for a handshake start instead.
  pushInbound(s, false, gDev[1].mac, kRoleWorker, kReasonNotBonded, 0);
  CHECK(takeInbound(s, local, gNow + 700100).becameDeclined);
  CHECK(!onSessionUp(s, gNow + 700200, false));                             // asked 100 ms ago: not again yet
  CHECK(!planRequest(s, local, gNow + 700200, true).send);
  plan = planRequest(s, local, gNow + 700000 + kRequestRetryMs, true);
  CHECK(plan.send && (plan.flags & kRequestFlagFresh) && plan.watchAck);   // declined: every request is watched
  requestResult(s, gNow + 705000, true);
  plan = planRequest(s, local, gNow + 705000 + kRequestRepeatMs, false);
  CHECK(!plan.send && plan.kickHandshake);
  plan = planRequest(s, local, gNow + 705000 + kRequestRepeatMs + 100, false);
  CHECK(!plan.send && !plan.kickHandshake);                            // one start a minute
  onLinkLost(s);
  CHECK(s.agreement == Agreement::Declined);                           // silence is not a new answer

  // The clock wraps: the minute still takes a minute.
  resetWorld(0xFFFFFFFFu - 30000u);
  bond(0, 1, kRoleMaster);
  run(1000);
  CHECK(gDev[0].s.agreement == Agreement::Declined);
  const size_t sent = sentCount(0, true);
  run(5 * 60000);
  CHECK(gNow < 0x80000000u && sentCount(0, true) == sent + 5);
}

// The side that answered the handshake has its session up before the other
// side can read what it sends: its request waits kSessionSettleMs.
static void caseSessionSettle() {
  State s = {};
  Local local = {};
  local.bonded = true;
  local.role = kRoleWorker;
  std::memcpy(local.partner, gDev[1].mac, 6);
  reset(s);
  CHECK(onSessionUp(s, 5000, true));
  CHECK(!hasWork(s, 5000) && !planRequest(s, local, 5000, true).send);
  CHECK(!hasWork(s, 5000 + kSessionSettleMs - 1) && hasWork(s, 5000 + kSessionSettleMs));
  RequestPlan plan = planRequest(s, local, 5000 + kSessionSettleMs, true);
  CHECK(plan.send && (plan.flags & kRequestFlagFresh) && !plan.watchAck);
  requestResult(s, 5000 + kSessionSettleMs, true);

  // The side that started the handshake asks at once.
  State t = {};
  reset(t);
  CHECK(onSessionUp(t, 5000, false) && planRequest(t, local, 5000, true).send);

  // Declined: the 5 s since the last request and the settle time both hold.
  pushInbound(s, false, gDev[1].mac, kRoleMaster, kReasonNotBonded, s.lastSeq);
  CHECK(takeInbound(s, local, 6100).becameDeclined);
  CHECK(!onSessionUp(s, 6200, true));                                  // asked 200 ms ago
  CHECK(!planRequest(s, local, 6200 + kSessionSettleMs, true).send);
  CHECK(planRequest(s, local, 6000 + kRequestRetryMs, true).send);
  requestResult(s, 6000 + kRequestRetryMs, true);
  CHECK(!onSessionUp(s, 60000, true));                                 // long after the last request
  CHECK(!planRequest(s, local, 60000 + kSessionSettleMs - 1, true).send);
  CHECK(planRequest(s, local, 60000 + kSessionSettleMs, true).send);
}

// A partner that declined, the session gone, and a request still waiting to
// be sent (the partner's late heartbeat or a full send queue set it off). The
// once-a-minute handshake start must go on: nothing else brings the session
// back, because a declined partner gets no heartbeat.
static void caseDeclinedSessionLost() {
  for (int how = 0; how < 2; ++how) {
    State s = {};
    Local local = {};
    local.bonded = true;
    local.role = kRoleMaster;
    std::memcpy(local.partner, gDev[1].mac, 6);
    reset(s);
    uint32_t now = 1000;
    CHECK(planRequest(s, local, now, true).send);
    requestResult(s, now, true);
    pushInbound(s, false, gDev[1].mac, kRoleWorker, kReasonNotBonded, s.lastSeq);
    CHECK(takeInbound(s, local, now + 100).becameDeclined);
    if (how == 0) {
      CHECK(!onPartnerHeartbeat(s, kRoleWorker, kRoleMaster, now + 200).proceed);   // sets a request off
    } else {
      now += kRequestRepeatMs;
      CHECK(planRequest(s, local, now, true).send);
      requestResult(s, now, false);                                                 // the send queue was full
    }
    CHECK(s.requestDue);
    // The session is lost before that request could go out. Ten minutes.
    int kicks = 0, sends = 0;
    for (uint32_t t = 0; t < 10 * 60000; t += 100) {
      now += 100;
      if (!hasWork(s, now)) continue;
      const RequestPlan plan = planRequest(s, local, now, false);
      kicks += plan.kickHandshake;
      sends += plan.send;
    }
    CHECK(sends == 0 && kicks >= 9 && kicks <= 10);
    // The session comes back: asked on it.
    CHECK(!onSessionUp(s, now, false));
    CHECK(planRequest(s, local, now + kRequestRetryMs, true).send);
  }
}

// An agreed bond sends nothing for weeks. A deadline left standing from the
// last request would read as "still ahead" once it is more than 24.8 days old
// and stop the once-a-minute request after a decline.
static void caseDeclineAfterWeeks() {
  const uint32_t kDay = 24UL * 3600UL * 1000UL;
  for (int how = 0; how < 3; ++how) {
    State s = {};
    Local local = {};
    local.bonded = true;
    local.role = kRoleMaster;
    std::memcpy(local.partner, gDev[1].mac, 6);
    reset(s);
    uint32_t now = 1000;
    CHECK(planRequest(s, local, now, true).send);
    requestResult(s, now, true);
    if (how == 0) {            // agreed by the partner's answer
      pushInbound(s, false, gDev[1].mac, kRoleWorker, kReasonAccepted, s.lastSeq);
      CHECK(takeInbound(s, local, now + 100).line == Line::Accepted);
    } else if (how == 1) {     // ... by its own request
      pushInbound(s, true, gDev[1].mac, kRoleWorker, kRequestFlagFresh, 5);
      CHECK(takeInbound(s, local, now + 100).sendNotice);
    } else {                   // ... by its heartbeat (firmware without these frames)
      CHECK(onPartnerHeartbeat(s, kRoleWorker, kRoleMaster, now + 100).proceed);
    }
    CHECK(s.agreement == Agreement::Agreed && s.nextRepeatMs == 0);
    now += 30 * kDay;
    CHECK(!hasWork(s, now));
    pushInbound(s, false, gDev[1].mac, kRoleWorker, kReasonEnded, 0);
    CHECK(takeInbound(s, local, now).becameDeclined);
    CHECK(!hasWork(s, now + kRequestRepeatMs - 1) && hasWork(s, now + kRequestRepeatMs));
    const RequestPlan plan = planRequest(s, local, now + kRequestRepeatMs, true);
    CHECK(plan.send && plan.flags == 0 && plan.watchAck);
  }

  // The same without ever agreeing: a request was sent, then one more became
  // due as the session went away, and for 30 days there was no session to send
  // it on (a due request holds the once-a-minute turn back, so the deadline of
  // the first request is left standing). The session returns and the partner's
  // notice arrives before the request has gone out.
  State s = {};
  Local local = {};
  local.bonded = true;
  std::memcpy(local.partner, gDev[1].mac, 6);
  reset(s);
  CHECK(planRequest(s, local, 1000, true).send);
  requestResult(s, 1000, true);
  CHECK(onSessionUp(s, 1500, true));
  uint32_t now = 1500;
  int sends = 0;
  for (uint32_t t = 0; t < 30 * kDay; t += 1000) {
    now += 1000;
    if (hasWork(s, now)) sends += planRequest(s, local, now, false).send;
  }
  CHECK(sends == 0 && onSessionUp(s, now, true));
  pushInbound(s, false, gDev[1].mac, kRoleWorker, kReasonBondedElsewhere, 0);
  CHECK(takeInbound(s, local, now + 200).becameDeclined);
  CHECK(hasWork(s, now + 200 + kRequestRepeatMs) &&
        planRequest(s, local, now + 200 + kRequestRepeatMs, true).send);
}

// A partner without these frames never answers; its bond heartbeat (sent only
// to its own partner) stands for agreement. From a partner that declined, a
// heartbeat is not acted on and brings a request, at most every 5 s.
static void caseHeartbeat() {
  resetWorld();
  State s = {};
  Local local = {};
  local.bonded = true;
  local.role = kRoleMaster;
  std::memcpy(local.partner, gDev[1].mac, 6);
  reset(s);
  Beat b = onPartnerHeartbeat(s, kRoleWorker, kRoleMaster, gNow);
  CHECK(b.proceed && b.line == Line::Accepted && s.agreement == Agreement::Agreed);
  b = onPartnerHeartbeat(s, kRoleWorker, kRoleMaster, gNow + 5000);
  CHECK(b.proceed && b.line == Line::None);
  onLinkLost(s);
  CHECK(s.agreement == Agreement::Unknown);
  b = onPartnerHeartbeat(s, kRoleWorker, kRoleMaster, gNow + 60000);
  CHECK(b.proceed && b.line == Line::None && s.agreement == Agreement::Agreed);   // no second line

  pushInbound(s, false, gDev[1].mac, kRoleWorker, kReasonEnded, 0);
  CHECK(takeInbound(s, local, gNow + 61000).becameDeclined);
  s.lastRequestMs = gNow + 61000;
  b = onPartnerHeartbeat(s, kRoleWorker, kRoleMaster, gNow + 62000);
  CHECK(!b.proceed && b.line == Line::None && s.agreement == Agreement::Declined);
  CHECK(!planRequest(s, local, gNow + 62000, true).send);              // 1 s after the last request
  CHECK(planRequest(s, local, gNow + 61000 + kRequestRetryMs, true).send);
}

static void caseTexts() {
  CHECK(std::string(reasonPhrase(kReasonEnded)) == "ended the bond");
  CHECK(std::string(reasonPhrase(kReasonNotBonded)) == "has bond mode off");
  CHECK(std::string(reasonPhrase(kReasonBondedElsewhere)) == "is bonded to another device");
  CHECK(std::string(reasonKey(kReasonRoleConflict)) == "role_conflict" && std::string(reasonKey(77)).empty());
  CHECK(std::string(agreementKey(Agreement::Unknown)) == "unknown" &&
        std::string(agreementKey(Agreement::Agreed)) == "agreed" &&
        std::string(agreementKey(Agreement::Declined)) == "declined");
  // The numbers are the wire format.
  CHECK(kReasonAccepted == 1 && kReasonNotBonded == 2 && kReasonBondedElsewhere == 3 && kReasonEnded == 4 &&
        kReasonRoleConflict == 5 && kRequestFlagFresh == 1);
}


// ---- The role ------------------------------------------------------------------------

// bondconnect on `who`: the role roleForBond gives it, from what it knows of
// `partner`, then the reset of a partner change.
static RoleChoice bondChosenOrAuto(int who, int partner, bool chosen, uint8_t role) {
  Device& d = gDev[who];
  uint8_t peerRole = 0;
  bool peerChose = false;
  const bool known = roleNoteOf(d.s, d.local, gDev[partner].mac, &peerRole, &peerChose);
  const RoleChoice c =
      roleForBond(chosen, role, known, peerChose, peerRole, std::memcmp(d.mac, gDev[partner].mac, 6) > 0);
  d.local.roleChosen = chosen;
  bond(who, partner, c.role);
  return c;
}

static void caseRoleRules() {
  // roleForBond: chosen, else the other than a partner that chose, else by address.
  CHECK(roleForBond(true, kRoleWorker, true, true, kRoleWorker, true).role == kRoleWorker &&
        roleForBond(true, kRoleWorker, true, true, kRoleWorker, true).source == RoleSource::Chosen);
  CHECK(roleForBond(false, kRoleWorker, true, true, kRoleWorker, false).role == kRoleMaster &&
        roleForBond(false, kRoleWorker, true, true, kRoleWorker, false).source == RoleSource::Partner);
  CHECK(roleForBond(false, kRoleMaster, true, true, kRoleMaster, true).role == kRoleWorker);
  CHECK(roleForBond(false, kRoleWorker, true, false, kRoleWorker, true).role == kRoleMaster &&
        roleForBond(false, kRoleWorker, true, false, kRoleWorker, true).source == RoleSource::Address);
  CHECK(roleForBond(false, kRoleMaster, false, true, kRoleMaster, false).role == kRoleWorker &&
        roleForBond(false, kRoleMaster, false, true, kRoleMaster, false).source == RoleSource::Address);
  CHECK(roleForBond(false, kRoleWorker, true, true, 7, true).source == RoleSource::Address);  // a role it does not know
  // roleYields: only an automatic role, only to a partner that chose the same.
  CHECK(roleYields(false, kRoleMaster, true, kRoleMaster) && roleYields(false, kRoleWorker, true, kRoleWorker));
  CHECK(!roleYields(true, kRoleMaster, true, kRoleMaster) && !roleYields(false, kRoleMaster, false, kRoleMaster) &&
        !roleYields(false, kRoleMaster, true, kRoleWorker) && !roleYields(false, kRoleMaster, true, 9));
  CHECK(std::string(roleSourceKey(RoleSource::Chosen)) == "chosen" &&
        std::string(roleSourceKey(RoleSource::Partner)) == "partner" &&
        std::string(roleSourceKey(RoleSource::Address)) == "address" &&
        std::string(roleSourceKey(RoleSource::Kept)) == "kept");
  CHECK(kRequestFlagRoleChosen == 0x02 && kNoticeFlagRoleChosen == 0x01 && kRoleWorker == 0 && kRoleMaster == 1);
  CHECK(kRequestFlagRoleYields == 0x04 && (kRequestFlagRoleYields & (kRequestFlagFresh | kRequestFlagRoleChosen)) == 0);
  // roleSourceNow: chosen; the other than a partner's chosen role; the role
  // the addresses give; else kept from earlier.
  State st = {};
  CHECK(roleSourceNow(st, true, kRoleWorker, true, true) == RoleSource::Chosen);
  CHECK(roleSourceNow(st, false, kRoleMaster, true, true) == RoleSource::Address &&
        roleSourceNow(st, false, kRoleWorker, true, false) == RoleSource::Address);
  CHECK(roleSourceNow(st, false, kRoleWorker, true, true) == RoleSource::Kept &&
        roleSourceNow(st, false, kRoleMaster, true, false) == RoleSource::Kept);
  CHECK(roleSourceNow(st, false, kRoleWorker, false, false) == RoleSource::Address);  // no partner: the rule to come
  st.partnerRoleKnown = true;
  st.partnerRoleChosen = true;
  st.partnerRole = kRoleMaster;
  CHECK(roleSourceNow(st, false, kRoleWorker, true, true) == RoleSource::Partner);
  CHECK(roleSourceNow(st, false, kRoleMaster, true, true) == RoleSource::Address);  // the same role: not from it
  st.partnerRoleChosen = false;  // the partner gave its choice up ('bondrole auto')
  CHECK(roleSourceNow(st, false, kRoleWorker, true, true) == RoleSource::Kept);
}

// Device 0's user chose master. Device 1 (higher address, automatic) took
// master by the addresses. Device 0's request says "chosen": device 1 takes
// worker, answers ACCEPTED as worker, and nobody reports a conflict.
static void caseAutoYieldsToRequest() {
  resetWorld();
  CHECK(bondChosenOrAuto(0, 1, true, kRoleMaster).source == RoleSource::Chosen);
  CHECK(bondChosenOrAuto(1, 0, false, kRoleWorker).role == kRoleMaster);  // higher address, nothing known yet
  CHECK(gDev[0].local.role == kRoleMaster && gDev[1].local.role == kRoleMaster);
  run(2000);
  CHECK(gDev[1].local.role == kRoleWorker && gDev[1].roleSwitches == 1 && gDev[0].roleSwitches == 0);
  CHECK(gDev[0].local.role == kRoleMaster);
  for (int i = 0; i < 2; ++i) {
    CHECK(gDev[i].s.agreement == Agreement::Agreed && !gDev[i].s.roleConflict);
    CHECK(lineCount(i, Line::RoleConflict) == 0);
  }
  CHECK(roleSourceNow(gDev[1].s, false, gDev[1].local.role, true, true) == RoleSource::Partner &&
        roleSourceNow(gDev[0].s, true, gDev[0].local.role, true, false) == RoleSource::Chosen);
  CHECK(gDev[1].s.partnerRoleKnown && gDev[1].s.partnerRole == kRoleMaster && gDev[1].s.partnerRoleChosen);
  // Device 0 knows device 1 did not choose: it never yields itself.
  CHECK(gDev[0].s.partnerRoleKnown && !gDev[0].s.partnerRoleChosen);
}

// The same, with every request of device 0 lost: device 1 learns it from
// device 0's answer (ACCEPTED as master, "chosen": device 0 knows device 1's
// role is automatic and reports no conflict), takes worker, and asks again.
static void caseAutoYieldsToNotice() {
  resetWorld();
  bondChosenOrAuto(0, 1, true, kRoleMaster);
  bondChosenOrAuto(1, 0, false, kRoleWorker);
  gLose = [](const Frame& f) { return f.from == 0 && f.isRequest; };
  run(8000);
  CHECK(gDev[1].local.role == kRoleWorker && gDev[1].roleSwitches == 1);
  CHECK(sentCount(0, false, kReasonAccepted) >= 1 && sentCount(0, false, kReasonRoleConflict) == 0);
  CHECK(lineCount(0, Line::RoleConflict) == 0);
  CHECK(gDev[0].s.agreement == Agreement::Agreed && !gDev[0].s.roleConflict);
  CHECK(gDev[1].s.agreement == Agreement::Agreed && !gDev[1].s.roleConflict);
  CHECK(sentCount(1, true) >= 2);  // asked again with its new role
  CHECK(lineCount(1, Line::RoleConflict) == 0);
}

// Both users chose master: nobody yields; ROLE_CONFLICT says so on both.
// Neither chose and both are master (set by hand on old firmware): nothing
// changes either, as before.
static void caseBothChoseSameRole() {
  resetWorld();
  bondChosenOrAuto(0, 1, true, kRoleMaster);
  bondChosenOrAuto(1, 0, true, kRoleMaster);
  run(2000);
  for (int i = 0; i < 2; ++i) {
    CHECK(gDev[i].local.role == kRoleMaster && gDev[i].roleSwitches == 0);
    CHECK(gDev[i].s.agreement == Agreement::Agreed && gDev[i].s.roleConflict && lineCount(i, Line::RoleConflict) == 1);
  }
  resetWorld();
  bond(0, 1, kRoleMaster);
  bond(1, 0, kRoleMaster);
  run(2000);
  CHECK(gDev[0].roleSwitches == 0 && gDev[1].roleSwitches == 0 && gDev[0].s.roleConflict && gDev[1].s.roleConflict);
}

// A peer that asked to bond before this device bonded is remembered: a later
// bondconnect to it takes the other role than the one its user chose.
static void caseBondconnectUsesPeerChoice() {
  resetWorld();
  bondChosenOrAuto(1, 0, true, kRoleWorker);  // device 1 (higher address) chose worker
  run(1000);                                  // device 0, not bonded, answers NOT_BONDED and keeps the note
  uint8_t role = 9;
  bool chose = false;
  CHECK(roleNoteOf(gDev[0].s, gDev[0].local, gDev[1].mac, &role, &chose) && role == kRoleWorker && chose);
  const RoleChoice c = bondChosenOrAuto(0, 1, false, kRoleWorker);
  CHECK(c.role == kRoleMaster && c.source == RoleSource::Partner);  // the addresses alone would make it worker
  run(2000);
  CHECK(gDev[0].s.agreement == Agreement::Agreed && gDev[1].s.agreement == Agreement::Agreed);
  CHECK(!gDev[0].s.roleConflict && !gDev[1].s.roleConflict && gDev[0].roleSwitches == 0 && gDev[1].roleSwitches == 0);
  // A peer that never asked: nothing known.
  CHECK(!roleNoteOf(gDev[2].s, gDev[2].local, gDev[1].mac, &role, &chose));
}

// A partner whose firmware has no flag (or whose role was not chosen) says
// nothing about a choice: an automatic device keeps the role bondconnect gave
// it. An ENDED that carries a chosen role changes no role.
static void caseNoChoiceNoSwitch() {
  resetWorld();
  bond(0, 1, kRoleMaster);
  gDev[1].local.roleChosen = false;
  bond(1, 0, kRoleWorker);
  run(1000);
  CHECK(gDev[0].roleSwitches == 0 && gDev[1].roleSwitches == 0 && !gDev[0].s.roleConflict);
  Local local = gDev[1].local;
  local.role = kRoleWorker;
  pushInbound(gDev[1].s, false, gDev[0].mac, kRoleWorker, kReasonEnded, 0, kNoticeFlagRoleChosen);
  const Outcome out = takeInbound(gDev[1].s, local, gNow);
  CHECK(out.handled && !out.switchRole && gDev[1].s.agreement == Agreement::Declined);
  CHECK(gDev[1].s.partnerRoleKnown && gDev[1].s.partnerRoleChosen && gDev[1].s.partnerRole == kRoleWorker);
  // A partner change forgets what the old partner said.
  reset(gDev[1].s);
  CHECK(!gDev[1].s.partnerRoleKnown && roleSourceNow(gDev[1].s, false, kRoleMaster, true, true) == RoleSource::Address);
}

// The partner runs firmware from before role choice (no flags, never
// yields). Device 0's user chose master; device 1 is master by its address.
// Device 0 reports the conflict at once (ROLE_CONFLICT), as both devices did
// before, and keeps reporting it: the old partner's requests never clear it.
static void caseOldFirmwarePartner() {
  resetWorld();
  gDev[1].oldFirmware = true;
  bondChosenOrAuto(0, 1, true, kRoleMaster);
  bond(1, 0, kRoleMaster);
  run(2000);
  CHECK(gDev[0].local.role == kRoleMaster && gDev[1].local.role == kRoleMaster && gDev[1].roleSwitches == 0);
  CHECK(gDev[0].s.agreement == Agreement::Agreed && gDev[0].s.roleConflict);
  CHECK(lineCount(0, Line::RoleConflict) == 1);
  const size_t before = gSentLog.size();
  // The old partner's session comes up again: a fresh request, the same answer.
  reset(gDev[1].s);
  run(8000);
  CHECK(gDev[0].s.roleConflict && lineCount(0, Line::RoleConflictOver) == 0 && lineCount(0, Line::RoleConflict) == 1);
  CHECK(sentCount(0, false, kReasonRoleConflict, before) >= 1 && sentCount(0, false, kReasonAccepted, before) == 0);
  // With this firmware on both, the same start ends with device 1 as worker.
  resetWorld();
  bondChosenOrAuto(0, 1, true, kRoleMaster);
  bond(1, 0, kRoleMaster);
  run(2000);
  CHECK(gDev[1].local.role == kRoleWorker && !gDev[0].s.roleConflict && lineCount(0, Line::RoleConflict) == 0);
}

// The partner's session capability word lacks the bond bit (its build has no
// bond mode): planRequest sends nothing and starts no handshake, a confirmed
// word declines with kReasonNoBondBuild once (one line, also over several
// sessions), an unconfirmed one only holds the request, onSessionUp does not
// make it online, and a session whose word has the bit ends the decline.
static void caseNoBondBuild() {
  State s = {};
  Local local = {};
  local.bonded = true;
  std::memcpy(local.partner, "\x02\x00\x00\x00\x00\x0A", 6);
  local.partnerNoBond = true;
  local.partnerNoBondConfirmed = true;
  reset(s);
  uint32_t now = 1000;
  // A request is due; the session is ACTIVE; the hook has not run yet.
  RequestPlan plan = planRequest(s, local, now, true);
  CHECK(!plan.send && !plan.kickHandshake && plan.noBondBuildLine);
  CHECK(s.agreement == Agreement::Declined && s.reason == kReasonNoBondBuild && !s.requestDue);
  CHECK(std::strcmp(reasonKey(kReasonNoBondBuild), "no_bond_build") == 0);
  CHECK(std::strstr(reasonPhrase(kReasonNoBondBuild), "built without bond mode") != nullptr);
  CHECK(kReasonNoBondBuild > kReasonRoleConflict);   // never on the wire
  // The hook: not online.
  CHECK(!onSessionUp(s, now, false, false));
  // An hour of ticks, with and without the session: nothing sent, no kick, one line.
  int lines = 1;
  for (uint32_t t = 0; t < 3600000; t += 1000) {
    now += 1000;
    const bool session = (t / 600000) % 2 == 0;
    if (!hasWork(s, now)) continue;
    plan = planRequest(s, local, now, session);
    CHECK(!plan.send && !plan.kickHandshake);
    lines += plan.noBondBuildLine;
  }
  CHECK(lines == 1);
  // A new session with the same word: no line, still nothing.
  CHECK(!onSessionUp(s, now, true, false));
  plan = planRequest(s, local, now + kRequestRepeatMs, true);
  CHECK(!plan.send && !plan.noBondBuildLine);
  // reset() (a settings change) with the session up: no request; the user is told again.
  reset(s);
  plan = planRequest(s, local, now, true);
  CHECK(!plan.send && plan.noBondBuildLine);
  // A word that is not confirmed holds the request without a verdict.
  State u = {};
  reset(u);
  Local unconfirmed = local;
  unconfirmed.partnerNoBondConfirmed = false;
  plan = planRequest(u, unconfirmed, now, true);
  CHECK(!plan.send && !plan.kickHandshake && !plan.noBondBuildLine && u.agreement == Agreement::Unknown);
  // Reflashed with bond mode: the session's word has the bit.
  local.partnerNoBond = false;
  local.partnerNoBondConfirmed = false;
  CHECK(onSessionUp(s, now, false, true));
  CHECK(s.agreement == Agreement::Unknown && s.reason == kReasonNone && s.requestDue);
  plan = planRequest(s, local, now, true);
  CHECK(plan.send);
}

int main() {
  caseMutual();
  caseOneSided();
  caseBondedElsewhere();
  caseEndedAndBack();
  caseRequestOvertakenByEnded();
  caseNoRequestWhileEnding();
  caseRoleConflict();
  caseNoticesThatDoNotCount();
  caseLostFrames();
  caseInbox();
  casePeerLimits();
  caseMorePeersThanEntries();
  caseRequestFlood();
  caseRequestTiming();
  caseSessionSettle();
  caseDeclinedSessionLost();
  caseDeclineAfterWeeks();
  caseHeartbeat();
  caseTexts();
  caseRoleRules();
  caseAutoYieldsToRequest();
  caseAutoYieldsToNotice();
  caseBothChoseSameRole();
  caseBondconnectUsesPeerChoice();
  caseNoChoiceNoSwitch();
  caseOldFirmwarePartner();
  caseNoBondBuild();
  std::printf("espnow_bond_agree_core: %u checks passed\n", gChecks);
  return 0;
}
