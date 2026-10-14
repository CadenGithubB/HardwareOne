#include <algorithm>
#include <assert.h>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

using TerminalBuilder = std::function<size_t(uint8_t*, size_t)>;

static void exact(const TerminalBuilder& builder, std::initializer_list<uint8_t> expected) {
  uint8_t out[G2_TERMINAL_PB_MAX_BYTES]{};
  const size_t length = builder(out, sizeof(out));
  assert(length == expected.size() && std::equal(expected.begin(), expected.end(), out));
  assert(builder(nullptr, sizeof(out)) == 0);
  // Failed builds may leave partial bytes, but cannot cross caller capacity.
  for (size_t cap = 0; cap < length; ++cap) {
    uint8_t guarded[G2_TERMINAL_PB_MAX_BYTES + 16];
    memset(guarded, 0xCD, sizeof(guarded));
    assert(builder(guarded + 8, cap) == 0);
    for (size_t i = 0; i < 8; ++i) assert(guarded[i] == 0xCD);
    for (size_t i = 8 + cap; i < sizeof(guarded); ++i) assert(guarded[i] == 0xCD);
  }
}

static void scalarFixtures() {
  // Independently transcribed numeric fields from the two matched stock
  // descriptors. These fixtures do not use a second Terminal encoder.
  exact([](uint8_t* p, size_t n) { return g2BuildTerminalOpenPb(17, 7, "Mingus", "P4", p, n); },
        {0x08,1,0x10,17,0x1A,28,0x0A,10,0x08,1,0x12,6,'M','i','n','g','u','s',
         0x12,14,0x08,7,0x12,10,0x08,7,0x12,2,'P','4',0x18,1,0x20,1});
  exact([](uint8_t* p, size_t n) { return g2BuildTerminalClosePb(17, 0, p, n); },
        {0x08,2,0x10,17,0x22,2,0x08,0});
  exact([](uint8_t* p, size_t n) { return g2BuildTerminalHeartbeatPb(17, p, n); },
        {0x08,0xFF,1,0x10,17,0xCA,2,0}); // Empty tag41 is still present.
  exact([](uint8_t* p, size_t n) { return g2BuildTerminalAsrPb(17, "ok", 2, true, true, p, n); },
        {0x08,3,0x10,17,0x2A,8,0x0A,2,'o','k',0x10,1,0x18,1});
  exact([](uint8_t* p, size_t n) { return g2BuildTerminalStatePb(17, 7, 2, p, n); },
        {0x08,4,0x10,17,0x32,4,0x08,7,0x10,2});
  exact([](uint8_t* p, size_t n) { return g2BuildTerminalContentPb(17, 7, 9, 2, "ok", 2, p, n); },
        {0x08,5,0x10,17,0x3A,10,0x08,7,0x10,9,0x18,2,0x22,2,'o','k'});
  exact([](uint8_t* p, size_t n) { return g2BuildTerminalSyncPb(17, 7, true, p, n); },
        {0x08,12,0x10,17,0x72,4,0x08,7,0x10,1});
  exact([](uint8_t* p, size_t n) { return g2BuildTerminalSyncPb(17, 7, false, p, n); },
        {0x08,12,0x10,17,0x72,4,0x08,7,0x10,2});
}

static G2TerminalRx parse(std::initializer_list<uint8_t> bytes) {
  G2TerminalRx event{};
  assert(g2ParseTerminalRx(bytes.begin(), bytes.size(), &event));
  return event;
}

static void eventFixtures() {
  auto event = parse({0x08,0xA1,1,0x10,17,0xA2,1,0});
  assert(event.command == G2_TERMINAL_CMD_START && event.magic == 17);
  event = parse({0x08,0xAB,1,0x10,17,0xAA,1,0});
  assert(event.command == G2_TERMINAL_CMD_EXIT && event.magic == 17);
  for (uint8_t voice = 1; voice <= 5; ++voice) {
    event = parse({0x08,0xA2,1,0x10,17,0xB2,1,2,0x08,voice});
    assert(event.command == G2_TERMINAL_CMD_VOICE && event.voiceCommand == voice);
  }
  event = parse({0x08,0xF0,1,0x10,17,0xC2,2,2,0x08,1});
  assert(event.command == G2_TERMINAL_CMD_ACK && event.ackError == 1 && event.magic == 17);
  event = parse({0x08,0xF0,1,0xC2,2,0}); // nanopb default error/magic omitted.
  assert(event.command == G2_TERMINAL_CMD_ACK && event.ackError == 0 && event.magic == 0);
  event = parse({0xC2,2,2,0x08,0,0x10,255,1,0x08,0xF0,1});
  assert(event.command == G2_TERMINAL_CMD_ACK && event.magic == 255); // Field order is free.
  event = parse({0x08,0xA4,1,0xC2,1,2,0x08,7});
  assert(event.command == G2_TERMINAL_CMD_INTERRUPT && event.sessionId == 7);
  event = parse({0x08,0xA5,1,0xCA,1,4,0x10,1,0x08,7});
  assert(event.command == G2_TERMINAL_CMD_SESSION_SWITCH && event.sessionId == 7 && event.switchFlag);
  event = parse({0x08,0xA6,1,0xD2,1,6,0x08,255,255,255,255,15});
  assert(event.command == G2_TERMINAL_CMD_NEW_SESSION && event.agentId == UINT32_MAX);
  event = parse({0x08,0xA5,1,0xCA,1,0});
  assert(event.command == G2_TERMINAL_CMD_SESSION_SWITCH && !event.sessionId && !event.switchFlag);

  const std::vector<std::vector<uint8_t>> invalid = {
    {}, {0x08,0xF0,1,0x10,17}, // Missing oneof.
    {0x10,17,0xC2,2,0}, // Missing command.
    {0x08,0xF0,1,0x10,17,0xB2,1,0}, // ACK with voice body.
    {0x08,0xF0,1,0x10,17,0xC2,2,0,0xC2,2,0}, // Duplicate body.
    {0x08,0xF0,1,0x10,17,0x10,18,0xC2,2,0}, // Duplicate magic.
    {0x08,0xF0,1,0x08,0xF0,1,0xC2,2,0}, // Duplicate command.
    {0x08,0xF0,1,0x10,0x80,2,0xC2,2,0}, // Magic exceeds uint8.
    {0x08,0xF0,1,0xC2,2,3,0x08,0x80,2}, // Error exceeds uint8.
    {0x08,0xF0,1,0xC2,2,4,0x08,0,0x08,0}, // Duplicate error scalar.
    {0x08,0xF0,1,0xC2,2,2,0x0A,0}, // Wrong scalar wire type.
    {0x08,0xA2,1,0xB2,1,0}, {0x08,0xA2,1,0xB2,1,2,0x08,6}, // Unknown voice.
    {0x08,0xAB,1,0xAA,1,2,0x08,0}, // EXIT must be empty.
    {0x08,0xF0,1,0xC2,2,2,0x08}, // Truncated nested data.
    {0x08,0xF0,1,0xC2,2,0,0x18,1}, // Unrecognized wrapper field.
    {0x08,0xF0,1,0xC2,2,0,0}, // Invalid tag zero.
    {0x08,0xA4,1,0xCA,1,0}, // INTERRUPT with SESSION_SWITCH body.
    {0x08,0xA5,1,0xCA,1,2,0x10,2}, // Non-boolean switch flag.
    {0x08,0xA4,1,0xC2,1,6,0x08,255,255,255,255,16}, // uint32 overflow.
    {0x08,0xA4,1,0xC2,1,4,0x08,7,0x08,8}, // Duplicate ID.
    {0x08,0xA6,1,0xD2,1,2,0x10,1}, // Unknown new-session body field.
    {0x08,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,2}, // uint64 overflow.
  };
  for (const auto& bytes : invalid) {
    G2TerminalRx bad{255,255,255,255,UINT32_MAX,UINT32_MAX,true};
    assert(!g2ParseTerminalRx(bytes.data(), bytes.size(), &bad));
    assert(!bad.command && !bad.magic && !bad.ackError && !bad.voiceCommand);
    assert(!bad.sessionId && !bad.agentId && !bad.switchFlag);
  }
  uint8_t excess[33]{};
  assert(!g2ParseTerminalRx(excess, sizeof(excess), &event));
  assert(!g2ParseTerminalRx(excess, 1, nullptr));
}

static void fragmented(const uint8_t* pb, size_t length) {
  assert(length > 253 && length <= G2_TERMINAL_PB_MAX_BYTES);
  G2Temple arm;
  testArm = &arm;
  testFrames.clear();
  testFault = WireFault::None;
  testNativeTerminalEpoch = kTestNativeTerminalEpoch;
  testWriteAttempts = testDelayCount = 0;
  bool finalAttempted = false;
  assert(sendPbFragmented(arm, 42, G2_SID_TERMINAL, G2_FLAG_REQUEST, pb, length,
                         arm.connectionGeneration, &finalAttempted, 0,
                         kTestLifecycle, kTestPresentation, kTestNativeTerminalEpoch));
  assert(finalAttempted && testFrames.size() == (length + 232) / 233);
  uint8_t storage[G2_TERMINAL_PB_MAX_BYTES];
  G2RxReassembly reassembly{};
  g2RxReassemblyInit(&reassembly, storage, sizeof(storage));
  G2EnvelopeView view{};
  for (size_t i = 0; i < testFrames.size(); ++i) {
    auto frame = testFrames[i];
    assert(frame.size() <= 241 && frame[0] == 0xAA && frame[1] == 0x21);
    assert(frame[2] == 42 && frame[6] == G2_SID_TERMINAL && frame[7] == G2_FLAG_REQUEST);
    frame[1] = 0x12; // Run the actual receiver against the transmitted framing.
    const auto status = g2RxReassemblyPush(&reassembly, frame.data(), frame.size(), &view);
    assert(status == (i + 1 == testFrames.size() ? G2_RX_REASSEMBLY_COMPLETE : G2_RX_REASSEMBLY_NEED_MORE));
  }
  assert(view.payloadLen == length && memcmp(view.payload, pb, length) == 0);

  // The actual fragmenter forwards its native epoch through every envelope;
  // a close between fragments must stop before another boundary write.
  testFrames.clear(); testWriteAttempts = testDelayCount = 0;
  testFault = WireFault::NativeTerminalAfterFirst;
  finalAttempted = false;
  assert(!sendPbFragmented(arm, 43, G2_SID_TERMINAL, G2_FLAG_REQUEST, pb, length,
                          arm.connectionGeneration, &finalAttempted, 0,
                          kTestLifecycle, kTestPresentation, kTestNativeTerminalEpoch));
  assert(!finalAttempted && testFrames.size() == 1 && testWriteAttempts == 1);
  assert(!testCentralHeld && !testPeerHeld);
  testFault = WireFault::None;
  testNativeTerminalEpoch = kTestNativeTerminalEpoch;
}

static void boundsAndUtf8() {
  uint8_t pb[G2_TERMINAL_PB_MAX_BYTES];
  for (const std::string& text : {std::string(512, 'x'), [] {
        std::string s; for (unsigned i = 0; i < 128; ++i) s += "\xF0\x9F\xA5\x94"; return s;
      }()}) {
    const size_t contentLength = g2BuildTerminalContentPb(255, UINT32_MAX, UINT32_MAX,
        G2_TERMINAL_CONTENT_AGENT_RESULT, text.data(), text.size(), pb, sizeof(pb));
    assert(contentLength == 537);
    assert(memcmp(pb + contentLength - text.size(), text.data(), text.size()) == 0);
    fragmented(pb, contentLength);
    const size_t asrLength = g2BuildTerminalAsrPb(255, text.data(), text.size(), true, true, pb, sizeof(pb));
    assert(asrLength == 527);
    fragmented(pb, asrLength);
  }
  const std::vector<std::string> invalid = {
    std::string(513, 'x'), std::string("a\0b", 3), "\x80", "\xC0\x80",
    "\xE0\x80\x80", "\xED\xA0\x80", "\xF4\x90\x80\x80", "\xF0\x9F\xA5",
  };
  for (const auto& text : invalid) {
    assert(!g2BuildTerminalAsrPb(1, text.data(), text.size(), true, true, pb, sizeof(pb)));
    assert(!g2BuildTerminalContentPb(1, 1, 1, 2, text.data(), text.size(), pb, sizeof(pb)));
  }
  assert(!g2BuildTerminalAsrPb(1, nullptr, SIZE_MAX, true, true, pb, sizeof(pb)));
  assert(g2BuildTerminalAsrPb(1, nullptr, 0, true, true, pb, sizeof(pb)));
  assert(!g2BuildTerminalOpenPb(1, 0, "Mingus", "P4", pb, sizeof(pb)));
  assert(!g2BuildTerminalOpenPb(1, 1, "", "P4", pb, sizeof(pb)));
  assert(!g2BuildTerminalOpenPb(1, 1, "Mingus", "", pb, sizeof(pb)));
  assert(!g2BuildTerminalOpenPb(1, 1, std::string(65, 'x').c_str(), "P4", pb, sizeof(pb)));
  assert(!g2BuildTerminalOpenPb(1, 1, "Mingus", std::string(129, 'x').c_str(), pb, sizeof(pb)));
  assert(g2BuildTerminalOpenPb(255, UINT32_MAX, std::string(64, 'x').c_str(),
                               std::string(128, 'x').c_str(), pb, sizeof(pb)));
  assert(!g2BuildTerminalStatePb(1, 1, 6, pb, sizeof(pb)));
  assert(!g2BuildTerminalStatePb(1, 0, 1, pb, sizeof(pb)));
  assert(!g2BuildTerminalSyncPb(1, 0, true, pb, sizeof(pb)));
  assert(!g2BuildTerminalContentPb(1, 1, 1, 6, "ok", 2, pb, sizeof(pb)));
}


static void sessionAndQueryFixtures() {
  exact([](uint8_t* p, size_t n) {
    G2TerminalSessionItem session{7,"P4",1,1};
    return g2BuildTerminalSessionListPb(17,7,&session,1,p,n);
  }, {0x08,8,0x10,17,0x52,14,0x08,7,0x12,10,0x08,7,0x12,2,'P','4',0x18,1,0x20,1});
  exact([](uint8_t* p, size_t n) {
    G2TerminalQueryOption option{1,"Yes"};
    return g2BuildTerminalQueryPb(17,7,9,"?",&option,1,p,n);
  }, {0x08,6,0x10,17,0x42,16,0x08,9,0x12,1,'?',0x1a,7,0x08,1,0x12,3,'Y','e','s',0x20,7});
  exact([](uint8_t* p, size_t n) { return g2BuildTerminalResultPb(17,9,true,p,n); },
        {0x08,9,0x10,17,0x5a,2,0x08,1});
  exact([](uint8_t* p, size_t n) { return g2BuildTerminalResultPb(17,11,false,p,n); },
        {0x08,11,0x10,17,0x6a,2,0x08,2});
  exact([](uint8_t* p, size_t n) { return g2BuildTerminalSessionChangePb(17,7,p,n); },
        {0x08,10,0x10,17,0x62,2,0x08,7});
  auto event = parse({0x08,0xa3,1,0xba,1,4,0x08,9,0x10,2});
  assert(event.queryId == 9 && event.optionId == 2);
  event = parse({0x08,0xa7,1,0xe2,1,6,0x08,2,0x10,7,0x18,3});
  assert(event.displayState == 2 && event.sessionId == 7 && event.overlay == 3);
  event = parse({0x08,0xa8,1,0xda,1,0}); assert(event.command == G2_TERMINAL_CMD_NEW_CANCEL);
  event = parse({0x08,0xa9,1,0xf2,1,2,0x08,9}); assert(event.focusIndex == 9);
  event = parse({0x08,0xaa,1,0xfa,1,4,0x08,2,0x10,4}); assert(event.overlay == 2 && event.focusIndex == 4);
  event = parse({0x08,0xac,1,0xea,1,2,0x08,7}); assert(event.agentId == 7);
  event = parse({0x08,0xad,1,0x82,2,2,0x08,7}); assert(event.sessionId == 7);
  event = parse({0x08,0xae,1,0x8a,2,6,0x08,7,0x12,2,'o','k'});
  assert(event.sessionId == 7 && strcmp(event.thinkingWord,"ok") == 0);
  uint8_t pb[G2_TERMINAL_PB_MAX_BYTES];
  G2TerminalAgentItem agents[10]; G2TerminalSessionItem sessions[10];
  const std::string name(64,'a'), title(128,'t'), question(1024,'q');
  for (uint32_t i=0;i<10;++i) {
    agents[i] = {UINT32_MAX-i,name.c_str()};
    sessions[i] = {UINT32_MAX-i,title.c_str(),UINT32_MAX-i,5};
  }
  size_t n = g2BuildTerminalOpenListPb(255,agents,10,UINT32_MAX,sessions,10,pb,sizeof(pb));
  assert(n > 2000 && n <= sizeof(pb)); fragmented(pb,n);
  assert(!g2BuildTerminalOpenListPb(1,agents,11,UINT32_MAX,sessions,10,pb,sizeof(pb)));
  assert(!g2BuildTerminalSessionListPb(1,4,sessions,10,pb,sizeof(pb)));
  sessions[1].id = sessions[0].id;
  assert(!g2BuildTerminalSessionListPb(1,UINT32_MAX,sessions,10,pb,sizeof(pb)));
  G2TerminalQueryOption options[8];
  for (uint32_t i=0;i<8;++i) options[i]={UINT32_MAX-i,title.c_str()};
  n = g2BuildTerminalQueryPb(255,UINT32_MAX,UINT32_MAX,question.c_str(),options,8,pb,sizeof(pb));
  assert(n > 2100 && n <= sizeof(pb)); fragmented(pb,n);
  assert(!g2BuildTerminalQueryPb(1,1,1,question.c_str(),options,9,pb,sizeof(pb)));
  assert(!g2BuildTerminalQueryPb(1,1,1,std::string(1025,'x').c_str(),options,8,pb,sizeof(pb)));
  options[1].id = options[0].id;
  assert(!g2BuildTerminalQueryPb(1,1,1,"?",options,8,pb,sizeof(pb)));
}

int main() {
  scalarFixtures();
  eventFixtures();
  boundsAndUtf8();
  sessionAndQueryFixtures();
  std::cout << "G2 native Terminal codec fixtures passed\n";
}
