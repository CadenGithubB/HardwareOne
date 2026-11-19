// The answer header of ESP-NOW frames (System_ESPNow_AnswerCore.h, compiled
// as is): rule F3 of the 1.0 format.
//
//   * the header is five bytes, {u32 reqId little endian, u8 status}, and a
//     round trip keeps every value; a payload under five bytes does not parse;
//   * status 0 is ok and nothing else is: an unknown shared code (12-15) or
//     an unknown frame code (16+) is a failure with no name;
//   * one name table: the shared codes 0-11 have the same name in every
//     answer; the 16+ codes are read only with their opcode (16 is "failed"
//     in CMD_RESP, "not_a_dir" in the FS answers, "incomplete" in
//     FILE_CANCEL) and have no name in an answer that defines none;
//   * the shared numbers are pinned: a later firmware reads busy as busy;
//   * the answer opcodes are the eight 1.0 answers and nothing else.
#include "System_ESPNow_AnswerCore.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

static unsigned gChecks = 0;
#define CHECK(c)                                                                 \
  do {                                                                           \
    ++gChecks;                                                                   \
    if (!(c)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); \
      std::exit(1);                                                              \
    }                                                                            \
  } while (0)

using namespace espnowanswer;

static bool is(const char* a, const char* b) { return a && b && std::strcmp(a, b) == 0; }

static void header_layout() {
  CHECK(kHeaderLen == 5);
  uint8_t out[8];
  std::memset(out, 0xEE, sizeof(out));
  packHeader(out, 0x11223344u, kBusy);
  CHECK(out[0] == 0x44 && out[1] == 0x33 && out[2] == 0x22 && out[3] == 0x11);  // little endian
  CHECK(out[4] == 1);
  CHECK(out[5] == 0xEE);  // nothing past the five bytes
  uint32_t reqId = 0;
  uint8_t status = 0xFF;
  CHECK(parseHeader(out, 5, &reqId, &status) && reqId == 0x11223344u && status == kBusy);
  // Every reqId and status survive a round trip, including 0 and the extremes.
  for (uint32_t id : {0u, 1u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu}) {
    for (unsigned st = 0; st < 256; st += 17) {
      packHeader(out, id, (uint8_t)st);
      CHECK(parseHeader(out, sizeof(out), &reqId, &status) && reqId == id && status == st);
    }
  }
  // Short payloads do not parse, and nothing is written.
  reqId = 77;
  status = 77;
  CHECK(!parseHeader(out, 4, &reqId, &status) && reqId == 77 && status == 77);
  CHECK(!parseHeader(out, 0, &reqId, &status));
  CHECK(!parseHeader(nullptr, 5, &reqId, &status));
  CHECK(parseHeader(out, 5, nullptr, nullptr));
}

static void ok_and_failures() {
  CHECK(isOk(kOk));
  for (unsigned st = 1; st < 256; ++st) CHECK(!isOk((uint8_t)st));
}

static void shared_numbers_pinned() {
  // The wire values: frozen at 1.0, a later firmware reads them the same.
  CHECK(kOk == 0 && kBusy == 1 && kRefused == 2 && kUnsupported == 3 && kTooLarge == 4 && kNoSlot == 5 &&
        kBadRequest == 6 && kNotReady == 7 && kIoError == 8 && kNotFound == 9 && kTimeout == 10 &&
        kCancelled == 11);
  CHECK(kSharedLast == 15 && kFrameFirst == 16);
  CHECK(kCmdFailed == 16 && kFsNotADir == 16 && kFileIncomplete == 16 && kFileCrcMismatch == 17);
}

static void one_name_table() {
  const char* shared[] = {"ok", "busy", "refused", "unsupported", "too_large", "no_slot",
                          "bad_request", "not_ready", "io_error", "not_found", "timeout", "cancelled"};
  const uint8_t answers[] = {kOpTopoStart, kOpTopoPeer, kOpCmdResp, kOpFsListReply,
                             kOpFsStatReply, kOpFsGetAck, kOpStream, kOpFileCancel};
  for (uint8_t st = 0; st < 12; ++st) {
    CHECK(is(sharedStatusName(st), shared[st]));
    // The same word in every answer.
    for (uint8_t op : answers) CHECK(is(statusName(op, st), shared[st]));
  }
  // 12-15: shared codes not given out yet: no name (shown as a number).
  for (uint8_t st = 12; st <= kSharedLast; ++st) {
    CHECK(sharedStatusName(st) == nullptr);
    for (uint8_t op : answers) CHECK(statusName(op, st) == nullptr);
  }
  // 16+: read with their frame only.
  CHECK(is(statusName(kOpCmdResp, 16), "failed"));
  CHECK(is(statusName(kOpFsListReply, 16), "not_a_dir"));
  CHECK(is(statusName(kOpFsStatReply, 16), "not_a_dir"));
  CHECK(is(statusName(kOpFsGetAck, 16), "not_a_dir"));
  CHECK(is(statusName(kOpFileCancel, 16), "incomplete"));
  CHECK(is(statusName(kOpFileCancel, 17), "crc_mismatch"));
  CHECK(statusName(kOpCmdResp, 17) == nullptr);
  CHECK(statusName(kOpFsListReply, 17) == nullptr);
  CHECK(statusName(kOpStream, 16) == nullptr);
  CHECK(statusName(kOpTopoStart, 16) == nullptr);
  CHECK(statusName(kOpTopoPeer, 16) == nullptr);
  CHECK(statusName(kOpFileCancel, 18) == nullptr);
  CHECK(statusName(kOpCmdResp, 255) == nullptr);
  // A frame code is never read as another frame's: 16 differs by frame.
  CHECK(!is(statusName(kOpCmdResp, 16), statusName(kOpFileCancel, 16)));
}

static void answer_opcodes() {
  unsigned count = 0;
  for (unsigned op = 0; op < 256; ++op) {
    if (isAnswerOpcode((uint8_t)op)) ++count;
  }
  CHECK(count == 8);
  CHECK(kOpTopoStart == 33 && kOpTopoPeer == 34 && kOpCmdResp == 51 && kOpFsListReply == 71 &&
        kOpFsStatReply == 73 && kOpFsGetAck == 75 && kOpStream == 90 && kOpFileCancel == 115);
  // Requests are not answers.
  CHECK(!isAnswerOpcode(50) && !isAnswerOpcode(32) && !isAnswerOpcode(70) && !isAnswerOpcode(153) &&
        !isAnswerOpcode(110) && !isAnswerOpcode(1));
}

int main() {
  header_layout();
  ok_and_failures();
  shared_numbers_pinned();
  one_name_table();
  answer_opcodes();
  std::printf("espnow_answer_core: %u checks passed\n", gChecks);
  return 0;
}
