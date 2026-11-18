// Host tests of System_ESPNow_ForwardCore.h, the production rules of a bond
// master (B) forwarding its bond partner's (X) readings, battery state and
// events to the mesh master (M) named on it:
//   * FORWARD_LINK (155) byte by byte, unknown ops, short and hostile frames;
//   * whether B forwards and why not (every row, first match), the partner's
//     agreement (unknown after agreed keeps the link, never agreed does not,
//     declined and a partner change forget it), the link numbers;
//   * B's state per master: announce, back-off, only a matching answer counts,
//     stale, refused and its retry, a new session, a rekey (same session id)
//     keeps the link, query, refresh, withdraw tries, the withdrawal of
//     everything once per session, which frames go to a target;
//   * a frame of the partner only from the link's partner, in the link's hold
//     epoch, with the setting on and no decline;
//   * M: every announce check, the generation rule with late announcements in
//     both orders and a late withdrawal of everything, every withdraw row,
//     every attribution verdict and the resume once per stream, the query
//     limit, which records may be given up, names; any paired device may
//     announce (no allow list);
//   * the primary mesh: only a master paired in the default mesh slot gets
//     forwarded data (forwardDecide, primaryMeshSlot, inPrimaryMesh);
//   * the partner's sensor status: the 16-byte frame with its link byte, and
//     whose it is on M (the readings stream, never a resume);
//   * a sensor request for the partner on B: every row of sensorReqDecide,
//     the leases (subscribe, renew, one-shot, unsubscribe, expiry, a stream
//     X or B's user runs already, by hand on and off, the end of every lease,
//     the "on" sent again on a new bond session);
//   * end to end through the counter rule: X's numbers through B to M with a
//     loss on each hop, forwarding off and on again (no false loss).
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "System_ESPNow_ForwardCore.h"
#include "System_ESPNow_PowerEventCore.h"

using namespace espnowfwd;

static unsigned gChecks = 0;
#define CHECK(c)                                                                 \
  do {                                                                           \
    ++gChecks;                                                                   \
    if (!(c)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); \
      std::exit(1);                                                              \
    }                                                                            \
  } while (0)

static bool is(const char* a, const char* b) { return a && b && std::strcmp(a, b) == 0; }

static const uint8_t kX[6] = { 0x02, 0xA0, 0, 0, 0, 0x01 };    // the partner
static const uint8_t kX2[6] = { 0x02, 0xA0, 0, 0, 0, 0x02 };   // another partner
static const uint8_t kB[6] = { 0x02, 0xB0, 0, 0, 0, 0x01 };    // the bond master (via)
static const uint8_t kM[6] = { 0x02, 0xC0, 0, 0, 0, 0x01 };    // the mesh master

static LinkFrame announce(uint8_t gen, uint16_t bootTag, const uint8_t partner[6] = kX) {
  LinkFrame f = {};
  f.op = kOpAnnounce;
  f.linkGen = gen;
  f.viaBootTag = bootTag;
  f.streams = kStreamsAll;
  f.sensorMask = kSensorMaskDefault;
  std::memcpy(f.partnerMac, partner, 6);
  for (size_t i = 0; i < kPartnerKeyLen; ++i) f.partnerKey[i] = (uint8_t)(0x40 + i + partner[5]);
  std::memcpy(f.partnerName, "porch", 5);
  return f;
}

static LinkFrame withdraw(uint8_t gen, uint16_t bootTag, const uint8_t* partner) {
  LinkFrame f = {};
  f.op = kOpWithdraw;
  f.linkGen = gen;
  f.viaBootTag = bootTag;
  if (partner) std::memcpy(f.partnerMac, partner, 6);
  return f;
}

static LinkFrame answer(uint8_t answers, uint8_t result, uint8_t gen, uint16_t bootTag, const uint8_t* partner = kX) {
  LinkFrame a = {};
  a.op = kOpAnswer;
  a.answers = answers;
  a.result = result;
  a.linkGen = gen;
  a.viaBootTag = bootTag;
  a.streams = kStreamsAll;
  a.sensorMask = kSensorMaskDefault;
  if (partner) std::memcpy(a.partnerMac, partner, 6);
  return a;
}

static void testFrame() {
  LinkFrame f = announce(7, 0xBEEF);
  f.result = 0;
  uint8_t wire[kLinkFrameLen];
  std::memset(wire, 0xEE, sizeof(wire));
  packLinkFrame(wire, f);
  CHECK(wire[0] == 1 && wire[1] == 7 && wire[2] == 0xEF && wire[3] == 0xBE && wire[4] == 0x07 && wire[5] == 0 &&
        wire[6] == 0 && wire[7] == 0);
  CHECK(std::memcmp(wire + 8, kX, 6) == 0);
  CHECK(wire[14] == (kSensorMaskDefault & 0xFF) && wire[15] == (kSensorMaskDefault >> 8));
  CHECK(wire[16] == 0x41 && wire[47] == (uint8_t)(0x40 + 31 + 1));
  CHECK(std::memcmp(wire + 48, "porch", 5) == 0 && wire[53] == 0 && wire[79] == 0);
  for (size_t i = 80; i < kLinkFrameLen; ++i) CHECK(wire[i] == 0);
  LinkFrame back = {};
  CHECK(parseLinkFrame(wire, sizeof(wire), back));
  CHECK(back.op == 1 && back.linkGen == 7 && back.viaBootTag == 0xBEEF && back.streams == kStreamsAll &&
        back.sensorMask == kSensorMaskDefault && std::memcmp(back.partnerMac, kX, 6) == 0 &&
        std::memcmp(back.partnerKey, f.partnerKey, 32) == 0 && std::memcmp(back.partnerName, f.partnerName, 32) == 0);
  // Short, null, unknown ops; a longer payload is read up to 96.
  CHECK(!parseLinkFrame(wire, kLinkFrameLen - 1, back) && !parseLinkFrame(nullptr, kLinkFrameLen, back));
  uint8_t longer[kLinkFrameLen + 30];
  std::memset(longer, 0x5A, sizeof(longer));
  std::memcpy(longer, wire, sizeof(wire));
  CHECK(parseLinkFrame(longer, sizeof(longer), back) && back.linkGen == 7);
  for (int op = 0; op < 256; ++op) {
    wire[0] = (uint8_t)op;
    CHECK(parseLinkFrame(wire, sizeof(wire), back) == (op >= 1 && op <= 4));
  }
  // An announcement of link number 0 is malformed (0 marks "not forwarded",
  // and a request with it would ask for the via's own sensors); the other
  // ops may carry 0 (B's "I forward for nobody" before its first link).
  wire[1] = 0;
  for (int op = 1; op <= 4; ++op) {
    wire[0] = (uint8_t)op;
    CHECK(parseLinkFrame(wire, sizeof(wire), back) == (op != kOpAnnounce));
  }
  wire[0] = kOpAnnounce;
  wire[1] = 7;
  // Hostile bytes: stream and sensor bits outside the tables are dropped.
  uint8_t hostile[kLinkFrameLen];
  std::memset(hostile, 0xFF, sizeof(hostile));
  hostile[0] = kOpAnswer;
  CHECK(parseLinkFrame(hostile, sizeof(hostile), back) && back.streams == kStreamsAll &&
        back.sensorMask == espnowsensor::kSensorMaskAll && back.result == 0xFF);
  // Results and their words.
  for (uint8_t r = 1; r < kResultCount; ++r) {
    if (r == 8) continue;
    CHECK(!is(resultKey(r), "refused") && !is(resultText(r), "for a reason this firmware does not know"));
    for (uint8_t k = 1; k < r; ++k) CHECK(!is(resultKey(r), resultKey(k)));
  }
  CHECK(is(resultKey(0), "refused") && is(resultKey(200), "refused"));
  // 8 is never sent: an answer with it is any refusal.
  CHECK(is(resultKey(8), "refused") && is(resultText(8), "for a reason this firmware does not know") &&
        is(resultKey(kResultStale), "stale") && is(resultKey(kResultNameTaken), "name_taken"));
  CHECK(kLinkFrameLen == 96 && kLinkFrameLen <= espnowsensor::kFrameCapRouted);
  // A name cut on a UTF-8 boundary by the sender's field rule.
  char name[kPartnerNameLen];
  const char* longName = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\xC3\xA9xyz";  // 30 a, then e-acute at 30..31
  CHECK(espnowsensor::eventFieldPut(name, sizeof(name), longName, 40));
  CHECK((uint8_t)name[30] == 0xC3 && (uint8_t)name[31] == 0xA9);
  const char* cutName = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\xC3\xA9";  // 31 a, then e-acute across the end
  CHECK(espnowsensor::eventFieldPut(name, sizeof(name), cutName, 40) && name[31] == 0 && name[30] == 'a');
}

static void testDecide() {
  CHECK(linkGenNext(0) == 1 && linkGenNext(1) == 2 && linkGenNext(254) == 255 && linkGenNext(255) == 1);
  for (int g = 0; g < 256; ++g) CHECK(linkGenNext((uint8_t)g) != 0);
  CHECK(serialNewer(2, 1) && serialNewer(1, 255) && serialNewer(128, 1) && !serialNewer(129, 1) && !serialNewer(5, 5) &&
        !serialNewer(1, 2) && serialNewer(0, 200));

  DecideInputs in = {};
  CHECK(forwardDecide(in) == WhyNot::Off);
  in.settingOn = true;
  CHECK(forwardDecide(in) == WhyNot::NotBondMaster);
  in.bondMaster = true;
  CHECK(forwardDecide(in) == WhyNot::BondNotAgreed);
  in.agreementOk = true;
  CHECK(forwardDecide(in) == WhyNot::MeshOff);
  in.meshOn = true;
  in.meshRole = espnowsensor::kRoleMaster;
  CHECK(forwardDecide(in) == WhyNot::IsMaster);
  in.meshRole = espnowsensor::kRoleBackup;  // a backup that has not taken over forwards
  CHECK(forwardDecide(in) == WhyNot::NoMaster);
  in.masterNamed = true;
  CHECK(forwardDecide(in) == WhyNot::MasterNotPaired);
  in.targetsPaired = 1;
  // Paired, but in another mesh than this device's primary one: nothing goes.
  CHECK(forwardDecide(in) == WhyNot::NotPrimaryMesh);
  in.targetsInPrimary = 1;
  CHECK(forwardDecide(in) == WhyNot::Forwards);
  in.meshRole = espnowsensor::kRoleWorker;
  CHECK(forwardDecide(in) == WhyNot::Forwards);
  for (size_t r = 0; r < kWhyNotCount; ++r) {
    CHECK(!is(whyNotKey((WhyNot)r), "?") && !is(whyNotText((WhyNot)r), "?"));
    for (size_t k = 0; k < r; ++k) CHECK(!is(whyNotKey((WhyNot)r), whyNotKey((WhyNot)k)));
  }
  CHECK(is(whyNotKey(WhyNot::NotKeptHere), "not_kept_here") && is(whyNotKey(WhyNot::PartnerChanged), "partner_changed") &&
        is(whyNotKey(WhyNot::NotPrimaryMesh), "not_primary_mesh"));
  CHECK(meshDataAllowed(false) && !meshDataAllowed(true));

  // The primary mesh: the default enabled slot, else the first enabled one,
  // else slot 0 (the single-mesh settings).
  {
    bool en[4] = { true, true, true, false };
    bool def[4] = { false, false, true, false };
    CHECK(primaryMeshSlot(en, def, 4) == 2);
    def[2] = false;
    CHECK(primaryMeshSlot(en, def, 4) == 0);
    en[0] = false;
    CHECK(primaryMeshSlot(en, def, 4) == 1);
    def[3] = true;  // a default that is not enabled does not count
    CHECK(primaryMeshSlot(en, def, 4) == 1);
    bool none[4] = { false, false, false, false };
    CHECK(primaryMeshSlot(none, none, 4) == 0);
    CHECK(inPrimaryMesh(1, 1) && !inPrimaryMesh(0, 1) && !inPrimaryMesh(-1, 0) && inPrimaryMesh(0, 0));
  }

  // The agreement: unknown before the partner ever agreed does not allow; once
  // agreed, an unknown (the partner out of range) keeps the link.
  AgreeMemo m = {};
  CHECK(!agreementAllows(m, kAgreeUnknown, false, 5));
  CHECK(agreementAllows(m, kAgreeAgreed, false, 5));
  CHECK(agreementAllows(m, kAgreeUnknown, false, 5));
  CHECK(agreementAllows(m, kAgreeUnknown, false, 5));
  // A role conflict or a decline stops it and forgets the agreement.
  CHECK(!agreementAllows(m, kAgreeAgreed, true, 5));
  CHECK(!agreementAllows(m, kAgreeUnknown, false, 5));
  CHECK(agreementAllows(m, kAgreeAgreed, false, 5));
  CHECK(!agreementAllows(m, kAgreeDeclined, 0, 5));
  CHECK(!agreementAllows(m, kAgreeUnknown, false, 5));
  // A partner change (the hold epoch moved) forgets it too.
  CHECK(agreementAllows(m, kAgreeAgreed, false, 5));
  CHECK(!agreementAllows(m, kAgreeUnknown, false, 6));
  CHECK(agreementAllows(m, kAgreeAgreed, false, 6));

  CHECK(streamsForTarget(kTargetPrimary) == kStreamsAll && streamsForTarget(kTargetBackup) == (kStreamBattery | kStreamEvents));
  CHECK(!(kSensorMaskDefault & espnowsensor::sensorBit(REMOTE_SENSOR_INPUT)) &&
        (kSensorMaskDefault | espnowsensor::sensorBit(REMOTE_SENSOR_INPUT)) == espnowsensor::kSensorMaskAll &&
        kSensorMaskDefault == 0x07EF);
}

static void testFrameAllowed() {
  StandingLink link = {};
  CHECK(forwardFrameAllowed(link, kX, false, false, 1, WhyNot::Forwards) == WhyNot::Off);
  CHECK(forwardFrameAllowed(link, kX, true, false, 1, WhyNot::MeshOff) == WhyNot::MeshOff);
  CHECK(forwardFrameAllowed(link, kX, true, false, 1, WhyNot::Forwards) == WhyNot::NotAccepted);
  link.active = true;
  std::memcpy(link.partner, kX, 6);
  link.epoch = 3;
  link.linkGen = 1;
  CHECK(forwardFrameAllowed(link, kX, true, false, 3, WhyNot::Forwards) == WhyNot::Forwards);
  // The new partner's frame before the tick ended the old link: not filed under it.
  CHECK(forwardFrameAllowed(link, kX2, true, false, 3, WhyNot::Forwards) == WhyNot::PartnerChanged);
  CHECK(forwardFrameAllowed(link, kX, true, false, 4, WhyNot::Forwards) == WhyNot::PartnerChanged);
  CHECK(forwardFrameAllowed(link, nullptr, true, false, 3, WhyNot::Forwards) == WhyNot::PartnerChanged);
  CHECK(forwardFrameAllowed(link, kX, true, true, 3, WhyNot::Forwards) == WhyNot::BondNotAgreed);
  // 'bondforward off' stops at once, before the tick withdraws.
  CHECK(forwardFrameAllowed(link, kX, false, false, 3, WhyNot::Forwards) == WhyNot::Off);
}

static TargetWant wantGen(uint8_t gen, uint8_t streams = kStreamsAll) {
  TargetWant w = {};
  w.want = true;
  w.linkGen = gen;
  w.streams = streams;
  w.sensorMask = kSensorMaskDefault;
  return w;
}

static void testTarget() {
  const uint16_t kBoot = 0x0011;
  TargetState t = {};
  t.have = true;
  std::memcpy(t.mac, kM, 6);
  uint32_t now = 1000;
  // No session: nothing, whatever is wanted.
  CHECK(targetStep(t, wantGen(1), 0, false, now) == 0);
  // A session that has not settled: nothing yet (the answering side may not read it).
  CHECK(targetStep(t, wantGen(1), 0x1234, false, now) == 0);
  CHECK(targetStep(t, wantGen(1), 0x1234, true, now) == kActAnnounce && t.state == TState::Announcing);
  // Back-off 2, 4, 8, 16, 32, 60, 60 s without an answer.
  const uint32_t gaps[] = { 2000, 4000, 8000, 16000, 32000, 60000, 60000 };
  for (uint32_t gap : gaps) {
    CHECK(targetStep(t, wantGen(1), 0x1234, true, now + gap - 1) == 0);
    now += gap;
    CHECK(targetStep(t, wantGen(1), 0x1234, true, now) == kActAnnounce);
  }
  // Only a matching answer counts: wrong link number, boot tag, op, or not an answer.
  CHECK(targetAnswer(t, answer(kOpAnnounce, kResultAccepted, 2, kBoot), kBoot, now) == AnswerEffect::Ignored);
  CHECK(targetAnswer(t, answer(kOpAnnounce, kResultAccepted, 1, kBoot + 1), kBoot, now) == AnswerEffect::Ignored);
  CHECK(targetAnswer(t, answer(kOpWithdraw, kResultWithdrawn, 1, kBoot), kBoot, now) == AnswerEffect::Ignored);
  LinkFrame notAnswer = answer(kOpAnnounce, kResultAccepted, 1, kBoot);
  notAnswer.op = kOpQuery;
  CHECK(targetAnswer(t, notAnswer, kBoot, now) == AnswerEffect::Ignored);
  CHECK(t.state == TState::Announcing);
  // Nothing is carried before it accepted.
  CHECK(!targetCarries(t, 0x1234, kStreamBattery, -1));
  CHECK(targetWhyNot(t, 0x1234, kStreamBattery, -1) == WhyNot::NotAccepted);
  CHECK(targetAnswer(t, answer(kOpAnnounce, kResultAccepted, 1, kBoot), kBoot, now) == AnswerEffect::Accepted);
  CHECK(t.state == TState::Accepted && t.acceptedStreams == kStreamsAll && t.acceptedMask == kSensorMaskDefault);
  CHECK(targetCarries(t, 0x1234, kStreamReadings, REMOTE_SENSOR_GPS) && targetCarries(t, 0x1234, kStreamEvents, -1));
  CHECK(!targetCarries(t, 0x1234, kStreamReadings, REMOTE_SENSOR_INPUT));
  CHECK(targetWhyNot(t, 0x1234, kStreamReadings, REMOTE_SENSOR_INPUT) == WhyNot::SensorOff);
  CHECK(!targetCarries(t, 0, kStreamBattery, -1) && targetWhyNot(t, 0, kStreamBattery, -1) == WhyNot::NoSession);
  // A rekey keeps the session id: still accepted, nothing sent, data flows.
  CHECK(targetStep(t, wantGen(1), 0x1234, true, now + 1000) == 0 && t.state == TState::Accepted);
  CHECK(targetCarries(t, 0x1234, kStreamBattery, -1));
  // The 10-minute refresh, same number.
  CHECK(targetStep(t, wantGen(1), 0x1234, true, now + kRefreshMs) == kActAnnounce && t.state == TState::Accepted);
  now += kRefreshMs;
  // A new session (the master restarted): back to announcing, same number;
  // nothing is carried on the new session until it answers.
  CHECK(!targetCarries(t, 0x9999, kStreamBattery, -1));
  CHECK(targetStep(t, wantGen(1), 0x9999, false, now) == 0 && t.state == TState::Announcing);
  CHECK(targetStep(t, wantGen(1), 0x9999, true, now) == kActAnnounce && t.linkGen == 1);
  CHECK(targetAnswer(t, answer(kOpAnnounce, kResultAccepted, 1, kBoot), kBoot, now) == AnswerEffect::Accepted);
  CHECK(targetCarries(t, 0x9999, kStreamBattery, -1));
  // A query: announce again at once.
  targetQuery(t);
  CHECK(targetStep(t, wantGen(1), 0x9999, true, now + 10) == kActAnnounce && t.state == TState::Announcing);
  CHECK(targetAnswer(t, answer(kOpAnnounce, kResultAccepted, 1, kBoot), kBoot, now) == AnswerEffect::Accepted);
  // Refused: retried after 10 minutes, not before.
  TargetState r = t;
  targetQuery(r);
  CHECK(targetStep(r, wantGen(1), 0x9999, true, now) == kActAnnounce);
  CHECK(targetAnswer(r, answer(kOpAnnounce, kResultNameTaken, 1, kBoot), kBoot, now) == AnswerEffect::Refused);
  CHECK(r.state == TState::Refused && r.lastResult == kResultNameTaken && !targetCarries(r, 0x9999, kStreamBattery, -1));
  CHECK(targetWhyNot(r, 0x9999, kStreamBattery, -1) == WhyNot::Refused);
  CHECK(targetStep(r, wantGen(1), 0x9999, true, now + kRefusedRetryMs - 1) == 0);
  CHECK(targetStep(r, wantGen(1), 0x9999, true, now + kRefusedRetryMs) == kActAnnounce && r.state == TState::Announcing);
  // ... or at once when anything changes (another link number).
  TargetState r2 = t;
  targetQuery(r2);
  targetStep(r2, wantGen(1), 0x9999, true, now);
  targetAnswer(r2, answer(kOpAnnounce, kResultFull, 1, kBoot), kBoot, now);
  CHECK(r2.state == TState::Refused && targetStep(r2, wantGen(2), 0x9999, true, now + 1) == kActAnnounce);
  // Stale: the owner steps its number; the target announces again.
  TargetState s = t;
  targetQuery(s);
  targetStep(s, wantGen(1), 0x9999, true, now);
  CHECK(targetAnswer(s, answer(kOpAnnounce, kResultStale, 1, kBoot), kBoot, now) == AnswerEffect::Stale);
  CHECK(s.state == TState::Announcing && targetStep(s, wantGen(2), 0x9999, true, now) == kActAnnounce && s.linkGen == 2);
  // The streams or sensors changed: announced again.
  TargetWant fewer = wantGen(1);
  fewer.sensorMask = espnowsensor::sensorBit(REMOTE_SENSOR_GPS);
  CHECK(targetStep(t, fewer, 0x9999, true, now + 20) == kActAnnounce);
  LinkFrame gpsOnly = answer(kOpAnnounce, kResultAccepted, 1, kBoot);
  gpsOnly.sensorMask = fewer.sensorMask;
  CHECK(targetAnswer(t, gpsOnly, kBoot, now) == AnswerEffect::Accepted);
  CHECK(t.acceptedMask == espnowsensor::sensorBit(REMOTE_SENSOR_GPS));
  // The sensors change while an announcement is still out: announced again at
  // once, and an acceptance echoing the older set does not count (it would
  // carry only the sensors both have until the 10-minute refresh).
  {
    TargetState c = {};
    c.have = true;
    CHECK(targetStep(c, wantGen(1), 0x0303, true, 0) == kActAnnounce);
    TargetWant all = wantGen(1);
    all.sensorMask = espnowsensor::kSensorMaskAll;
    CHECK(targetStep(c, all, 0x0303, true, 100) == kActAnnounce && c.state == TState::Announcing &&
          c.sensorMask == espnowsensor::kSensorMaskAll);
    CHECK(targetAnswer(c, answer(kOpAnnounce, kResultAccepted, 1, kBoot), kBoot, 150) == AnswerEffect::Ignored &&
          c.state == TState::Announcing && c.lastResult == 0);
    LinkFrame allAnswer = answer(kOpAnnounce, kResultAccepted, 1, kBoot);
    allAnswer.sensorMask = espnowsensor::kSensorMaskAll;
    CHECK(targetAnswer(c, allAnswer, kBoot, 200) == AnswerEffect::Accepted &&
          targetCarries(c, 0x0303, kStreamReadings, REMOTE_SENSOR_INPUT));
    // A refusal of the older one still counts (it does not depend on the sensors).
    TargetState d = {};
    d.have = true;
    targetStep(d, wantGen(1), 0x0303, true, 0);
    targetStep(d, all, 0x0303, true, 100);
    CHECK(targetAnswer(d, answer(kOpAnnounce, kResultNameTaken, 1, kBoot), kBoot, 150) == AnswerEffect::Refused);
    // The backup master is announced no sensors and answers none.
    TargetState b = {};
    b.have = true;
    TargetWant backup = wantGen(1, streamsForTarget(kTargetBackup));
    CHECK(targetStep(b, backup, 0x0404, true, 0) == kActAnnounce && announcedMask(b) == 0);
    LinkFrame backupAnswer = answer(kOpAnnounce, kResultAccepted, 1, kBoot);
    backupAnswer.streams = streamsForTarget(kTargetBackup);
    backupAnswer.sensorMask = 0;
    CHECK(targetAnswer(b, backupAnswer, kBoot, 10) == AnswerEffect::Accepted && b.acceptedStreams == backupAnswer.streams);
  }

  // Withdraw: up to three tries 2 s apart until answered, then idle.
  targetBeginWithdraw(t, 1, kX, now);
  CHECK(t.state == TState::Idle && t.wTries == 3 && !targetCarries(t, 0x9999, kStreamBattery, -1));
  TargetWant none = {};
  // (the session was told about a link: no withdrawal of everything this session)
  t.toldSessionId = 0x9999;
  CHECK(targetStep(t, none, 0x9999, true, now) == kActWithdraw);
  CHECK(targetStep(t, none, 0x9999, true, now + 1999) == 0);
  CHECK(targetStep(t, none, 0x9999, true, now + 2000) == kActWithdraw);
  CHECK(targetAnswer(t, answer(kOpWithdraw, kResultWithdrawn, 2, kBoot), kBoot, now) == AnswerEffect::Ignored);
  CHECK(targetAnswer(t, answer(kOpWithdraw, kResultWithdrawn, 1, kBoot, kX2), kBoot, now) == AnswerEffect::Ignored);
  CHECK(targetAnswer(t, answer(kOpWithdraw, kResultWithdrawn, 1, kBoot), kBoot, now) == AnswerEffect::WithdrawDone);
  CHECK(targetStep(t, none, 0x9999, true, now + 4000) == 0);
  // Unanswered: three tries, no more.
  TargetState w = {};
  w.have = true;
  w.state = TState::Accepted;
  w.sessionId = 7;
  w.toldSessionId = 7;
  targetBeginWithdraw(w, 4, kX, 0);
  int sent = 0;
  for (uint32_t ms = 0; ms < 20000; ms += 500) sent += (targetStep(w, none, 7, true, ms) & kActWithdraw) ? 1 : 0;
  CHECK(sent == 3);
  // A target told nothing gets no withdrawal.
  TargetState idle = {};
  targetBeginWithdraw(idle, 4, kX, 0);
  CHECK(idle.wTries == 0);
  // The withdrawal of everything: once per new session while nothing is forwarded.
  TargetState n = {};
  n.have = true;
  CHECK(targetStep(n, none, 0x0101, false, 0) == 0);
  CHECK(targetStep(n, none, 0x0101, true, 0) == kActWithdrawAll);
  CHECK(targetStep(n, none, 0x0101, true, 5000) == 0);
  CHECK(targetStep(n, none, 0x0202, true, 6000) == kActWithdrawAll);  // the master's new session
  targetQuery(n);                                                       // asked: told again
  CHECK(targetStep(n, none, 0x0202, true, 7000) == kActWithdrawAll);
  CHECK(targetAnswer(n, answer(kOpWithdraw, kResultWithdrawn, 0, kBoot, nullptr), kBoot, 0) == AnswerEffect::Noted);
  // Wanting the link after that announces it.
  CHECK(targetStep(n, wantGen(3), 0x0202, true, 8000) == kActAnnounce);
}

static ForwardLink entryFor(const uint8_t via[6]) {
  ForwardLink e = {};
  e.inUse = true;
  std::memcpy(e.via, via, 6);
  return e;
}

static void testAnnounceOnM() {
  const LinkFrame f = announce(1, 10);
  uint8_t self[6];
  std::memcpy(self, kM, 6);
  // Every check, first match.
  // Any paired device may announce: only the role, the partner and the name
  // are checked (there is no list of devices that may forward).
  CHECK(announceCheck(f, kB, self, false, false, false, false) == kResultNotReceiver);
  CHECK(announceCheck(f, kB, self, true, false, false, false) == 0);
  LinkFrame bad = f;
  std::memset(bad.partnerMac, 0, 6);
  CHECK(announceCheck(bad, kB, self, true, false, false, false) == kResultBadPartner);
  bad = f;
  bad.partnerMac[0] = 0x01;  // multicast
  CHECK(announceCheck(bad, kB, self, true, false, false, false) == kResultBadPartner);
  bad = f;
  std::memcpy(bad.partnerMac, kB, 6);  // itself
  CHECK(announceCheck(bad, kB, self, true, false, false, false) == kResultBadPartner);
  bad = f;
  std::memcpy(bad.partnerMac, kM, 6);  // this device
  CHECK(announceCheck(bad, kB, self, true, false, false, false) == kResultBadPartner);
  bad = f;
  std::memset(bad.partnerKey, 0, sizeof(bad.partnerKey));
  CHECK(announceCheck(bad, kB, self, true, false, false, false) == kResultBadPartner);
  CHECK(announceCheck(f, kB, self, true, true, false, false) == kResultKeyMismatch);
  CHECK(announceCheck(f, kB, self, true, true, true, true) == 0);  // paired: this device's own name is used
  CHECK(announceCheck(f, kB, self, true, false, false, true) == kResultNameTaken);
  CHECK(announceCheck(f, kB, self, true, false, false, false) == 0);

  // Apply: a new link; a repeat; a new stream; another partner.
  ForwardLink e = entryFor(kB);
  CHECK(announceFresh(e, f));
  CHECK(linkApplyAnnounce(e, f, "porch", false, 100) == AnnounceEffect::New);
  CHECK(e.state == LState::Active && e.linkGen == 1 && e.viaBootTag == 10 && e.resumeMask == kResumeAll &&
        is(e.name, "porch") && std::memcmp(e.partner, kX, 6) == 0);
  e.resumeMask = 0;
  LinkFrame more = f;
  more.sensorMask = (uint16_t)(f.sensorMask | espnowsensor::sensorBit(REMOTE_SENSOR_INPUT));
  CHECK(announceFresh(e, more) && linkApplyAnnounce(e, more, "porch2", false, 200) == AnnounceEffect::Repeat);
  CHECK(e.resumeMask == espnowsensor::sensorBit(REMOTE_SENSOR_INPUT) && e.confirmedMs == 200 && is(e.name, "porch2"));
  const LinkFrame f2 = announce(2, 10);
  CHECK(announceFresh(e, f2) && linkApplyAnnounce(e, f2, "porch", false, 300) == AnnounceEffect::NewStream);
  CHECK(e.resumeMask == kResumeAll && e.linkGen == 2);
  const LinkFrame other = announce(3, 10, kX2);
  CHECK(announceFresh(e, other) && linkApplyAnnounce(e, other, "shed", false, 400) == AnnounceEffect::Replaced);
  CHECK(std::memcmp(e.partner, kX2, 6) == 0 && e.linkGen == 3);
  // The generation rule: an older announcement of this boot is stale ...
  CHECK(!announceFresh(e, announce(2, 10)) && !announceFresh(e, announce(1, 10, kX2)));
  // ... a repeat of the active one is not; another boot always passes.
  CHECK(announceFresh(e, other) && announceFresh(e, announce(1, 11)));

  // Case A: a held refresh arrives after the withdrawal: not revived.
  ForwardLink a = entryFor(kB);
  linkApplyAnnounce(a, announce(5, 20), "porch", false, 0);
  CHECK(linkApplyWithdraw(a, withdraw(5, 20, kX), 10) && a.state == LState::Ended);
  CHECK(!announceFresh(a, announce(5, 20)));
  // B's next link uses a newer number: accepted.
  CHECK(announceFresh(a, announce(6, 20)));
  // Case B: the old partner's announcement arrives after the new one's.
  ForwardLink b = entryFor(kB);
  linkApplyAnnounce(b, announce(1, 20, kX), "porch", false, 0);
  linkApplyAnnounce(b, announce(2, 20, kX2), "shed", false, 5);
  CHECK(!announceFresh(b, announce(1, 20, kX)) && std::memcmp(b.partner, kX2, 6) == 0);
  // u8 wrap: 255 then 1 is newer.
  ForwardLink c = entryFor(kB);
  linkApplyAnnounce(c, announce(255, 30), "porch", false, 0);
  CHECK(announceFresh(c, announce(1, 30)) && !announceFresh(c, announce(250, 30)));

  // Withdraw rows: one link (all must match), nothing else.
  ForwardLink d = entryFor(kB);
  linkApplyAnnounce(d, announce(4, 40), "porch", false, 0);
  CHECK(!linkApplyWithdraw(d, withdraw(3, 40, kX), 1) && d.state == LState::Active);
  CHECK(!linkApplyWithdraw(d, withdraw(4, 41, kX), 1) && d.state == LState::Active);
  CHECK(!linkApplyWithdraw(d, withdraw(4, 40, kX2), 1) && d.state == LState::Active);
  ForwardLink d2 = entryFor(kB);
  linkApplyAnnounce(d2, announce(4, 40), "porch", false, 0);
  CHECK(linkApplyWithdraw(d2, withdraw(4, 40, kX), 2) && d2.state == LState::Ended && d2.resumeMask == 0);
  CHECK(!linkApplyWithdraw(d2, withdraw(4, 40, kX), 3));  // a repeated withdraw is harmless
  // Withdrawal of everything: a link of another boot, or not newer than its number.
  ForwardLink w1 = entryFor(kB);
  linkApplyAnnounce(w1, announce(4, 40), "porch", false, 0);
  CHECK(!linkApplyWithdraw(w1, withdraw(3, 40, nullptr), 1) && w1.state == LState::Active);  // a late one: the link is newer
  CHECK(linkApplyWithdraw(w1, withdraw(4, 40, nullptr), 1) && w1.state == LState::Ended);
  ForwardLink w2 = entryFor(kB);
  linkApplyAnnounce(w2, announce(9, 40), "porch", false, 0);
  CHECK(linkApplyWithdraw(w2, withdraw(0, 41, nullptr), 1));  // B restarted with nothing to forward
  // (a frame of the old boot cannot arrive any more: it was sealed with a session that is gone)
  CHECK(announceFresh(w2, announce(1, 41)) && !announceFresh(w2, announce(0, 41)));
  CHECK(linkConfirmed(entryFor(kB), 0) == false);
  ForwardLink conf = entryFor(kB);
  linkApplyAnnounce(conf, announce(1, 1), "porch", false, 1000);
  CHECK(linkConfirmed(conf, 1000 + kConfirmedForMs - 1) && !linkConfirmed(conf, 1000 + kConfirmedForMs));
  linkApplyAnnounce(conf, announce(1, 1), "porch", false, 1000 + kConfirmedForMs);
  CHECK(linkConfirmed(conf, 1000 + kConfirmedForMs + 5));
}

static void testAttribute() {
  ForwardLink e = entryFor(kB);
  linkApplyAnnounce(e, announce(3, 50), "porch", false, 0);
  // Not forwarded: own.
  CHECK(attribute(&e, false, true, false, 0, REMOTE_SENSOR_GPS).verdict == Verdict::Own);
  CHECK(attribute(nullptr, false, true, false, 0, REMOTE_SENSOR_GPS).verdict == Verdict::Own);
  CHECK(attribute(nullptr, true, true, false, 0, REMOTE_SENSOR_GPS).verdict == Verdict::Own);  // over the bond link
  // A forwarded mark on the bond link, or a bit and byte that disagree.
  CHECK(attribute(&e, true, true, true, 3, REMOTE_SENSOR_GPS).verdict == Verdict::Malformed);
  CHECK(attribute(&e, true, false, false, 3, kStreamIdBattery).verdict == Verdict::Malformed);
  CHECK(attribute(&e, false, true, true, 0, REMOTE_SENSOR_GPS).verdict == Verdict::Malformed);
  CHECK(attribute(&e, false, true, false, 3, REMOTE_SENSOR_GPS).verdict == Verdict::Malformed);
  // The battery frame has only the byte.
  CHECK(attribute(&e, false, false, false, 3, kStreamIdBattery).verdict == Verdict::Forwarded);
  // No link, an ended link, another number, a stream or sensor not announced.
  CHECK(attribute(nullptr, false, true, true, 3, REMOTE_SENSOR_GPS).verdict == Verdict::Unannounced);
  ForwardLink ended = e;
  ended.state = LState::Ended;
  CHECK(attribute(&ended, false, true, true, 3, REMOTE_SENSOR_GPS).verdict == Verdict::Unannounced);
  ForwardLink queried = entryFor(kB);
  queried.state = LState::Queried;
  CHECK(attribute(&queried, false, true, true, 3, REMOTE_SENSOR_GPS).verdict == Verdict::Unannounced);
  CHECK(attribute(&e, false, true, true, 2, REMOTE_SENSOR_GPS).verdict == Verdict::StaleLink);
  CHECK(attribute(&e, false, true, true, 3, REMOTE_SENSOR_INPUT).verdict == Verdict::StreamNotAnnounced);
  ForwardLink backupOnly = e;
  backupOnly.streams = kStreamBattery | kStreamEvents;
  CHECK(attribute(&backupOnly, false, true, true, 3, REMOTE_SENSOR_GPS).verdict == Verdict::StreamNotAnnounced);
  CHECK(attribute(&backupOnly, false, true, true, 3, kStreamIdEvents).verdict == Verdict::Forwarded);
  CHECK(attribute(&e, false, true, true, 3, 14).verdict == Verdict::StreamNotAnnounced);
  // The sensor status: the byte alone, the readings stream, any sensor, never a resume.
  CHECK(attribute(&e, false, false, false, 3, kStreamIdStatus).verdict == Verdict::Forwarded &&
        !attribute(&e, false, false, false, 3, kStreamIdStatus).resumePending);
  CHECK(attribute(&backupOnly, false, false, false, 3, kStreamIdStatus).verdict == Verdict::StreamNotAnnounced);
  CHECK(attribute(&e, false, false, false, 2, kStreamIdStatus).verdict == Verdict::StaleLink);
  CHECK(attribute(&e, true, false, false, 3, kStreamIdStatus).verdict == Verdict::Malformed);  // over the bond link
  CHECK(attribute(&e, false, false, false, 0, kStreamIdStatus).verdict == Verdict::Own);
  // Forwarded, with the resume once per stream (the owner clears the bit).
  Attribution a = attribute(&e, false, true, true, 3, REMOTE_SENSOR_GPS);
  CHECK(a.verdict == Verdict::Forwarded && a.resumePending);
  // A refused frame does not take the resume.
  CHECK(attribute(&e, false, true, true, 2, REMOTE_SENSOR_GPS).resumePending == false);
  e.resumeMask = (uint16_t)(e.resumeMask & ~(1u << REMOTE_SENSOR_GPS));
  CHECK(!attribute(&e, false, true, true, 3, REMOTE_SENSOR_GPS).resumePending);
  CHECK(attribute(&e, false, true, true, 3, REMOTE_SENSOR_IMU).resumePending);
  CHECK(attribute(&e, false, false, false, 3, kStreamIdBattery).resumePending);
  for (int v = 0; v <= (int)Verdict::StreamNotAnnounced; ++v) CHECK(!is(verdictKey((Verdict)v), "?"));

  // Queries: one per 5 s.
  ForwardLink q = entryFor(kB);
  CHECK(queryDue(q, 0));
  q.queriedOnce = true;
  q.lastQueryMs = 1000;
  CHECK(!queryDue(q, 5999) && queryDue(q, 6000));

  // Which records may be given up.
  for (int m = 0; m < 8; ++m) {
    const bool originPaired = m & 1, viaPaired = m & 2, active = m & 4;
    CHECK(recordReusable(kLinkMesh, originPaired, viaPaired, active) == !originPaired);
    CHECK(recordReusable(kLinkBond, originPaired, viaPaired, active) == !originPaired);
    CHECK(recordReusable(kLinkForwarded, originPaired, viaPaired, active) == (!viaPaired || !active));
  }
  CHECK(is(linkKey(kLinkMesh), "mesh") && is(linkKey(kLinkBond), "bond") && is(linkKey(kLinkForwarded), "forwarded"));
  CHECK(streamBitOf(0) == kStreamReadings && streamBitOf(10) == kStreamReadings && streamBitOf(11) == kStreamBattery &&
        streamBitOf(12) == kStreamEvents && streamBitOf(kStreamIdStatus) == kStreamReadings && streamBitOf(14) == 0);
}

static void testNamesAndStatusFrame() {
  uint8_t mac[6];
  CHECK(macFromText("02:b0:00:00:00:01", 17, mac) && std::memcmp(mac, kB, 6) == 0);
  CHECK(!macFromText("02:B0:00:00:00:0", 16, mac) && !macFromText("02-B0-00-00-00-01", 17, mac) &&
        !macFromText("02:B0:00:00:00:0G", 17, mac));
  CHECK(nameSameIgnoringCase("Porch", "pORCH") && !nameSameIgnoringCase("porch", "porch2") &&
        !nameSameIgnoringCase(nullptr, "a") && nameSameIgnoringCase("", ""));

  // The sensor status frame: 16 bytes, the link byte at 10, the rest 0.
  StatusFrame f = { 0x00000429u, 0x00000009u, 0xBEEF, 7 };
  uint8_t out[kStatusFrameLen];
  std::memset(out, 0xEE, sizeof(out));
  packStatusFrame(out, f);
  const uint8_t expect[kStatusFrameLen] = { 0x29, 0x04, 0, 0, 0x09, 0, 0, 0, 0xEF, 0xBE, 7, 0, 0, 0, 0, 0 };
  CHECK(std::memcmp(out, expect, sizeof(out)) == 0);
  StatusFrame back = {};
  CHECK(parseStatusFrame(out, sizeof(out), back) && back.presentMask == f.presentMask &&
        back.streamingMask == f.streamingMask && back.bootTag == 0xBEEF && back.fwdLink == 7);
  CHECK(!parseStatusFrame(out, sizeof(out) - 1, back) && !parseStatusFrame(nullptr, 16, back));
  CHECK(kStatusFrameLen <= espnowsensor::kFrameCapRouted);
}

static ReqInputs reqAll() {
  ReqInputs in = {};
  in.settingOn = true;
  in.bondMaster = true;
  in.linkActive = true;
  in.linkGen = 4;
  in.fwdLink = 4;
  in.fromAcceptedPrimary = true;
  in.mode = kReqSubscribe;
  in.mask = espnowsensor::sensorBit(REMOTE_SENSOR_TOF);
  in.forwardedMask = kSensorMaskDefault;
  in.partnerOnline = true;
  return in;
}

static void testSensorRequests() {
  // Every row, first match.
  ReqInputs in = reqAll();
  CHECK(sensorReqDecide(in) == ReqVerdict::Apply);
  in.partnerOnline = false;
  CHECK(sensorReqDecide(in) == ReqVerdict::PartnerOffline);
  in.mask = espnowsensor::sensorBit(REMOTE_SENSOR_INPUT);  // not forwarded by default
  CHECK(sensorReqDecide(in) == ReqVerdict::NotForwarded);
  in.mode = kReqUnsubscribe;  // an unsubscribe needs neither the sensors nor the partner
  CHECK(sensorReqDecide(in) == ReqVerdict::Apply);
  in.mode = 3;
  CHECK(sensorReqDecide(in) == ReqVerdict::BadMode);
  in.fromAcceptedPrimary = false;  // the backup master, another device, or not accepted yet
  CHECK(sensorReqDecide(in) == ReqVerdict::NotPrimaryMaster);
  in.fwdLink = 3;
  CHECK(sensorReqDecide(in) == ReqVerdict::OldLink);
  in.linkActive = false;
  CHECK(sensorReqDecide(in) == ReqVerdict::NoLink);
  in.bondMaster = false;
  CHECK(sensorReqDecide(in) == ReqVerdict::NotBondMaster);
  in.settingOn = false;
  CHECK(sensorReqDecide(in) == ReqVerdict::Off);
  for (size_t v = 0; v < kReqVerdictCount; ++v) {
    CHECK(!is(reqVerdictKey((ReqVerdict)v), "?") && !is(reqVerdictText((ReqVerdict)v), "?"));
    for (size_t k = 0; k < v; ++k) CHECK(!is(reqVerdictKey((ReqVerdict)v), reqVerdictKey((ReqVerdict)k)));
  }

  const uint16_t tof = espnowsensor::sensorBit(REMOTE_SENSOR_TOF);
  const uint16_t imu = espnowsensor::sensorBit(REMOTE_SENSOR_IMU);
  const uint16_t gps = espnowsensor::sensorBit(REMOTE_SENSOR_GPS);
  ReqLeases l = {};
  // Subscribe: on at once, once; a renewal sets the end anew and sends nothing.
  LeaseActs a = leaseApply(l, kReqSubscribe, (uint16_t)(tof | imu), 0, 0, 90000, 1000);
  CHECK(a.on == (tof | imu) && a.off == 0 && l.leased == (tof | imu) && l.started == (tof | imu));
  a = leaseApply(l, kReqSubscribe, tof, 0, 0, 90000, 50000);
  CHECK(a.on == 0 && a.off == 0 && l.untilMs[REMOTE_SENSOR_TOF] == 140000 && l.untilMs[REMOTE_SENSOR_IMU] == 91000);
  // Expiry: IMU ran out at 91000 and is switched off; ToF runs on.
  CHECK(leaseExpire(l, 90999) == 0);
  CHECK(leaseExpire(l, 91000) == imu && l.leased == tof && l.started == tof);
  // Unsubscribe: off for what the lease started; nothing for a sensor not leased.
  a = leaseApply(l, kReqUnsubscribe, (uint16_t)(tof | gps), 0, 0, 0, 60000);
  CHECK(a.off == tof && a.on == 0 && l.leased == 0 && l.started == 0);
  // A stream X (or this device's user) runs already is not started by the
  // lease, and its end leaves it on.
  a = leaseApply(l, kReqSubscribe, gps, gps, 0, 1000, 70000);
  CHECK(a.on == 0 && l.leased == gps && l.started == 0);
  CHECK(leaseExpire(l, 71000) == 0 && l.leased == 0);
  // One-shot: held 3 s at least, longer with a longer interval; it never cuts
  // a running subscription short.
  a = leaseApply(l, kReqOneshot, tof, 0, 500, 0, 100000);
  CHECK(a.on == tof && l.untilMs[REMOTE_SENSOR_TOF] == 100000 + kReqOneshotHoldMs);
  CHECK(leaseExpire(l, 100000 + kReqOneshotHoldMs) == tof);
  a = leaseApply(l, kReqOneshot, tof, 0, 8000, 0, 200000);
  CHECK(l.untilMs[REMOTE_SENSOR_TOF] == 208000);
  a = leaseApply(l, kReqSubscribe, tof, 0, 0, 90000, 201000);
  CHECK(a.on == 0 && l.untilMs[REMOTE_SENSOR_TOF] == 291000);
  a = leaseApply(l, kReqOneshot, tof, 0, 500, 0, 202000);
  CHECK(l.untilMs[REMOTE_SENSOR_TOF] == 291000);
  // By hand: on, the stream is the user's (the lease's end leaves it on).
  leaseByHand(l, REMOTE_SENSOR_TOF, true);
  CHECK(l.leased == tof && l.started == 0 && l.byUser == tof && leaseExpire(l, 291000) == 0);
  leaseByHand(l, REMOTE_SENSOR_TOF, false);  // and off again: no longer the user's
  CHECK(l.byUser == 0 && l.leased == 0);
  // By hand off ends the lease with it.
  a = leaseApply(l, kReqSubscribe, imu, 0, 0, 90000, 300000);
  leaseByHand(l, REMOTE_SENSOR_IMU, false);
  CHECK(l.leased == 0 && l.started == 0 && leaseExpire(l, 400000) == 0);
  leaseByHand(l, 200, true);  // out of range: nothing
  // The "on" again on a new bond session, not on the one it went over; none
  // without a session.
  a = leaseApply(l, kReqSubscribe, (uint16_t)(tof | gps), 0, 0, 90000, 500000);
  l.onSession[REMOTE_SENSOR_TOF] = 0x11;
  l.onSession[REMOTE_SENSOR_GPS] = 0x11;
  CHECK(leaseResend(l, 0x11) == 0 && leaseResend(l, 0) == 0 && leaseResend(l, 0x12) == (tof | gps));
  l.onSession[REMOTE_SENSOR_GPS] = 0;  // its "on" was not sent
  CHECK(leaseResend(l, 0x11) == gps);
  // Every lease ends (the link ended): what a lease started is switched off.
  CHECK(leaseEndAll(l) == (tof | gps) && l.leased == 0 && l.started == 0 && leaseEndAll(l) == 0);
  // Across the wrap of the uptime clock.
  a = leaseApply(l, kReqSubscribe, tof, 0, 0, 10000, 0xFFFFF000u);
  CHECK(leaseExpire(l, 0xFFFFFFF0u) == 0 && leaseExpire(l, 0x00001710u) == tof);
}

// Review of 2026-11-18: the "off" a lease owes, the user's streams, the
// leases that end when their sensor or their master goes, a shorter renewal.
static void testLeaseEnds() {
  const uint16_t tof = espnowsensor::sensorBit(REMOTE_SENSOR_TOF);
  const uint16_t imu = espnowsensor::sensorBit(REMOTE_SENSOR_IMU);
  const uint16_t gps = espnowsensor::sensorBit(REMOTE_SENSOR_GPS);
  ReqLeases l = {};
  // Every end of a lease that started its sensor owes the "off" until it was
  // sent: expiry, unsubscribe, every lease at once.
  LeaseActs a = leaseApply(l, kReqSubscribe, (uint16_t)(tof | imu | gps), 0, 0, 10000, 1000);
  CHECK(a.on == (tof | imu | gps) && l.offOwed == 0);
  a = leaseApply(l, kReqSubscribe, gps, 0, 0, 50000, 2000);
  CHECK(leaseExpire(l, 11000) == (tof | imu) && l.offOwed == (tof | imu));
  leaseOffSent(l, tof);
  CHECK(l.offOwed == imu);  // the "off" for IMU could not be sent: still owed
  a = leaseApply(l, kReqUnsubscribe, gps, 0, 0, 0, 12000);
  CHECK(a.off == gps && l.offOwed == (imu | gps));
  leaseOffSent(l, gps);
  // IMU streams at X because its "off" did not go out, so X's status says it
  // streams: a new lease takes it over (not "the user's"), and its end owes
  // the "off" again.
  a = leaseApply(l, kReqSubscribe, imu, imu, 0, 5000, 20000);
  CHECK(a.on == imu && l.started == imu && l.offOwed == 0);
  CHECK(leaseExpire(l, 25000) == imu && l.offOwed == imu);
  // A stream the user switches by hand meanwhile is the user's: nothing owed.
  leaseByHand(l, REMOTE_SENSOR_IMU, true);
  CHECK(l.offOwed == 0 && l.byUser == imu);
  // ... and no lease starts or stops it, whatever X's status says.
  a = leaseApply(l, kReqSubscribe, imu, 0, 0, 5000, 30000);
  CHECK(a.on == 0 && l.started == 0 && leaseExpire(l, 35000) == 0 && l.offOwed == 0);
  leaseByHand(l, REMOTE_SENSOR_IMU, false);
  a = leaseApply(l, kReqSubscribe, imu, 0, 0, 5000, 40000);
  CHECK(a.on == imu && l.started == imu);
  CHECK(leaseEndAll(l) == imu && l.offOwed == imu && l.leased == 0);
  // Another partner, or no longer its bond master: everything is forgotten.
  leaseByHand(l, REMOTE_SENSOR_GPS, true);
  leaseForgetPartner(l);
  CHECK(l.offOwed == 0 && l.byUser == 0 && l.leased == 0 && l.started == 0);
  // Leases on sensors no longer forwarded end; with keep 0 (the master left
  // the primary mesh) all of them. A lease that started nothing owes nothing.
  a = leaseApply(l, kReqSubscribe, (uint16_t)(tof | imu), 0, 0, 90000, 50000);
  a = leaseApply(l, kReqSubscribe, gps, gps, 0, 90000, 50000);
  CHECK(leaseKeepOnly(l, (uint16_t)(tof | gps)) == imu && l.leased == (tof | gps) && l.offOwed == imu);
  CHECK(leaseKeepOnly(l, (uint16_t)(tof | gps)) == 0);
  CHECK(leaseKeepOnly(l, 0) == tof && l.leased == 0 && l.offOwed == (imu | tof));
  // A renewal with a shorter lease moves the end earlier (a one-shot never does).
  l = ReqLeases{};
  a = leaseApply(l, kReqSubscribe, tof, 0, 0, 90000, 1000);
  a = leaseApply(l, kReqSubscribe, tof, 0, 0, 10000, 2000);
  CHECK(l.untilMs[REMOTE_SENSOR_TOF] == 12000 && leaseExpire(l, 12000) == tof);
}

// X's sequence through B to M by the counter rule, with B forwarding in
// order. Loss on either hop shows on M; a pause of the forwarding does not.
static void testEndToEnd() {
  using espnowsensor::CounterCounts;
  using espnowsensor::CounterTrack;
  using espnowsensor::counterJudge;
  using espnowsensor::counterVerdictKeeps;
  CounterTrack onB = {}, onM = {};
  CounterCounts cB = {}, cM = {};
  ForwardLink e = entryFor(kB);
  linkApplyAnnounce(e, announce(1, 7), "porch", false, 0);
  uint32_t now = 0;
  auto send = [&](uint16_t counter, bool lostToB, bool lostToM) {
    now += 100;
    if (lostToB) return;
    const bool kept = counterVerdictKeeps(counterJudge(onB, cB, 0xAAAA, counter, false, now));
    if (!kept || lostToM) return;
    const Attribution a = attribute(&e, false, true, true, e.linkGen, REMOTE_SENSOR_GPS);
    CHECK(a.verdict == Verdict::Forwarded);
    counterJudge(onM, cM, 0xAAAA, counter, a.resumePending, now);
    if (a.resumePending) e.resumeMask = (uint16_t)(e.resumeMask & ~(1u << REMOTE_SENSOR_GPS));
  };
  for (uint16_t c = 1; c <= 100; ++c) send(c, c == 30, c == 60);
  CHECK(cB.received == 99 && cB.lost == 1 && cM.received == 98 && cM.lost == 2);
  // Forwarding off for 40000 readings, then on again (a new link number):
  // the first reading is a resume, nothing more is lost.
  linkApplyWithdraw(e, withdraw(1, 7, kX), now);
  for (uint32_t c = 101; c <= 40100; ++c) send((uint16_t)c, false, true);
  linkApplyAnnounce(e, announce(2, 7), "porch", false, now);
  for (uint32_t c = 40101; c <= 40110; ++c) send((uint16_t)c, false, false);
  CHECK(cM.lost == 2 && cM.resumes == 1 && cM.received == 108);
  // X restarts: another boot tag, a restart on M.
  now += 100;
  CHECK(counterJudge(onM, cM, 0xBBBB, 1, false, now) == espnowsensor::CounterVerdict::Restart);
}

int main() {
  testFrame();
  testDecide();
  testFrameAllowed();
  testTarget();
  testAnnounceOnM();
  testAttribute();
  testNamesAndStatusFrame();
  testSensorRequests();
  testLeaseEnds();
  testEndToEnd();
  std::printf("espnow_forward_core_tests: %u checks passed\n", gChecks);
  return 0;
}
