#include "G2_ConversateSession.h"
#include <assert.h>
#include <stdio.h>
#include <vector>
#include <string>

static std::vector<uint8_t> unhex(const char* hex) {
  std::vector<uint8_t> out;
  for (size_t i = 0; i < strlen(hex); i += 2)
    out.push_back(uint8_t(std::stoul(std::string(hex + i, 2), nullptr, 16)));
  return out;
}

static void wire(const uint8_t* frame, size_t len, const char* expectedHex) {
  const auto expected = unhex(expectedHex);
  assert(len == expected.size() + 10);
  assert(frame[0] == 0xaa && frame[1] == 0x21 && frame[2] == 42);
  assert(frame[3] == expected.size() + 2 && frame[4] == 1 && frame[5] == 1);
  assert(frame[6] == 11 && frame[7] == 0x20);
  assert(memcmp(frame + 8, expected.data(), expected.size()) == 0);
  G2EnvelopeView view{};
  assert(g2ParseEnvelope(frame, len, &view));
}

int main() {
  ownerTests();
  rxQueueTests();
  // Non-personal protocol bytes from the 2026-09-20 stock phone session.
  uint8_t frame[256];
  wire(frame, g2BuildConversateHeartbeat(42, 88, frame, sizeof(frame)), "08ff0110585a00");
  wire(frame, g2BuildConversateHeartbeat(42, 130, frame, sizeof(frame)), "08ff011082015a00");
  wire(frame, g2BuildConversateControl(42, 132, false, frame, sizeof(frame)), "08011084011a0408022000");
  wire(frame, g2BuildConversatePrep(42, 84, frame, sizeof(frame)),
       "080310542a1910001a15080110011a0a0a034f464612034f666622034f4646");
  wire(frame, g2BuildConversateControl(42, 85, true, frame, sizeof(frame)),
       "080110551a210801120a0801100118002001280020002a0a0a034f464612034f666632034f4646");
  wire(frame, g2BuildConversateInterfaceReply(42,135,0,false,true,frame,sizeof(frame)),
       "08a5011087017a06080010001801");
  wire(frame, g2BuildConversateInterfaceReply(42,140,0,true,false,frame,sizeof(frame)),
       "08a501108c017a06080010011800");
  wire(frame, g2BuildConversateLanguageReply(42,145,0,frame,sizeof(frame)),
       "08a7011091018a01020800");
  wire(frame, g2BuildConversateLanguageReply(42,147,1,frame,sizeof(frame)),
       "08a7011093018a01020801");
  wire(frame, g2BuildConversatePauseResume(42,150,false,frame,sizeof(frame)),
       "08011096011a0408032000");
  wire(frame, g2BuildConversatePauseResume(42,151,true,frame,sizeof(frame)),
       "08011097011a0408042000");
  assert(!g2BuildConversateHeartbeat(0, 1, nullptr, 256));
  assert(!g2BuildConversateControl(0, 1, true, frame, 20));
  assert(!g2BuildConversatePrep(0, 1, frame, 20));
  for (const char* hex : {"080210012200", "0804105532020801",
                          "08a20110545200", "08a1011084014a020802",
                          "08a40110870172021001", "08a401108c0172020801",
                          "08a6011091018201060a044155544f", "08a8011001920100"}) {
    const auto pb = unhex(hex);
    G2ConversateEvent event;
    assert(g2ParseConversateEvent(pb.data(), pb.size(), &event));
    for (size_t n = 0; n < pb.size(); ++n)
      assert(!g2ParseConversateEvent(pb.data(), n, &event));
  }
  for (const char* hex : {"08041055320208", "080410553200", "08041055320208010804",
                          "0802100122001002", "080210010a00", "08021001220100",
                          "08a4011001720408010800", "08a4011001720410011000",
                          "08a401100172021201", "08a6011001820100",
                          "08a60110018201060a044100544f", "08a40110017200820100",
                          "08a60110018201020801", "08a8011001820100"}) {
    const auto pb = unhex(hex); G2ConversateEvent event;
    assert(!g2ParseConversateEvent(pb.data(), pb.size(), &event));
    assert(event.command == 0);
  }
  using S = G2ConversateSession;
  S s;
  assert(!s.arm(0, 9, 1, 2));
  assert(!s.arm(0, 3601, 1, 2));
  assert(!s.arm(0, 600, 0, 2));
  assert(s.arm(0, 600, 1, 2));
  assert(!s.start(0)); // SELECT requires successful preparation
  s.prepared = true;
  assert(s.start(0));
  assert(!s.start(1));
  assert(!s.arm(1, 300, 1, 2));
  for (uint32_t now = 0; now < 600000; now += 50) {
    s.audio(now, 1, uint8_t(now / 50));
    if (s.heartbeatDue(now)) {
      s.heartbeatSent(now, now / 5000);
      assert(!s.acknowledge(123456));
      assert(s.acknowledge(now / 5000));
      assert(!s.acknowledge(now / 5000));
    }
    assert(s.check(now, true, false) == S::Stop::None);
  }
  assert(s.packets == 12000 && s.heartbeats == 119 && s.acknowledgements == 119);
  assert(s.maxGapMs == 50 && s.lostPackets == 0 && s.duplicates == 0);
  assert(s.check(600000, true, false) == S::Stop::Deadline);
  assert(s.check(599950, false, false) == S::Stop::Disconnected);
  assert(s.check(599950, true, true) == S::Stop::Busy);
  s.stop(S::Stop::NativeExit);
  s.audio(600000, 1, 0);
  assert(s.packets == 12000 && !s.acknowledge(119) && !s.heartbeatDue(700000));
  assert(s.arm(UINT32_MAX - 2000, 300, 4, 5));
  s.prepared = true; assert(s.start(UINT32_MAX - 2000));
  assert(!s.heartbeatDue(2998) && s.heartbeatDue(2999));
  assert(s.waitMs(2900) == 99 && s.waitMs(2999) == 1);
  s.audio(2999, 3, 0); assert(s.packets == 0); // wrong connection
  s.audio(2999, 4, 254);
  s.audio(3049, 4, 0); // one missing packet at sequence wrap
  s.audio(3099, 4, 0); // duplicate
  assert(s.lostPackets == 1 && s.duplicates == 1);
  s.heartbeatSent(2999, 300);
  s.audio(15000, 4, 1);
  assert(s.check(15000, true, false) == S::Stop::None);
  assert(!s.acknowledge(300) && s.ackTimeouts == 1 && s.consecutiveFailures == 1);
  assert(s.check(22999, true, false) == S::Stop::None);
  assert(s.check(23000, true, false) == S::Stop::AudioStall);
  s.stop(S::Stop::User);
  assert(s.arm(0, 300, 1, 2));
  assert(s.check(60000, true, false) == S::Stop::ArmTimeout);
  s.prepared = true; assert(s.start(60001));
  assert(s.check(68001, true, false) == S::Stop::AudioStall); // no first packet
  // Parser defaults are proto3 zero, not sticky previous settings.
  auto off = unhex("08a40110870172021001"); G2ConversateEvent event;
  assert(g2ParseConversateEvent(off.data(),off.size(),&event));
  assert(!event.hasValue && event.transcribe == 0 && event.aiCue == 1);
  assert(!g2BuildConversateInterfaceReply(1,1,0,true,true,nullptr,64));
  assert(!g2BuildConversateLanguageReply(1,1,0,frame,4));
  assert(!g2BuildConversatePauseResume(1,1,true,frame,4));
  puts("G2 Conversate: captured menus, strict RX, two-hour owner/token rotation, CLI, close/re-entry, pause and recovery passed");
}
