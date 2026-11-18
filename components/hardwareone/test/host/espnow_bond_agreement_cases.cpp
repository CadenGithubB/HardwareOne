// Cases for test_espnow_bond_agreement.py: what the two ends of a bond tell
// each other (BOND_REQUEST / BOND_NOTICE) and what this device does with it.
//
// The runner appends this file to the doubles and pasted production code of
// espnow_bond_partner_change_harness.cpp (everything above its "Cases" line):
// System_ESPNow_BondAgreeCore.h as is, the agreement glue with v4h_bond_request
// and v4h_bond_notice, v4h_bond_heartbeat, bondNotifySessionEstablished, the
// bond parts of the espnow_task loop (hbTickProduction with the agreement tick,
// syncTickProduction) and the bond commands. "This device" is the one under
// test; the peers are scripted. Frames reach the handlers the way the receive
// table lets them: the agreement rows from any paired peer with a session, the
// heartbeat only from the configured partner (test_espnow_dispatch_policy.py
// runs the table itself).

static const uint8_t* const kPeerA = kPartnerA;   // 02:11:22:33:44:01
static const uint8_t* const kPeerB = kPartnerB;   // 02:11:22:33:44:02
static const uint8_t kPeerC[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x03};
static const char* const kMacA = "02:11:22:33:44:01";
static const char* const kMacB = "02:11:22:33:44:02";

static void tick() {
  hbTickProduction(gNowMs);
  syncTickProduction(gNowMs);
}

// `ms` of the espnow_task loop in 100 ms steps; `everyStep` is what the peers do.
static void advance(uint32_t ms, const std::function<void()>& everyStep = nullptr) {
  for (uint32_t t = 0; t < ms; t += 100) {
    gNowMs += 100;
    if (everyStep) everyStep();
    tick();
  }
}

// ---- Frames from a peer, through the receive table's conditions --------------------
static void rxRequest(const uint8_t* src, uint8_t role, uint8_t flags, uint16_t seq) {
  if (!bondSessionActiveWith(src)) return;   // REQ_SESSION_ENC
  V4PayloadBondRequest rq = {};
  rq.role = role;
  rq.flags = flags;
  rq.seq = seq;
  esp_now_recv_info info{src};
  V4RxCtx ctx{&info, reinterpret_cast<const uint8_t*>(&rq), sizeof(rq), "peer"};
  v4h_bond_request(ctx);
}

static void rxNotice(const uint8_t* src, uint8_t reason, uint8_t role, uint16_t seq, uint8_t flags = 0) {
  if (!bondSessionActiveWith(src)) return;
  V4PayloadBondNotice n = {};
  n.reason = reason;
  n.role = role;
  n.seq = seq;
  n.flags = flags;
  esp_now_recv_info info{src};
  V4RxCtx ctx{&info, reinterpret_cast<const uint8_t*>(&n), sizeof(n), "peer"};
  v4h_bond_notice(ctx);
}

// A bond heartbeat: REQ_BOND_PEER, so only the configured partner's arrives.
static bool rxHeartbeat(const uint8_t* src, uint8_t role) {
  if (!(gSettings.bondModeEnabled && isBondPeerMac(src) && bondSessionActiveWith(src))) return false;
  V4PayloadBondHeartbeat hb = {};
  hb.role = role;
  hb.bootCounter = 3;
  esp_now_recv_info info{src};
  V4RxCtx ctx{&info, reinterpret_cast<const uint8_t*>(&hb), sizeof(hb), "peer"};
  v4h_bond_heartbeat(ctx);
  return true;
}

// ---- What this device sent and said ---------------------------------------------------
static size_t requestsTo(const uint8_t* mac, size_t from = 0) {
  size_t n = 0;
  for (size_t i = from; i < gAgreeFrames.size(); ++i) {
    n += gAgreeFrames[i].type == ESPNOW_V4_TYPE_BOND_REQUEST && macEquals(gAgreeFrames[i].mac, mac);
  }
  return n;
}
static size_t noticesTo(const uint8_t* mac, int reason = -1, size_t from = 0) {
  size_t n = 0;
  for (size_t i = from; i < gAgreeFrames.size(); ++i) {
    const AgreeFrame& f = gAgreeFrames[i];
    n += f.type == ESPNOW_V4_TYPE_BOND_NOTICE && macEquals(f.mac, mac) && (reason < 0 || f.b0 == reason);
  }
  return n;
}
static const AgreeFrame* lastAgreeFrame(const uint8_t* mac, uint8_t type) {
  for (size_t i = gAgreeFrames.size(); i-- > 0;) {
    if (gAgreeFrames[i].type == type && macEquals(gAgreeFrames[i].mac, mac)) return &gAgreeFrames[i];
  }
  return nullptr;
}
static size_t framesOfType(const uint8_t* mac, uint8_t type, size_t from = 0) {
  size_t n = 0;
  for (size_t i = from; i < gFrames.size(); ++i) n += gFrames[i].type == type && macEquals(gFrames[i].mac, mac);
  return n;
}
static size_t linesWith(const char* text, size_t from = 0) {
  size_t n = 0;
  for (size_t i = from; i < gEventLines.size(); ++i) n += gEventLines[i].find(text) != std::string::npos;
  return n;
}
static size_t posted(uint8_t type) {
  size_t n = 0;
  for (const auto& e : gPosted) n += e.type == type;
  return n;
}
static uint8_t agreementState() { return bondAgreementView().state; }

// This device bonded to A (session up), nothing known about A's side yet. The
// modelled master of the older cases is pointed elsewhere: A answers only what
// a case sends.
static void bondedToA(const uint8_t self[6], uint8_t role) {
  resetWorld(self);
  gSettings.bondModeEnabled = true;
  gSettings.bondRole = role;
  gSettings.bondPeerMac = kMacA;
  gMaster.mac = kPeerC;
}

// The request this device sends once A's session is up; returns its number.
// The side that started the handshake (the higher address) asks at once, the
// side that answered it a second later.
static uint16_t sessionUpAndRequest(const uint8_t* partner) {
  bondNotifySessionEstablished(partner);
  tick();
  if (sessionIsASide(gSelfMac, partner)) advance(bondagree::kSessionSettleMs);
  const AgreeFrame* rq = lastAgreeFrame(partner, ESPNOW_V4_TYPE_BOND_REQUEST);
  return rq ? rq->seq : 0;
}
static size_t authLinesWith(const char* text, size_t from = 0) {
  size_t n = 0;
  for (size_t i = from; i < gAuthLines.size(); ++i) n += gAuthLines[i].find(text) != std::string::npos;
  return n;
}

// ---- Cases ---------------------------------------------------------------------------

// Both devices are bonded to each other. This device asks when the session
// comes up; A's own request makes the bond agreed here and is answered
// ACCEPTED; A's ACCEPTED changes nothing more. Once agreed nothing is asked.
static void caseMutual() {
  bondedToA(kMasterSelf, BOND_ROLE_MASTER);
  const uint16_t seq = sessionUpAndRequest(kPeerA);
  const AgreeFrame* rq = lastAgreeFrame(kPeerA, ESPNOW_V4_TYPE_BOND_REQUEST);
  CHECK(requestsTo(kPeerA) == 1 && rq && seq != 0 && rq->b0 == BOND_ROLE_MASTER &&
        (rq->b1 & BOND_REQUEST_FLAG_FRESH) && !rq->direct,
        "the partner is asked once when its session comes up (%zu requests, seq %u)", requestsTo(kPeerA),
        (unsigned)seq);
  CHECK(agreementState() == BOND_AGREEMENT_UNKNOWN, "no answer yet: the agreement is unknown");
  CHECK(gWatchedSends.empty(), "the first request on a new session is not held against the session when it is lost");
  CHECK(gAuthLines.size() == 1 && gAuthLines[0] == "Bond session active (role=master)",
        "the bond session has its auth-log record (%zu)", gAuthLines.size());

  rxRequest(kPeerA, BOND_ROLE_WORKER, BOND_REQUEST_FLAG_FRESH, 7);
  CHECK(noticesTo(kPeerA) == 0, "the answer waits for the tick");
  tick();
  const AgreeFrame* answer = lastAgreeFrame(kPeerA, ESPNOW_V4_TYPE_BOND_NOTICE);
  CHECK(noticesTo(kPeerA) == 1 && answer && answer->b0 == BOND_NOTICE_ACCEPTED && answer->seq == 7 &&
        answer->b1 == BOND_ROLE_MASTER,
        "the partner's request is answered ACCEPTED with its number and our role");
  CHECK(agreementState() == BOND_AGREEMENT_AGREED && linesWith("accepted the bond") == 1,
        "the partner's request makes the bond agreed, with one line (%zu)", linesWith("accepted the bond"));

  rxNotice(kPeerA, BOND_NOTICE_ACCEPTED, BOND_ROLE_WORKER, seq);
  tick();
  CHECK(agreementState() == BOND_AGREEMENT_AGREED && linesWith("accepted the bond") == 1 &&
        !bondAgreementView().roleConflict, "the partner's ACCEPTED keeps it agreed without a second line");

  const size_t before = gAgreeFrames.size();
  uint32_t beat = 0;
  advance(5 * 60000, [&] {
    if ((beat += 100) % 5000 == 0) rxHeartbeat(kPeerA, BOND_ROLE_WORKER);
  });
  CHECK(gAgreeFrames.size() == before, "an agreed bond sends no more requests or notices (%zu in 5 min)",
        gAgreeFrames.size() - before);
  CHECK(gEspNow->bondPeerOnline && gSessionClears.empty() && gSettings.bondModeEnabled &&
        gSettings.bondPeerMac == String(kMacA), "the agreed bond stays up and its settings are as they were");
  char text[160];
  bondAgreementDescribe(text, sizeof text);
  CHECK(std::strstr(text, "is bonded to this device too") != nullptr, "status line: %s", text);
}

// A request from a paired peer that is not the partner. It is answered
// NOT_BONDED (bond mode off here) or BONDED_ELSEWHERE (another partner), the
// user gets one line saying what to run, and nothing about this device's bond
// changes.
static void caseRequestFromNonPartner() {
  resetWorld(kWorkerSelf);                      // bond mode off, no partner
  rxRequest(kPeerB, BOND_ROLE_MASTER, BOND_REQUEST_FLAG_FRESH, 3);
  tick();
  const AgreeFrame* answer = lastAgreeFrame(kPeerB, ESPNOW_V4_TYPE_BOND_NOTICE);
  CHECK(noticesTo(kPeerB) == 1 && answer && answer->b0 == BOND_NOTICE_NOT_BONDED && answer->seq == 3,
        "bond mode off: the request is answered NOT_BONDED");
  CHECK(!gSettings.bondModeEnabled && gSettings.bondPeerMac.length() == 0 && !gEspNow->bondPeerOnline &&
        agreementState() == BOND_AGREEMENT_UNKNOWN, "a request does not turn bond mode on or set a partner");
  CHECK(linesWith("wants to bond with this device; run 'bondconnect 02:11:22:33:44:02' here to accept") == 1,
        "the user is told who asked and what to run (%zu lines)", gEventLines.size());

  // The once-a-minute repeats are answered each time; the line is not repeated
  // for 15 minutes. A fresh request (the peer's session came up again) may
  // bring it back after a minute.
  for (int i = 0; i < 14; ++i) {
    advance(60000);
    rxRequest(kPeerB, BOND_ROLE_MASTER, 0, (uint16_t)(4 + i));
    tick();
  }
  CHECK(noticesTo(kPeerB, BOND_NOTICE_NOT_BONDED) == 15 && linesWith("wants to bond") == 1,
        "14 repeats in 14 minutes: every one answered (%zu), no second line (%zu)",
        noticesTo(kPeerB, BOND_NOTICE_NOT_BONDED), linesWith("wants to bond"));
  advance(61000);
  rxRequest(kPeerB, BOND_ROLE_MASTER, 0, 30);
  tick();
  CHECK(linesWith("wants to bond") == 2, "after 15 minutes a repeat brings the line once more");
  advance(30000);
  rxRequest(kPeerB, BOND_ROLE_MASTER, BOND_REQUEST_FLAG_FRESH, 31);
  tick();
  CHECK(linesWith("wants to bond") == 2, "a fresh request 30 s after the line does not repeat it");
  advance(31000);
  rxRequest(kPeerB, BOND_ROLE_MASTER, BOND_REQUEST_FLAG_FRESH, 32);
  tick();
  CHECK(linesWith("wants to bond") == 3, "a fresh request a minute after the line repeats it");

  // Bonded to A and agreed: B's request is answered BONDED_ELSEWHERE. Neither
  // the frame (8 bytes: reason, role, number) nor the line names A.
  bondedToA(kMasterSelf, BOND_ROLE_MASTER);
  const uint16_t seq = sessionUpAndRequest(kPeerA);
  rxNotice(kPeerA, BOND_NOTICE_ACCEPTED, BOND_ROLE_WORKER, seq);
  tick();
  const size_t lines = gEventLines.size();
  rxRequest(kPeerB, BOND_ROLE_MASTER, BOND_REQUEST_FLAG_FRESH, 9);
  tick();
  answer = lastAgreeFrame(kPeerB, ESPNOW_V4_TYPE_BOND_NOTICE);
  CHECK(answer && answer->b0 == BOND_NOTICE_BONDED_ELSEWHERE && answer->seq == 9,
        "bonded to another device: the request is answered BONDED_ELSEWHERE");
  CHECK(sizeof(V4PayloadBondNotice) == 8, "the notice has no room for an address");
  // bondconnect gives the higher address the master role, so this device's
  // role would tell B on which side of it the partner's address lies.
  CHECK(gSettings.bondRole == BOND_ROLE_MASTER && answer && answer->b1 == 0,
        "the answer to a peer that is not the partner does not carry this device's role (%u)",
        answer ? (unsigned)answer->b1 : 99u);
  CHECK(gEventLines.size() == lines + 1 && linesWith("bonded to another device", lines) == 1 &&
        linesWith(kMacA, lines) == 0, "the line about B's request does not name the partner");
  CHECK(gSettings.bondModeEnabled && gSettings.bondPeerMac == String(kMacA) && gEspNow->bondPeerOnline &&
        agreementState() == BOND_AGREEMENT_AGREED && noticesTo(kPeerA) == 0,
        "B's request changed nothing about the bond with A");

  // B asks without pause (100 requests in 10 s): one answer per 5 s, the
  // later one with the number of B's newest request by then, and one line.
  const size_t answersBefore = noticesTo(kPeerB), linesBefore = linesWith("wants to bond");
  const auto partnerBeats = [] {
    if (gNowMs % 5000 == 0) rxHeartbeat(kPeerA, BOND_ROLE_WORKER);
  };
  advance(60000, partnerBeats);
  for (int i = 0; i < 100; ++i) {
    rxRequest(kPeerB, BOND_ROLE_MASTER, BOND_REQUEST_FLAG_FRESH, (uint16_t)(100 + i));
    advance(100, partnerBeats);
  }
  // The first is answered at once, the next one 5 s later: number 150.
  answer = lastAgreeFrame(kPeerB, ESPNOW_V4_TYPE_BOND_NOTICE);
  CHECK(noticesTo(kPeerB) - answersBefore == 2 && answer && answer->seq == 150 && answer->b1 == 0,
        "100 requests in 10 s from a peer that is not the partner: %zu answers (want 2), the last to number %u "
        "(want 150)", noticesTo(kPeerB) - answersBefore, answer ? (unsigned)answer->seq : 0u);
  CHECK(linesWith("wants to bond") - linesBefore == 1, "... and one line (%zu)",
        linesWith("wants to bond") - linesBefore);
  CHECK(noticesTo(kPeerA) == 0 && agreementState() == BOND_AGREEMENT_AGREED, "... and the partner is left alone");

  // Two requests in a row (a bond command given twice on B): the first is
  // answered at once, the second when the 5 s are over, to its own number, so
  // B's newest request does not stay unanswered until its repeat a minute on.
  advance(60000, partnerBeats);
  const size_t twice = noticesTo(kPeerB);
  rxRequest(kPeerB, BOND_ROLE_MASTER, BOND_REQUEST_FLAG_FRESH, 300);
  advance(100, partnerBeats);
  rxRequest(kPeerB, BOND_ROLE_MASTER, BOND_REQUEST_FLAG_FRESH, 301);
  advance(4800, partnerBeats);
  answer = lastAgreeFrame(kPeerB, ESPNOW_V4_TYPE_BOND_NOTICE);
  CHECK(noticesTo(kPeerB) - twice == 1 && answer && answer->seq == 300, "the first of two requests is answered at once");
  advance(200, partnerBeats);
  answer = lastAgreeFrame(kPeerB, ESPNOW_V4_TYPE_BOND_NOTICE);
  CHECK(noticesTo(kPeerB) - twice == 2 && answer && answer->seq == 301 && answer->b0 == BOND_NOTICE_BONDED_ELSEWHERE &&
        answer->b1 == 0, "the second is answered 5 s after the first, to its own number (%u)",
        answer ? (unsigned)answer->seq : 0u);

  // No session with the asker: nothing is sent and no handshake is started.
  gSessionDownFor = kPeerB;
  const size_t frames = gFrames.size();
  V4PayloadBondRequest rq = {};
  rq.seq = 10;
  esp_now_recv_info info{kPeerB};
  V4RxCtx ctx{&info, reinterpret_cast<const uint8_t*>(&rq), sizeof(rq), "peer"};
  v4h_bond_request(ctx);
  tick();
  CHECK(framesOfType(kPeerB, ESPNOW_V4_TYPE_BOND_NOTICE, frames) == 0 && gAutoStarts == 0 && gManualStarts == 0,
        "without a session the answer is not sent and no handshake is started for it");

  // A short frame and a request numbered 0 are not requests.
  bondedToA(kMasterSelf, BOND_ROLE_MASTER);
  rq.seq = 0;
  esp_now_recv_info infoA{kPeerA};
  V4RxCtx zero{&infoA, reinterpret_cast<const uint8_t*>(&rq), sizeof(rq), "peer"};
  v4h_bond_request(zero);
  V4RxCtx shortFrame{&infoA, reinterpret_cast<const uint8_t*>(&rq), sizeof(rq) - 1, "peer"};
  v4h_bond_request(shortFrame);
  v4h_bond_notice(shortFrame);
  bondNotifySessionEstablished(kPeerC);          // someone else's session: not ours to ask
  tick();
  CHECK(noticesTo(kPeerA) == 0 && agreementState() == BOND_AGREEMENT_UNKNOWN,
        "a request numbered 0 or a short frame is dropped");
}

// A notice counts only from the configured partner, and an answer only when it
// is to the last request sent.
static void caseNoticeFromNonPartner() {
  bondedToA(kMasterSelf, BOND_ROLE_MASTER);
  const uint16_t seq = sessionUpAndRequest(kPeerA);
  rxNotice(kPeerA, BOND_NOTICE_ACCEPTED, BOND_ROLE_WORKER, seq);
  tick();
  const size_t lines = gEventLines.size();
  for (uint8_t reason : {(uint8_t)BOND_NOTICE_ENDED, (uint8_t)BOND_NOTICE_NOT_BONDED,
                         (uint8_t)BOND_NOTICE_BONDED_ELSEWHERE}) {
    rxNotice(kPeerB, reason, BOND_ROLE_WORKER, 0);
    tick();
  }
  CHECK(agreementState() == BOND_AGREEMENT_AGREED && gEspNow->bondPeerOnline && gEventLines.size() == lines &&
        posted(SYSEVT_BOND_OFFLINE) == 0 && gSettings.bondPeerMac == String(kMacA),
        "ENDED, NOT_BONDED and BONDED_ELSEWHERE from a peer that is not the partner change nothing");
  CHECK(gBondAgree.noticesIgnored == 3, "the three notices were counted as ignored (%lu)",
        (unsigned long)gBondAgree.noticesIgnored);

  // The partner's answer to an older request, and a reason this firmware does
  // not know, are not used either.
  rxNotice(kPeerA, BOND_NOTICE_NOT_BONDED, BOND_ROLE_WORKER, (uint16_t)(seq + 100));
  tick();
  rxNotice(kPeerA, 99, BOND_ROLE_WORKER, 0);
  tick();
  CHECK(agreementState() == BOND_AGREEMENT_AGREED && gEspNow->bondPeerOnline,
        "an answer to another request and an unknown reason leave the agreement alone");

  // With bond mode off nobody is the partner: an ACCEPTED bonds nothing.
  resetWorld(kWorkerSelf);
  rxNotice(kPeerA, BOND_NOTICE_ACCEPTED, BOND_ROLE_MASTER, 0);
  tick();
  CHECK(!gSettings.bondModeEnabled && gSettings.bondPeerMac.length() == 0 && gEventLines.empty() &&
        gAgreeFrames.empty(), "a notice to a device with bond mode off does nothing");
}

// The partner ends the bond. This device keeps its setting, but stops the
// heartbeat, the capability requests and the timeout that cleared the session
// every 15 s; it asks once a minute. When the partner bonds again the bond
// comes back by itself.
static void caseEndedStopsTheCycle() {
  bondedToA(kMasterSelf, BOND_ROLE_MASTER);
  gMaster.mac = kPeerA;                          // A answers the bond sync until it ends the bond
  const uint16_t seq = sessionUpAndRequest(kPeerA);
  rxNotice(kPeerA, BOND_NOTICE_ACCEPTED, BOND_ROLE_WORKER, seq);
  rxHeartbeat(kPeerA, BOND_ROLE_WORKER);
  tick();
  CHECK(gEspNow->bondPeerOnline && agreementState() == BOND_AGREEMENT_AGREED, "bonded, agreed and online");
  uint32_t beat = 0;
  advance(30000, [&] {
    if ((beat += 100) % 5000 == 0) rxHeartbeat(kPeerA, BOND_ROLE_WORKER);
  });
  gEspNow->bondSettingsReceived = true;          // the settings came too (their transfer is not modelled)
  CHECK(gEspNow->lastRemoteCapValid && gEspNow->bondManifestReceived && gEspNow->bondCapSent,
        "the bond synced before the partner ended it (cap=%d manifest=%d capSent=%d)",
        (int)gEspNow->lastRemoteCapValid, (int)gEspNow->bondManifestReceived, (int)gEspNow->bondCapSent);
  gMaster.mac = kPeerC;                          // from here A answers only what the case sends

  rxNotice(kPeerA, BOND_NOTICE_ENDED, BOND_ROLE_WORKER, 0);
  CHECK(gEspNow->bondPeerOnline, "the handler only keeps the notice");
  tick();
  CHECK(agreementState() == BOND_AGREEMENT_DECLINED && bondAgreementView().reason == BOND_NOTICE_ENDED,
        "the partner's ENDED declines the bond");
  CHECK(!gEspNow->bondPeerOnline && gEspNow->lastBondHeartbeatReceivedMs == 0 && !isBondSynced() &&
        posted(SYSEVT_BOND_OFFLINE) == 1, "the partner is offline at once, with one offline event (%zu)",
        posted(SYSEVT_BOND_OFFLINE));
  // What was synced with the partner goes too: it starts from nothing when it
  // bonds again, and until then the status must not show a synced partner.
  CHECK(!gEspNow->lastRemoteCapValid && !gEspNow->bondManifestReceived && !gEspNow->bondSettingsReceived &&
        !gEspNow->bondCapSent && gEspNow->bondSyncInFlight == BOND_SYNC_NONE,
        "the decline clears the sync state (cap=%d manifest=%d settings=%d capSent=%d inFlight=%d)",
        (int)gEspNow->lastRemoteCapValid, (int)gEspNow->bondManifestReceived, (int)gEspNow->bondSettingsReceived,
        (int)gEspNow->bondCapSent, (int)gEspNow->bondSyncInFlight);
  CHECK(gSettings.bondModeEnabled && gSettings.bondPeerMac == String(kMacA),
        "this device's bond setting is kept (mode=%d peer='%s')", (int)gSettings.bondModeEnabled,
        gSettings.bondPeerMac.c_str());
  CHECK(linesWith("02:11:22:33:44:01 ended the bond") == 1 && linesWith("bonddisconnect") == 1,
        "one line says the partner ended the bond and how to end it here");
  char text[160];
  bondAgreementDescribe(text, sizeof text);
  CHECK(std::strstr(text, "ended the bond") != nullptr, "status line: %s", text);

  // Five minutes with the session up: a request a minute and nothing else.
  const size_t frames = gFrames.size(), agree = gAgreeFrames.size();
  advance(5 * 60000);
  CHECK(requestsTo(kPeerA, agree) == 5 && gFrames.size() - frames == 5,
        "declined: %zu requests and %zu frames in all in 5 minutes (want 5 and 5: no heartbeat, no CAP_REQ)",
        requestsTo(kPeerA, agree), gFrames.size() - frames);
  const AgreeFrame* repeat = lastAgreeFrame(kPeerA, ESPNOW_V4_TYPE_BOND_REQUEST);
  CHECK(repeat && !(repeat->b1 & BOND_REQUEST_FLAG_FRESH), "the once-a-minute request is not marked fresh");
  // Its acknowledgement is watched: for a declined partner the request is the
  // only frame that can show the session died with a restart of the partner.
  // (The one request before the decline, the first on the session, is not.)
  CHECK(gWatchedSends.size() == requestsTo(kPeerA, agree) && gWatchedSends.size() == 5,
        "%zu of the %zu requests to a declined partner have their acknowledgement watched", gWatchedSends.size(),
        requestsTo(kPeerA, agree));
  CHECK(gSessionClears.empty() && posted(SYSEVT_BOND_OFFLINE) == 1 && !gEspNow->bondPeerOnline &&
        linesWith("ended the bond") == 1,
        "declined: no session cleared (%zu), no further offline event or line", gSessionClears.size());

  // The partner answers the requests that follow NOT_BONDED: the same news.
  // "Ended" stays the reason shown and nothing more is printed or posted.
  for (int i = 0; i < 3; ++i) {
    const AgreeFrame* rq = lastAgreeFrame(kPeerA, ESPNOW_V4_TYPE_BOND_REQUEST);
    rxNotice(kPeerA, BOND_NOTICE_NOT_BONDED, BOND_ROLE_WORKER, rq->seq);
    advance(60000);
  }
  CHECK(linesWith("has bond mode off") == 0 && linesWith("ended the bond") == 1 &&
        posted(SYSEVT_BOND_OFFLINE) == 1 && bondAgreementView().reason == BOND_NOTICE_ENDED,
        "NOT_BONDED answers after ENDED add no line (%zu lines in all)", gEventLines.size());

  // A session that comes up again does not make a declined partner online.
  // It still gets its auth-log record: the bond token of that session is
  // valid, and when the partner bonds again the bond goes live on it.
  const size_t online = posted(SYSEVT_BOND_ONLINE), auth = gAuthLines.size();
  bondNotifySessionEstablished(kPeerA);
  CHECK(!gEspNow->bondPeerOnline && posted(SYSEVT_BOND_ONLINE) == online && !gEspNow->bondNeedsCapabilityRequest,
        "the session alone does not bring a declined partner online");
  CHECK(gAuthLines.size() == auth + 1 &&
        authLinesWith("Bond session active (role=master, partner says it is not bonded)", auth) == 1,
        "a session with a declined partner is recorded in the auth log (%zu new records)",
        gAuthLines.size() - auth);

  // Nor does a heartbeat that was under way when it ended the bond; it is
  // asked instead, at most every 5 s.
  advance(10000);
  const size_t asked = gAgreeFrames.size();
  const uint32_t beatsUsed = gEspNow->bondHeartbeatsReceived;
  for (int i = 0; i < 20; ++i) {
    rxHeartbeat(kPeerA, BOND_ROLE_WORKER);
    advance(100);
  }
  CHECK(!gEspNow->bondPeerOnline && gEspNow->bondHeartbeatsReceived == beatsUsed,
        "a declined partner's heartbeat is not acted on");
  CHECK(requestsTo(kPeerA, asked) == 1, "20 heartbeats in 2 s from a declined partner bring one request (%zu)",
        requestsTo(kPeerA, asked));

  // The partner bonds again: its request agrees the bond, its heartbeat brings
  // it online, and the master starts its sync.
  rxRequest(kPeerA, BOND_ROLE_WORKER, BOND_REQUEST_FLAG_FRESH, 40);
  tick();
  CHECK(agreementState() == BOND_AGREEMENT_AGREED && noticesTo(kPeerA, BOND_NOTICE_ACCEPTED) == 1 &&
        linesWith("accepted the bond") == 2, "the partner's new request agrees the bond again, with its line");
  const size_t caps = framesOfType(kPeerA, ESPNOW_V4_TYPE_BOND_CAP_REQ);
  rxHeartbeat(kPeerA, BOND_ROLE_WORKER);
  advance(6000, [] { if (gNowMs % 5000 == 0) rxHeartbeat(kPeerA, BOND_ROLE_WORKER); });
  CHECK(gEspNow->bondPeerOnline && framesOfType(kPeerA, ESPNOW_V4_TYPE_BOND_CAP_REQ) > caps &&
        framesOfType(kPeerA, ESPNOW_V4_TYPE_BOND_HEARTBEAT) > 0,
        "after the new agreement the heartbeat and the capability request are back");
}

// The bench: only this device is bonded. With a partner that never answers
// the request (firmware without it) everything is as before: online from the
// session, capability requests, timeout, session cleared, again. With a
// partner that answers NOT_BONDED the round stops after its first step.
static void caseOneSidedBond() {
  // No answer at all.
  bondedToA(kMasterSelf, BOND_ROLE_MASTER);
  bondNotifySessionEstablished(kPeerA);
  size_t clearsSeen = 0;
  uint32_t clearedAt = 0;
  advance(120000, [&] {
    // The handshake comes back 5 s after the timeout cleared the session.
    if (gSessionClears.size() > clearsSeen) {
      clearsSeen = gSessionClears.size();
      clearedAt = gNowMs;
    }
    if (clearedAt != 0 && gNowMs - clearedAt >= 5000) {
      clearedAt = 0;
      bondNotifySessionEstablished(kPeerA);
    }
  });
  CHECK(gSessionClears.size() >= 5 && posted(SYSEVT_BOND_OFFLINE) >= 5 &&
        framesOfType(kPeerA, ESPNOW_V4_TYPE_BOND_CAP_REQ) >= 5 && framesOfType(kPeerA, ESPNOW_V4_TYPE_BOND_HEARTBEAT) >= 20,
        "no answer: the bond behaves as before (%zu session clears, %zu offline events, %zu CAP_REQ in 2 min)",
        gSessionClears.size(), posted(SYSEVT_BOND_OFFLINE), framesOfType(kPeerA, ESPNOW_V4_TYPE_BOND_CAP_REQ));
  CHECK(agreementState() == BOND_AGREEMENT_UNKNOWN && gEventLines.size() == linesWith("heartbeat timeout"),
        "no answer: the agreement stays unknown and nothing new is reported");
  CHECK(requestsTo(kPeerA) >= 6 && requestsTo(kPeerA) <= 9,
        "no answer: asked at each session start (about every 20 s), %zu times in 2 min", requestsTo(kPeerA));

  // The partner answers the first request: NOT_BONDED.
  bondedToA(kMasterSelf, BOND_ROLE_MASTER);
  const uint16_t seq = sessionUpAndRequest(kPeerA);
  CHECK(gEspNow->bondPeerOnline, "until the answer the session makes the partner online, as before");
  rxNotice(kPeerA, BOND_NOTICE_NOT_BONDED, BOND_ROLE_WORKER, seq);
  tick();
  const size_t frames = gFrames.size();
  advance(5 * 60000);
  CHECK(agreementState() == BOND_AGREEMENT_DECLINED && gSessionClears.empty() && posted(SYSEVT_BOND_OFFLINE) == 1,
        "NOT_BONDED: no session cleared in 5 minutes (%zu), one offline event (%zu)", gSessionClears.size(),
        posted(SYSEVT_BOND_OFFLINE));
  CHECK(gFrames.size() - frames == 5 && requestsTo(kPeerA) == 6 &&
        framesOfType(kPeerA, ESPNOW_V4_TYPE_BOND_CAP_REQ, frames) == 0 &&
        framesOfType(kPeerA, ESPNOW_V4_TYPE_BOND_HEARTBEAT, frames) == 0,
        "NOT_BONDED: %zu frames in 5 minutes, all of them the once-a-minute request", gFrames.size() - frames);
  CHECK(linesWith("has bond mode off") == 1 && gEventLines.size() == 1, "NOT_BONDED: one line says why (%zu)",
        gEventLines.size());
  CHECK(linesWith("Run 'bondconnect this-device' on it") == 1, "the line names this device for the other side");
  CHECK(gSettings.bondModeEnabled && gSettings.bondPeerMac == String(kMacA), "the bond setting is kept");

  // Declined and the session gone: the side that starts handshakes tries once
  // a minute (backed off by the handshake code); the other side waits.
  gSessionUp = false;
  const size_t sent = gFrames.size();
  advance(3 * 60000);
  CHECK(gFrames.size() == sent && gAutoStarts == 3,
        "declined without a session: nothing sent, %d handshake starts in 3 minutes (want 3)", gAutoStarts);
  bondedToA(kWorkerSelf, BOND_ROLE_WORKER);
  rxNotice(kPeerA, BOND_NOTICE_NOT_BONDED, BOND_ROLE_MASTER, 0);
  tick();
  gSessionUp = false;
  advance(3 * 60000);
  CHECK(agreementState() == BOND_AGREEMENT_DECLINED && gAutoStarts == 0 && gManualStarts == 0,
        "the side that answers handshakes starts none for a declined partner");

  // The session is lost while a request is waiting to go out (a late
  // heartbeat of the declined partner set it off, to be sent 5 s after the
  // last one). The once-a-minute handshake start must not wait for that
  // request: nothing else brings the session back.
  bondedToA(kMasterSelf, BOND_ROLE_MASTER);
  const uint16_t asked = sessionUpAndRequest(kPeerA);
  rxNotice(kPeerA, BOND_NOTICE_NOT_BONDED, BOND_ROLE_WORKER, asked);
  tick();
  rxHeartbeat(kPeerA, BOND_ROLE_WORKER);
  CHECK(agreementState() == BOND_AGREEMENT_DECLINED && gBondAgree.requestDue,
        "a declined partner's heartbeat sets a request off");
  gSessionUp = false;
  const size_t before = gFrames.size();
  advance(5 * 60000);
  CHECK(gFrames.size() == before && gAutoStarts >= 4 && gAutoStarts <= 5,
        "declined, a request waiting, no session: %d handshake starts in 5 minutes (want one a minute)",
        gAutoStarts);
  gSessionUp = true;
  bondNotifySessionEstablished(kPeerA);
  advance(1000);
  CHECK(requestsTo(kPeerA) == 2, "the partner is asked on the new session (%zu requests)", requestsTo(kPeerA));
}

// Both devices bonded to each other with the same role. Both learn it: this
// device answers the partner's request ROLE_CONFLICT and says so; the bond
// itself goes on as it does today.
static void caseRoleConflict() {
  bondedToA(kMasterSelf, BOND_ROLE_MASTER);
  const uint16_t seq = sessionUpAndRequest(kPeerA);
  rxRequest(kPeerA, BOND_ROLE_MASTER, BOND_REQUEST_FLAG_FRESH, 5);
  tick();
  const AgreeFrame* answer = lastAgreeFrame(kPeerA, ESPNOW_V4_TYPE_BOND_NOTICE);
  CHECK(answer && answer->b0 == BOND_NOTICE_ROLE_CONFLICT && answer->seq == 5,
        "a partner with our own role is answered ROLE_CONFLICT");
  CHECK(agreementState() == BOND_AGREEMENT_AGREED && bondAgreementView().roleConflict &&
        linesWith("are both bond master. Run 'bondrole worker' on one of them") == 1 &&
        linesWith("accepted the bond") == 0, "the conflict is reported once, with what to run");
  rxNotice(kPeerA, BOND_NOTICE_ROLE_CONFLICT, BOND_ROLE_MASTER, seq);
  tick();
  CHECK(bondAgreementView().roleConflict && linesWith("are both bond") == 1,
        "the partner's own ROLE_CONFLICT adds no second line");
  CHECK(gEspNow->bondPeerOnline && gSettings.bondRole == BOND_ROLE_MASTER,
        "the conflict changes neither the role nor the link");
  char text[160];
  bondAgreementDescribe(text, sizeof text);
  CHECK(std::strstr(text, "both are master") != nullptr, "status line: %s", text);

  // The partner's role is changed: its heartbeat carries the new one.
  rxHeartbeat(kPeerA, BOND_ROLE_WORKER);
  CHECK(!bondAgreementView().roleConflict && linesWith("different bond roles again") == 1,
        "a heartbeat with the other role ends the conflict, with a line");

  // A conflict that starts later is seen in the heartbeat, after three in a
  // row (a role swap changes one device a moment before the other).
  rxHeartbeat(kPeerA, BOND_ROLE_MASTER);
  rxHeartbeat(kPeerA, BOND_ROLE_MASTER);
  CHECK(!bondAgreementView().roleConflict, "two heartbeats with our role are not yet a conflict");
  rxHeartbeat(kPeerA, BOND_ROLE_WORKER);
  rxHeartbeat(kPeerA, BOND_ROLE_MASTER);
  rxHeartbeat(kPeerA, BOND_ROLE_MASTER);
  CHECK(!bondAgreementView().roleConflict, "the count starts over after a heartbeat with the other role");
  rxHeartbeat(kPeerA, BOND_ROLE_MASTER);
  CHECK(bondAgreementView().roleConflict && linesWith("are both bond master") == 2,
        "three heartbeats in a row with our role are a conflict");

  // The partner says ROLE_CONFLICT first (it saw our request before we saw its).
  bondedToA(kWorkerSelf, BOND_ROLE_WORKER);
  const uint16_t seq2 = sessionUpAndRequest(kPeerA);
  rxNotice(kPeerA, BOND_NOTICE_ROLE_CONFLICT, BOND_ROLE_WORKER, seq2);
  tick();
  CHECK(agreementState() == BOND_AGREEMENT_AGREED && bondAgreementView().roleConflict &&
        linesWith("are both bond worker. Run 'bondrole master' on one of them") == 1,
        "the partner's ROLE_CONFLICT is learnt and reported here too");
}

// How often a peer is told or asked.
static void caseRateLimits() {
  // A paired peer keeps sending bond frames to a device with bond mode off:
  // one NOT_BONDED notice a minute, whatever the number of frames.
  resetWorld(kWorkerSelf);
  for (int i = 0; i < 100; ++i) {
    bondAgreementNoteIgnoredFrame(kPeerB, true);
    advance(500);
  }
  CHECK(noticesTo(kPeerB, BOND_NOTICE_NOT_BONDED) == 1 && gAgreeFrames.size() == 1 && gAgreeFrames[0].seq == 0,
        "100 ignored bond frames in 50 s: one notice (%zu)", gAgreeFrames.size());
  advance(11000);
  bondAgreementNoteIgnoredFrame(kPeerB, true);
  tick();
  CHECK(noticesTo(kPeerB) == 2, "a minute after the first notice the next ignored frame brings another");
  CHECK(gEventLines.empty() && !gSettings.bondModeEnabled, "ignored frames print nothing here and change nothing");

  // Each peer has its own limit; a frame that did not come over the peer's
  // session earns no notice.
  bondAgreementNoteIgnoredFrame(kPeerC, true);
  bondAgreementNoteIgnoredFrame(kPeerA, false);
  tick();
  CHECK(noticesTo(kPeerC) == 1 && noticesTo(kPeerA) == 0 && noticesTo(kPeerB) == 2,
        "per peer, and only for session frames");

  // Bonded to A: B's ignored frames are answered BONDED_ELSEWHERE; a peer that
  // is the partner by the time the tick runs is owed nothing; an answer to a
  // request counts as that peer's notice for the minute.
  bondedToA(kMasterSelf, BOND_ROLE_MASTER);
  bondAgreementNoteIgnoredFrame(kPeerB, true);
  bondAgreementNoteIgnoredFrame(kPeerA, true);
  tick();
  CHECK(noticesTo(kPeerB, BOND_NOTICE_BONDED_ELSEWHERE) == 1 && noticesTo(kPeerA) == 0,
        "the notice says BONDED_ELSEWHERE; the partner gets none");
  CHECK(lastAgreeFrame(kPeerB, ESPNOW_V4_TYPE_BOND_NOTICE)->b1 == 0, "... and does not carry this device's role");
  resetWorld(kWorkerSelf);
  rxRequest(kPeerB, BOND_ROLE_MASTER, BOND_REQUEST_FLAG_FRESH, 2);
  tick();
  bondAgreementNoteIgnoredFrame(kPeerB, true);
  advance(5000);
  CHECK(noticesTo(kPeerB) == 1, "the answer to its request is the peer's notice for that minute (%zu)",
        noticesTo(kPeerB));

  // The send queue is full: the request is tried again 5 s later, not every tick.
  bondedToA(kMasterSelf, BOND_ROLE_MASTER);
  gTxQueueFull = true;
  bondNotifySessionEstablished(kPeerA);
  advance(4000);
  gTxQueueFull = false;
  CHECK(requestsTo(kPeerA) == 0 && gWatchedSends.empty(), "nothing was queued or watched while the queue was full");
  advance(2000);
  const AgreeFrame* rq = lastAgreeFrame(kPeerA, ESPNOW_V4_TYPE_BOND_REQUEST);
  CHECK(requestsTo(kPeerA) == 1 && rq && (rq->b1 & BOND_REQUEST_FLAG_FRESH),
        "the request went out on the retry, still marked fresh (%zu)", requestsTo(kPeerA));

  // No session yet: the request waits for it and goes out when it is there.
  bondedToA(kWorkerSelf, BOND_ROLE_WORKER);
  gSessionUp = false;
  cmd_bond_connect(String(kMacB));               // sets the partner: asks as soon as possible
  advance(30000);
  CHECK(requestsTo(kPeerB) == 0 && gAgreeFrames.empty(), "no request without a session");
  gSessionUp = true;
  bondNotifySessionEstablished(kPeerB);
  tick();
  // This device answered the handshake (the lower address): its session is up
  // before B has finished opening its own, and a request sent now would be
  // dropped there. It waits a second.
  advance(bondagree::kSessionSettleMs - 200);
  CHECK(requestsTo(kPeerB) == 0, "the side that answered the handshake does not ask in the first second");
  advance(200);
  CHECK(requestsTo(kPeerB) == 1 && gWatchedSends.empty(),
        "the request goes out when the other side has the session too (%zu), unwatched", requestsTo(kPeerB));
  advance(60000);
  CHECK(requestsTo(kPeerB) == 2 && gWatchedSends.size() == 1,
        "the repeat a minute later has its acknowledgement watched (%zu requests, %zu watched)",
        requestsTo(kPeerB), gWatchedSends.size());

  // More peers than the limit table has entries, each sending a bond frame
  // every 5 s for ten minutes: about one notice a minute per entry, not one
  // per frame.
  resetWorld(kWorkerSelf);
  uint32_t step = 0;
  advance(10 * 60000, [&] {
    step += 100;
    for (uint8_t i = 0; i < 7; ++i) {
      const uint8_t mac[6] = {0x02, 0x11, 0x22, 0x33, 0x55, i};
      if (step % 5000 == i * 500u) bondAgreementNoteIgnoredFrame(mac, true);
    }
  });
  CHECK(gAgreeFrames.size() >= 40 && gAgreeFrames.size() <= 51,
        "7 peers with ignored bond frames for 10 minutes: %zu notices (want about 50, not 840)",
        gAgreeFrames.size());
}

// A partner whose firmware has no BOND_REQUEST: it never answers, but its bond
// heartbeat (sent only to its own partner) stands for agreement, and the bond
// works as it always did.
static void caseNoAnswerPartner() {
  bondedToA(kMasterSelf, BOND_ROLE_MASTER);
  gMaster.mac = kPeerA;                          // A answers the bond sync, not the request
  bondNotifySessionEstablished(kPeerA);
  uint32_t beat = 0;
  const auto partnerBeats = [&] {
    if ((beat += 100) % 5000 == 0) rxHeartbeat(kPeerA, BOND_ROLE_WORKER);
  };
  advance(6000, partnerBeats);
  CHECK(agreementState() == BOND_AGREEMENT_AGREED && linesWith("accepted the bond") == 1,
        "the partner's heartbeat stands for its agreement");
  advance(5 * 60000, partnerBeats);
  CHECK(gEspNow->bondPeerOnline && gSessionClears.empty() && gEspNow->lastRemoteCapValid &&
        gEspNow->bondManifestReceived, "the bond syncs and stays up as before (cap=%d manifest=%d clears=%zu)",
        (int)gEspNow->lastRemoteCapValid, (int)gEspNow->bondManifestReceived, gSessionClears.size());
  CHECK(requestsTo(kPeerA) == 1, "one request at the session start, none after the heartbeat agreed (%zu)",
        requestsTo(kPeerA));

  // The partner goes silent and comes back: asked again at the new session,
  // agreed again by its heartbeat, without a second line.
  gSessionUp = false;                            // out of range: its session goes with the timeout
  advance(20000);
  CHECK(!gEspNow->bondPeerOnline && gSessionClears.size() == 1 && agreementState() == BOND_AGREEMENT_UNKNOWN,
        "after the heartbeat timeout the agreement is unknown again");
  gSessionUp = true;
  bondNotifySessionEstablished(kPeerA);
  advance(6000, partnerBeats);
  CHECK(agreementState() == BOND_AGREEMENT_AGREED && requestsTo(kPeerA) == 2 &&
        linesWith("accepted the bond") == 1, "back: asked once more, agreed, no second line");

  // Without any answer or heartbeat, and with the session up, the request is
  // repeated once a minute.
  bondedToA(kWorkerSelf, BOND_ROLE_WORKER);
  bondNotifySessionEstablished(kPeerA);
  gEspNow->bondPeerOnline = false;               // keeps the heartbeat timeout out of this
  advance(5 * 60000 + 1000);
  CHECK(requestsTo(kPeerA) == 6, "unanswered: asked at once and then once a minute (%zu in 5 min)",
        requestsTo(kPeerA));
}

// A partner change resets the agreement and asks the new partner. Nothing of
// it goes out while bond frames are held, and a request received during the
// hold is answered after it, from the new settings.
static void casePartnerChange() {
  bondedToA(kWorkerSelf, BOND_ROLE_WORKER);
  rxNotice(kPeerA, BOND_NOTICE_ENDED, BOND_ROLE_MASTER, 0);
  tick();
  CHECK(agreementState() == BOND_AGREEMENT_DECLINED, "declined by A");

  bool rxDuringHold = false;
  gPreempt = [&] {
    gNowMs += 10;
    if (bondPartnerChanging() && !rxDuringHold) {
      rxDuringHold = true;
      rxRequest(kPeerB, BOND_ROLE_MASTER, BOND_REQUEST_FLAG_FRESH, 21);   // B asks while we switch to it
    }
    tick();
  };
  cmd_bond_connect(String(kMacB));
  gPreempt = nullptr;
  CHECK(rxDuringHold, "the case did not reach the frame hold");
  size_t held = 0;
  for (const auto& f : gAgreeFrames) held += f.held;
  CHECK(held == 0, "%zu requests or notices went out while the partner change held bond frames", held);
  const AgreeFrame* ended = lastAgreeFrame(kPeerA, ESPNOW_V4_TYPE_BOND_NOTICE);
  // Numbered 0: this device never sent A a request, so the notice voids none.
  CHECK(noticesTo(kPeerA, BOND_NOTICE_ENDED) == 1 && ended && ended->direct && ended->seq == 0,
        "the partner bondconnect replaces is told ENDED by the command itself (%zu)",
        noticesTo(kPeerA, BOND_NOTICE_ENDED));
  tick();
  const AgreeFrame* answer = lastAgreeFrame(kPeerB, ESPNOW_V4_TYPE_BOND_NOTICE);
  CHECK(answer && answer->b0 == BOND_NOTICE_ACCEPTED && answer->seq == 21 && !answer->held,
        "B's request from during the change is answered ACCEPTED after it");
  CHECK(agreementState() == BOND_AGREEMENT_AGREED && requestsTo(kPeerB) >= 1,
        "the new partner was asked and its request agreed the bond");

  // The old partner's late notice is nobody's now.
  rxNotice(kPeerA, BOND_NOTICE_ENDED, BOND_ROLE_MASTER, 0);
  tick();
  CHECK(agreementState() == BOND_AGREEMENT_AGREED, "the old partner's notice no longer counts");

  // bonddisconnect: the agreement is gone with the bond, nothing more is asked.
  const size_t before = gAgreeFrames.size();
  cmd_bond_disconnect(String(""));
  CHECK(noticesTo(kPeerB, BOND_NOTICE_ENDED, before) == 1, "bonddisconnect tells the partner ENDED");
  advance(3 * 60000);
  CHECK(gAgreeFrames.size() == before + 1 && bondAgreementView().state == BOND_AGREEMENT_UNKNOWN,
        "after bonddisconnect nothing is asked (%zu frames)", gAgreeFrames.size() - before - 1);
}

// The partner's "not bonded" is handled on espnow_task while cmd_exec is in
// the middle of bondNotifySessionEstablished: before it looked at the
// agreement, after it set the partner online, just before the online event,
// or during the auth-log write (a blocking file append). Wherever it lands,
// the decline wins: the partner is not left online with a capability request
// due and a 15 s timeout to come, and an offline event never comes before
// the online event it belongs to.
static void caseDeclineWhileSessionComesUp() {
  bondedToA(kMasterSelf, BOND_ROLE_MASTER);
  gPreemptPoints = 0;
  bondNotifySessionEstablished(kPeerA);
  const int points = gPreemptPoints;
  CHECK(points >= 3 && gEspNow->bondPeerOnline, "the session brings the partner online (%d points for espnow_task)",
        points);

  bool duringWrite = false, beforeOnlineEvent = false;
  for (int at = 1; at <= points; ++at) {
    bondedToA(kMasterSelf, BOND_ROLE_MASTER);
    int n = 0;
    bool fired = false;
    gPreempt = [&] {
      if (++n != at) return;
      fired = true;
      duringWrite = duringWrite || gInAuthWrite;
      beforeOnlineEvent = beforeOnlineEvent || (gState.bondPeerOnline && posted(SYSEVT_BOND_ONLINE) == 0);
      rxNotice(kPeerA, BOND_NOTICE_NOT_BONDED, BOND_ROLE_WORKER, 0);
      tick();
    };
    bondNotifySessionEstablished(kPeerA);
    gPreempt = nullptr;
    CHECK(fired, "point %d of %d was not reached", at, points);
    CHECK(agreementState() == BOND_AGREEMENT_DECLINED && !gState.bondPeerOnline &&
          !gState.bondNeedsCapabilityRequest && gState.lastBondHeartbeatReceivedMs == 0,
          "a decline at point %d of the session coming up left the partner online=%d capRequest=%d lastBeat=%lu",
          at, (int)gState.bondPeerOnline, (int)gState.bondNeedsCapabilityRequest,
          (unsigned long)gState.lastBondHeartbeatReceivedMs);
    // Every offline event has its online event before it, and the last word
    // is not "online".
    int open = 0;
    bool offlineFirst = false;
    for (const auto& e : gPosted) {
      if (e.type == SYSEVT_BOND_ONLINE) ++open;
      if (e.type == SYSEVT_BOND_OFFLINE && --open < 0) offlineFirst = true;
    }
    CHECK(!offlineFirst && open == 0,
          "a decline at point %d: %zu online and %zu offline events, offline first: %d", at,
          posted(SYSEVT_BOND_ONLINE), posted(SYSEVT_BOND_OFFLINE), (int)offlineFirst);
    CHECK(gAuthLines.size() == 1, "the session has its one auth-log record (%zu)", gAuthLines.size());
    // The 20 s after: nothing of the old round (capability request, heartbeat,
    // timeout, session cleared).
    const size_t frames = gFrames.size();
    advance(20000);
    CHECK(gSessionClears.empty() && linesWith("heartbeat timeout") == 0 &&
          framesOfType(kPeerA, ESPNOW_V4_TYPE_BOND_CAP_REQ) == 0 &&
          framesOfType(kPeerA, ESPNOW_V4_TYPE_BOND_HEARTBEAT, frames) == 0 && !gState.bondPeerOnline,
          "a decline at point %d: the old round went on (%zu session clears, %zu timeout lines, %zu CAP_REQ)", at,
          gSessionClears.size(), linesWith("heartbeat timeout"), framesOfType(kPeerA, ESPNOW_V4_TYPE_BOND_CAP_REQ));
  }
  CHECK(duringWrite && beforeOnlineEvent,
        "the case did not reach the auth-log write (%d) or the moment before the online event (%d)",
        (int)duringWrite, (int)beforeOnlineEvent);

  // The worker side has no capability request to leave behind, but the same
  // online state.
  bondedToA(kWorkerSelf, BOND_ROLE_WORKER);
  gPreempt = [&] {
    if (!gInAuthWrite) return;
    rxNotice(kPeerA, BOND_NOTICE_BONDED_ELSEWHERE, BOND_ROLE_MASTER, 0);
    tick();
  };
  bondNotifySessionEstablished(kPeerA);
  gPreempt = nullptr;
  CHECK(agreementState() == BOND_AGREEMENT_DECLINED && !gState.bondPeerOnline &&
        posted(SYSEVT_BOND_ONLINE) == 1 && posted(SYSEVT_BOND_OFFLINE) == 1 &&
        gPosted.back().type == SYSEVT_BOND_OFFLINE, "worker: a decline during the auth-log write wins");
}

// A request goes through the send queue; ENDED is sent by the command itself.
// The partner's request from just before it ended the bond arrives after its
// ENDED. It must not agree the bond again ("ended the bond", then "accepted
// the bond", heartbeats until the next NOT_BONDED): the ENDED names the last
// request the partner made, and requests up to that number are not used.
static void caseEndedVoidsOlderRequest() {
  for (int sameTick = 0; sameTick < 2; ++sameTick) {
    bondedToA(kMasterSelf, BOND_ROLE_MASTER);
    sessionUpAndRequest(kPeerA);
    rxRequest(kPeerA, BOND_ROLE_WORKER, BOND_REQUEST_FLAG_FRESH, 7);
    rxHeartbeat(kPeerA, BOND_ROLE_WORKER);
    tick();
    CHECK(agreementState() == BOND_AGREEMENT_AGREED && gEspNow->bondPeerOnline && linesWith("accepted the bond") == 1,
          "bonded and agreed");
    // A made request 8, then ended the bond: ENDED (naming 8) arrives first.
    // The number has nothing to do with the requests this device sent.
    rxNotice(kPeerA, BOND_NOTICE_ENDED, BOND_ROLE_WORKER, 8);
    if (!sameTick) tick();
    const size_t notices = noticesTo(kPeerA), frames = gFrames.size();
    rxRequest(kPeerA, BOND_ROLE_WORKER, 0, 8);
    tick();
    CHECK(agreementState() == BOND_AGREEMENT_DECLINED && bondAgreementView().reason == BOND_NOTICE_ENDED &&
          !gEspNow->bondPeerOnline, "the request from before the ENDED does not agree the bond again (same tick: %d)",
          sameTick);
    CHECK(linesWith("ended the bond") == 1 && linesWith("accepted the bond") == 1 && noticesTo(kPeerA) == notices &&
          gBondAgree.requestsVoided == 1,
          "... is not answered and prints nothing (%zu 'accepted' lines, %zu answers)",
          linesWith("accepted the bond"), noticesTo(kPeerA) - notices);
    advance(30000);
    CHECK(framesOfType(kPeerA, ESPNOW_V4_TYPE_BOND_HEARTBEAT, frames) == 0 && posted(SYSEVT_BOND_OFFLINE) == 1 &&
          posted(SYSEVT_BOND_ONLINE) == 1, "... and brings no heartbeat or online event back");
    // An older repeat is void too; the partner's next request (it bonded
    // again) is newer and counts.
    rxRequest(kPeerA, BOND_ROLE_WORKER, 0, 6);
    tick();
    CHECK(agreementState() == BOND_AGREEMENT_DECLINED && gBondAgree.requestsVoided == 2, "an older request is void too");
    rxRequest(kPeerA, BOND_ROLE_WORKER, BOND_REQUEST_FLAG_FRESH, 9);
    tick();
    const AgreeFrame* answer = lastAgreeFrame(kPeerA, ESPNOW_V4_TYPE_BOND_NOTICE);
    CHECK(agreementState() == BOND_AGREEMENT_AGREED && answer && answer->b0 == BOND_NOTICE_ACCEPTED &&
          answer->seq == 9 && linesWith("accepted the bond") == 2,
          "the partner's request from after its ENDED agrees the bond again");
  }

  // A partner that restarted after its ENDED counts from 1 again. It reaches
  // this device over a new session, which forgets the old number.
  bondedToA(kMasterSelf, BOND_ROLE_MASTER);
  sessionUpAndRequest(kPeerA);
  rxNotice(kPeerA, BOND_NOTICE_ENDED, BOND_ROLE_WORKER, 300);
  tick();
  rxRequest(kPeerA, BOND_ROLE_WORKER, BOND_REQUEST_FLAG_FRESH, 1);
  tick();
  CHECK(agreementState() == BOND_AGREEMENT_DECLINED, "on the same session request 1 is older than 300");
  bondNotifySessionEstablished(kPeerA);
  rxRequest(kPeerA, BOND_ROLE_WORKER, BOND_REQUEST_FLAG_FRESH, 1);
  tick();
  CHECK(agreementState() == BOND_AGREEMENT_AGREED, "on a new session the restarted partner's request 1 counts");

  // This device ends the bond: its ENDED names the last request it made.
  bondedToA(kMasterSelf, BOND_ROLE_MASTER);
  const uint16_t asked = sessionUpAndRequest(kPeerA);
  rxNotice(kPeerA, BOND_NOTICE_NOT_BONDED, BOND_ROLE_WORKER, asked);
  tick();
  advance(10000);
  const uint16_t last = lastAgreeFrame(kPeerA, ESPNOW_V4_TYPE_BOND_REQUEST)->seq;
  // A's late heartbeat sets a request off in the moment between the ENDED and
  // the settings change. That request would be newer than the number in the
  // ENDED and would not be void: it is not made.
  bool inWindow = false;
  gPreempt = [&] {
    if (inWindow || stepAt("directSend") == SIZE_MAX || bondPartnerChanging()) return;
    inWindow = true;
    rxHeartbeat(kPeerA, BOND_ROLE_WORKER);
    tick();
  };
  cmd_bond_disconnect(String(""));
  gPreempt = nullptr;
  const AgreeFrame* ended = lastAgreeFrame(kPeerA, ESPNOW_V4_TYPE_BOND_NOTICE);
  CHECK(inWindow, "the case did not reach the moment after the ENDED");
  CHECK(last != 0 && ended && ended->b0 == BOND_NOTICE_ENDED && ended->direct && ended->seq == last,
        "ENDED names the last request made (%u, want %u)", ended ? (unsigned)ended->seq : 0u, (unsigned)last);
  CHECK(&gAgreeFrames.back() == ended && lastAgreeFrame(kPeerA, ESPNOW_V4_TYPE_BOND_REQUEST)->seq == last,
        "no request was made after the ENDED");
  advance(3 * 60000);
  CHECK(&gAgreeFrames.back() == lastAgreeFrame(kPeerA, ESPNOW_V4_TYPE_BOND_NOTICE), "... nor in the minutes after");
}

// 'bondconnect' for the partner this device already has. It used to answer
// "Already connected" also when that partner had declined or never answered,
// and left the next question to the once-a-minute request. Now it says how the
// bond stands and asks at once, at most every 5 s.
static void caseConnectAgainAsksAgain() {
  bondedToA(kMasterSelf, BOND_ROLE_MASTER);
  const uint16_t asked = sessionUpAndRequest(kPeerA);
  rxNotice(kPeerA, BOND_NOTICE_NOT_BONDED, BOND_ROLE_WORKER, asked);
  tick();
  advance(20000);                                // the next once-a-minute request is 40 s away
  const size_t before = requestsTo(kPeerA), frames = gAgreeFrames.size();
  const char* reply = cmd_bond_connect(String(kMacA));
  CHECK(std::strstr(reply, "Already connected") == nullptr && std::strstr(reply, "is already set to") &&
        std::strstr(reply, kMacA) && std::strstr(reply, "but it has bond mode off") &&
        std::strstr(reply, "It is being asked again now.") && std::strstr(reply, "'bondconnect this-device' on it"),
        "bondconnect to a partner that declined says so and that it is asked: %s", reply);
  tick();
  const AgreeFrame* rq = lastAgreeFrame(kPeerA, ESPNOW_V4_TYPE_BOND_REQUEST);
  CHECK(requestsTo(kPeerA) == before + 1 && rq && (rq->b1 & BOND_REQUEST_FLAG_FRESH) && rq->seq == asked + 1,
        "... and the request goes out on the next turn (%zu new)", requestsTo(kPeerA) - before);
  CHECK(noticesTo(kPeerA, BOND_NOTICE_ENDED) == 0 && gAgreeFrames.size() == frames + 1 &&
        agreementState() == BOND_AGREEMENT_DECLINED && gSettings.bondModeEnabled &&
        gSettings.bondPeerMac == String(kMacA), "... with nothing else sent or changed");
  // Again right away: not sooner than 5 s after the last request.
  reply = cmd_bond_connect(String(kMacA));
  tick();
  CHECK(requestsTo(kPeerA) == before + 1 && std::strstr(reply, "is asked again in 5 s."),
        "a second bondconnect at once does not send a second request: %s", reply);
  advance(4900);
  CHECK(requestsTo(kPeerA) == before + 1, "... not before the 5 s are over");
  advance(100);
  CHECK(requestsTo(kPeerA) == before + 2, "... and then it does (%zu)", requestsTo(kPeerA) - before);
  // The partner's reason is the one shown.
  rxNotice(kPeerA, BOND_NOTICE_BONDED_ELSEWHERE, BOND_ROLE_WORKER,
           lastAgreeFrame(kPeerA, ESPNOW_V4_TYPE_BOND_REQUEST)->seq);
  tick();
  reply = cmd_bond_connect(String(kMacA));
  CHECK(std::strstr(reply, "but it is bonded to another device"), "the reply gives the partner's reason: %s", reply);

  // No answer yet, and no session: said as it is, nothing sent, and the side
  // that starts handshakes starts one.
  bondedToA(kMasterSelf, BOND_ROLE_MASTER);
  gSessionUp = false;
  reply = cmd_bond_connect(String(kMacA));
  tick();
  CHECK(std::strstr(reply, "but it has not answered yet") &&
        std::strstr(reply, "It is asked again as soon as it can be reached.") && gAgreeFrames.empty(),
        "bondconnect again without an answer or a session: %s", reply);
  gSessionUp = true;
  bondNotifySessionEstablished(kPeerA);
  tick();
  CHECK(requestsTo(kPeerA) == 1, "... and the partner is asked when the session is there (%zu)", requestsTo(kPeerA));

  // Agreed, with the session up: the bond runs (an Error under the command
  // reply contract: starting what runs), and nothing is asked.
  rxRequest(kPeerA, BOND_ROLE_WORKER, BOND_REQUEST_FLAG_FRESH, 4);
  tick();
  const size_t agreedFrames = gAgreeFrames.size();
  reply = cmd_bond_connect(String(kMacA));
  advance(10000);
  CHECK(std::strcmp(reply, "Error: The bond with this device is already running. Refresh it with 'bondresync'.") == 0 &&
        requestsTo(kPeerA) == 1 && gAgreeFrames.size() == agreedFrames,
        "bondconnect to an agreed partner: %s", reply);
}

// The agreement in JSON is the same text wherever it appears (bondstatus
// json, /api/bond/status, the system status): these two functions.
static void caseJsonText() {
  bondedToA(kMasterSelf, BOND_ROLE_MASTER);
  BondAgreementView view = bondAgreementView();
  CHECK(std::strcmp(bondAgreementKey(view), "unknown") == 0 && std::strcmp(bondAgreementReasonKey(view), "") == 0,
        "no answer yet: %s / %s", bondAgreementKey(view), bondAgreementReasonKey(view));
  const uint16_t seq = sessionUpAndRequest(kPeerA);
  rxNotice(kPeerA, BOND_NOTICE_ROLE_CONFLICT, BOND_ROLE_MASTER, seq);
  tick();
  view = bondAgreementView();
  CHECK(std::strcmp(bondAgreementKey(view), "agreed") == 0 &&
        std::strcmp(bondAgreementReasonKey(view), "role_conflict") == 0, "same role: %s / %s",
        bondAgreementKey(view), bondAgreementReasonKey(view));
  rxHeartbeat(kPeerA, BOND_ROLE_WORKER);
  view = bondAgreementView();
  CHECK(std::strcmp(bondAgreementKey(view), "agreed") == 0 && std::strcmp(bondAgreementReasonKey(view), "") == 0,
        "agreed: %s / %s", bondAgreementKey(view), bondAgreementReasonKey(view));
  const char* const reasons[] = {"not_bonded", "bonded_elsewhere", "ended"};
  const uint8_t codes[] = {BOND_NOTICE_NOT_BONDED, BOND_NOTICE_BONDED_ELSEWHERE, BOND_NOTICE_ENDED};
  for (int i = 0; i < 3; ++i) {
    bondedToA(kMasterSelf, BOND_ROLE_MASTER);
    rxNotice(kPeerA, codes[i], BOND_ROLE_WORKER, 0);
    tick();
    view = bondAgreementView();
    CHECK(std::strcmp(bondAgreementKey(view), "declined") == 0 &&
          std::strcmp(bondAgreementReasonKey(view), reasons[i]) == 0, "declined: %s / %s (want %s)",
          bondAgreementKey(view), bondAgreementReasonKey(view), reasons[i]);
  }
  resetWorld(kWorkerSelf);                       // no bond: nothing to report
  view = bondAgreementView();
  CHECK(std::strcmp(bondAgreementKey(view), "unknown") == 0 && std::strcmp(bondAgreementReasonKey(view), "") == 0,
        "no bond: %s / %s", bondAgreementKey(view), bondAgreementReasonKey(view));
}


// The role. bondconnect keeps a role the user chose (the addresses alone would
// give the other); the request and the answer say "chosen". A partner whose
// role is automatic and the same is not reported as a conflict: it takes the
// other role on that answer. This device, automatic, takes the other role than
// a partner that chose, from its request or its notice, answers with the new
// role, and asks again; bondconnect to a peer that asked earlier uses what it
// said.
static void caseRoleChoice() {
  // Chosen master on the lower address: kept by bondconnect.
  resetWorld(kWorkerSelf);
  gMaster.mac = kPeerC;
  gSettings.bondRole = BOND_ROLE_MASTER;
  gSettings.bondRoleChosen = true;
  std::string reply = cmd_bond_connect(String(kMacA));
  CHECK(gSettings.bondModeEnabled && gSettings.bondRole == BOND_ROLE_MASTER && gSettings.bondRoleChosen,
        "bondconnect keeps the role the user chose (role %u)", (unsigned)gSettings.bondRole);
  CHECK(reply.find("Role: master (chosen with 'bondrole', kept)") != std::string::npos, "reply: %s", reply.c_str());
  sessionUpAndRequest(kPeerA);
  const AgreeFrame* rq = lastAgreeFrame(kPeerA, ESPNOW_V4_TYPE_BOND_REQUEST);
  CHECK(rq && rq->b0 == BOND_ROLE_MASTER && (rq->b1 & BOND_REQUEST_FLAG_ROLE_CHOSEN) &&
        (rq->b1 & BOND_REQUEST_FLAG_ROLE_YIELDS), "the request carries the role, the chosen flag and 'I yield'");
  // A, automatic and master too, asks: answered ACCEPTED as master, "chosen".
  rxRequest(kPeerA, BOND_ROLE_MASTER, BOND_REQUEST_FLAG_FRESH | BOND_REQUEST_FLAG_ROLE_YIELDS, 9);
  tick();
  const AgreeFrame* answer = lastAgreeFrame(kPeerA, ESPNOW_V4_TYPE_BOND_NOTICE);
  CHECK(answer && answer->b0 == BOND_NOTICE_ACCEPTED && answer->b1 == BOND_ROLE_MASTER &&
        (answer->b4 & BOND_NOTICE_FLAG_ROLE_CHOSEN) && answer->seq == 9,
        "a partner whose role is automatic is answered ACCEPTED with our chosen role");
  CHECK(!bondAgreementView().roleConflict && linesWith("are both bond") == 0 && gAutoRoleCalls == 0 &&
        gSettings.bondRole == BOND_ROLE_MASTER, "no conflict is reported: the partner takes the other role");

  // The same partner on firmware from before role choice (no 'I yield'): it
  // keeps its role, so the conflict is reported at once, as before.
  rxRequest(kPeerA, BOND_ROLE_MASTER, BOND_REQUEST_FLAG_FRESH, 10);
  tick();
  answer = lastAgreeFrame(kPeerA, ESPNOW_V4_TYPE_BOND_NOTICE);
  CHECK(answer && answer->b0 == BOND_NOTICE_ROLE_CONFLICT && answer->seq == 10 && bondAgreementView().roleConflict &&
        linesWith("are both bond") == 1 && gSettings.bondRole == BOND_ROLE_MASTER,
        "an older partner with the same role: ROLE_CONFLICT at once");

  // Neither chose: the addresses decide, as before (the higher is master).
  resetWorld(kWorkerSelf);
  gMaster.mac = kPeerC;
  gSettings.bondRole = BOND_ROLE_MASTER;
  reply = cmd_bond_connect(String(kMacA));
  CHECK(gSettings.bondRole == BOND_ROLE_WORKER && reply.find("automatic: this device's address is the lower") !=
        std::string::npos, "neither chose: the lower address is worker (%s)", reply.c_str());

  // Automatic on the higher address (master by the addresses); A chose master:
  // this device takes worker and answers as worker, not "chosen".
  bondedToA(kMasterSelf, BOND_ROLE_MASTER);
  sessionUpAndRequest(kPeerA);
  rq = lastAgreeFrame(kPeerA, ESPNOW_V4_TYPE_BOND_REQUEST);
  CHECK(rq && !(rq->b1 & BOND_REQUEST_FLAG_ROLE_CHOSEN), "an automatic role is sent without the chosen flag");
  rxRequest(kPeerA, BOND_ROLE_MASTER, BOND_REQUEST_FLAG_FRESH | BOND_REQUEST_FLAG_ROLE_CHOSEN, 11);
  tick();
  answer = lastAgreeFrame(kPeerA, ESPNOW_V4_TYPE_BOND_NOTICE);
  CHECK(gAutoRoleCalls == 1 && gAutoRoleTo == BOND_ROLE_WORKER && gSettings.bondRole == BOND_ROLE_WORKER,
        "an automatic role yields to the partner's choice (%d calls)", gAutoRoleCalls);
  CHECK(answer && answer->b0 == BOND_NOTICE_ACCEPTED && answer->b1 == BOND_ROLE_WORKER &&
        !(answer->b4 & BOND_NOTICE_FLAG_ROLE_CHOSEN), "the answer states the new role");
  CHECK(!bondAgreementView().roleConflict && linesWith("are both bond") == 0, "no conflict is reported");
  // Once yielded, the partner's next request changes nothing.
  rxRequest(kPeerA, BOND_ROLE_MASTER, BOND_REQUEST_FLAG_ROLE_CHOSEN, 12);
  tick();
  CHECK(gAutoRoleCalls == 1 && gSettings.bondRole == BOND_ROLE_WORKER, "nothing more to yield");
  char words[200];
  bondRoleWords(words, sizeof(words));
  CHECK(std::strstr(words, "worker (automatic: ") != nullptr && std::strstr(words, "chose master") != nullptr,
        "role words: %s", words);
  // A gives its choice up ('bondrole auto'): this device stays worker, and
  // says the role was kept, not that the addresses gave it (they give master).
  rxRequest(kPeerA, BOND_ROLE_MASTER, BOND_REQUEST_FLAG_ROLE_YIELDS, 20);
  tick();
  bondRoleWords(words, sizeof(words));
  CHECK(gSettings.bondRole == BOND_ROLE_WORKER && std::strstr(words, "worker (automatic: kept as it was set earlier") !=
        nullptr, "role words: %s", words);

  // The same from A's notice (its request was lost): yield, then ask again.
  bondedToA(kMasterSelf, BOND_ROLE_MASTER);
  const uint16_t seq = sessionUpAndRequest(kPeerA);
  const size_t asked = requestsTo(kPeerA);
  rxNotice(kPeerA, BOND_NOTICE_ROLE_CONFLICT, BOND_ROLE_MASTER, seq, BOND_NOTICE_FLAG_ROLE_CHOSEN);
  tick();
  CHECK(gAutoRoleCalls == 1 && gSettings.bondRole == BOND_ROLE_WORKER && !bondAgreementView().roleConflict,
        "an automatic role yields to the choice a notice states");
  advance(bondagree::kRequestRetryMs + 200);
  rq = lastAgreeFrame(kPeerA, ESPNOW_V4_TYPE_BOND_REQUEST);
  CHECK(requestsTo(kPeerA) == asked + 1 && rq && rq->b0 == BOND_ROLE_WORKER,
        "the partner is asked again with the new role (%zu requests)", requestsTo(kPeerA) - asked);

  // A chosen role does not yield: both chose master is a conflict, reported.
  bondedToA(kMasterSelf, BOND_ROLE_MASTER);
  gSettings.bondRoleChosen = true;
  sessionUpAndRequest(kPeerA);
  rxRequest(kPeerA, BOND_ROLE_MASTER, BOND_REQUEST_FLAG_FRESH | BOND_REQUEST_FLAG_ROLE_CHOSEN, 13);
  tick();
  answer = lastAgreeFrame(kPeerA, ESPNOW_V4_TYPE_BOND_NOTICE);
  CHECK(gAutoRoleCalls == 0 && gSettings.bondRole == BOND_ROLE_MASTER && answer &&
        answer->b0 == BOND_NOTICE_ROLE_CONFLICT && bondAgreementView().roleConflict && linesWith("are both bond") == 1,
        "both chose the same role: ROLE_CONFLICT, as before");

  // bondconnect to a peer that asked before: the other role than the one its
  // user chose (the addresses alone would make this lower address worker).
  resetWorld(kWorkerSelf);
  gMaster.mac = kPeerC;
  rxRequest(kPeerA, BOND_ROLE_WORKER, BOND_REQUEST_FLAG_FRESH | BOND_REQUEST_FLAG_ROLE_CHOSEN, 3);
  tick();
  reply = cmd_bond_connect(String(kMacA));
  CHECK(gSettings.bondRole == BOND_ROLE_MASTER && reply.find("chose worker") != std::string::npos,
        "bondconnect takes the other role than the one the peer's user chose (%s)", reply.c_str());
  // The bond status reply says how the role came about.
  bondRoleWords(words, sizeof(words));
  CHECK(std::strstr(words, "master (automatic: ") != nullptr, "role words: %s", words);
  gSettings.bondRoleChosen = true;
  bondRoleWords(words, sizeof(words));
  CHECK(std::strstr(words, "master (chosen with 'bondrole'") != nullptr, "role words: %s", words);
}
int main() {
  caseMutual();
  caseRequestFromNonPartner();
  caseNoticeFromNonPartner();
  caseEndedStopsTheCycle();
  caseOneSidedBond();
  caseRoleConflict();
  caseRateLimits();
  caseNoAnswerPartner();
  casePartnerChange();
  caseDeclineWhileSessionComesUp();
  caseEndedVoidsOlderRequest();
  caseConnectAgainAsksAgain();
  caseJsonText();
  caseRoleChoice();
  std::printf("espnow_bond_agreement: %d checks, %d failed\n", gChecks, gFailures);
  return gFailures ? 1 : 0;
}
