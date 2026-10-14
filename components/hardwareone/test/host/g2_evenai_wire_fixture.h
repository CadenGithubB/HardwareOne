#pragma once

// Decode the actual codec output at the native adapter's send boundary. This
// catches successful mock writes whose real protobuf would exceed the current
// MTU, omit the final text, or alter the native single-shot contract.
#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>

namespace G2EvenAiWireFixture {
inline uint64_t scalar(const uint8_t* pb, size_t size, size_t& pos, uint8_t wire) {
  assert(wire == G2_PB_WIRE_VARINT);
  uint64_t value = 0;
  assert(g2PbReadVarint(pb, size, &pos, &value));
  return value;
}

inline void body(const uint8_t* pb, size_t size, const char* text, bool reply, bool end) {
  size_t pos = 0;
  bool countSeen = false, streamSeen = false, modeSeen = false, textSeen = false, endSeen = false;
  while (pos < size) {
    uint32_t field = 0;
    uint8_t wire = 0;
    assert(g2PbReadTag(pb, size, &pos, &field, &wire));
    if (field == G2_AI_REPLY_F_TEXT) {
      assert(!textSeen && wire == G2_PB_WIRE_LEN_DELIM);
      textSeen = true;
      uint64_t bytes = 0;
      assert(g2PbReadVarint(pb, size, &pos, &bytes) && bytes <= size - pos);
      assert(std::string(reinterpret_cast<const char*>(pb + pos), size_t(bytes)) == text);
      pos += size_t(bytes);
    } else if (field == G2_AI_REPLY_F_CNT) {
      assert(!countSeen && scalar(pb, size, pos, wire) == 0); countSeen = true;
    } else if (field == G2_AI_REPLY_F_STREAM) {
      assert(!streamSeen && scalar(pb, size, pos, wire) == 0); streamSeen = true;
    } else if (field == G2_AI_REPLY_F_MODE) {
      assert(!modeSeen && scalar(pb, size, pos, wire) == 0); modeSeen = true;
    } else if (field == G2_AI_REPLY_F_END) {
      assert(reply && !endSeen && scalar(pb, size, pos, wire) == uint64_t(end)); endSeen = true;
    } else {
      assert(false && "unexpected native text field");
    }
  }
  assert(pos == size && countSeen && streamSeen && modeSeen);
  assert(textSeen == (text[0] != '\0') && endSeen == reply);
}
}  // namespace G2EvenAiWireFixture

inline void checkNativeWire(const char* text, bool reply, bool end) {
  assert(text && std::strlen(text) <= 212);
  assert(!end || (reply && text[0] != '\0'));
  uint8_t frame[256] = {};
  constexpr uint8_t seq = 71;
  const size_t count = reply
      ? g2BuildEvenAIReply(seq, 207, 0, text, end, frame, sizeof(frame))
      : g2BuildEvenAIAsk(seq, 207, 0, text, frame, sizeof(frame));
  assert(count > G2_ENVELOPE_HDR_LEN + G2_ENVELOPE_CRC_LEN && count <= 241);
  assert(frame[0] == G2_PREAMBLE_0 && frame[1] == G2_PREAMBLE_TX && frame[2] == seq);
  assert(size_t(frame[3]) + G2_ENVELOPE_HDR_LEN == count);
  assert(frame[4] == 1 && frame[5] == 1 && frame[6] == G2_SID_EVEN_AI && frame[7] == G2_FLAG_REQUEST);
  const uint8_t* pb = frame + G2_ENVELOPE_HDR_LEN;
  const size_t size = count - G2_ENVELOPE_HDR_LEN - G2_ENVELOPE_CRC_LEN;
  const uint16_t crc = uint16_t(frame[count - 2]) | (uint16_t(frame[count - 1]) << 8);
  assert(crc == g2CrcCcittFalse(pb, size));
  G2EnvelopeView view{};
  assert(g2ParseEnvelope(frame, count, &view));
  // The parser must reject corrupt payload bytes, even with intact lengths.
  frame[count - 3] ^= 1;
  assert(!g2ParseEnvelope(frame, count, &view));
  frame[count - 3] ^= 1;
  bool commandSeen = false, magicSeen = false, bodySeen = false;
  size_t pos = 0;
  while (pos < size) {
    uint32_t field = 0;
    uint8_t wire = 0;
    assert(g2PbReadTag(pb, size, &pos, &field, &wire));
    if (field == G2_AI_F_CMD) {
      assert(!commandSeen && G2EvenAiWireFixture::scalar(pb, size, pos, wire) ==
          uint64_t(reply ? G2_AI_CMD_REPLY : G2_AI_CMD_ASK));
      commandSeen = true;
    } else if (field == G2_AI_F_MAGIC) {
      assert(!magicSeen && G2EvenAiWireFixture::scalar(pb, size, pos, wire) == 207);
      magicSeen = true;
    } else {
      assert(!bodySeen && field == uint32_t(reply ? G2_AI_F_REPLY : G2_AI_F_ASK));
      assert(wire == G2_PB_WIRE_LEN_DELIM);
      uint64_t bytes = 0;
      assert(g2PbReadVarint(pb, size, &pos, &bytes) && bytes <= size - pos);
      G2EvenAiWireFixture::body(pb + pos, size_t(bytes), text, reply, end);
      pos += size_t(bytes);
      bodySeen = true;
    }
  }
  assert(pos == size && commandSeen && magicSeen && bodySeen);
  if (std::strlen(text) == 212) assert(count == size_t(reply ? 241 : 239));
}
