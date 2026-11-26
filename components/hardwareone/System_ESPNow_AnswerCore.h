// System_ESPNow_AnswerCore.h - the answer header of ESP-NOW frames (rule F3)
//
// Every answer frame starts with the same five bytes, little endian:
//
//   0  reqId   32 bit: the number of the request this answers. It is the
//              request's own correlation number: the reqId field of a request
//              that has one (FS_*_REQ, TOPO_REQ, SENSOR_REQ), otherwise the
//              request frame's header msgId (CMD, USER_SYNC); for a file,
//              the transferId FILE_START carries (1.0 step 7). 0 =
//              no request: a frame sent unasked (the startstream STREAM).
//   4  status  0 ok; 1-15 the shared reasons below, the same number with the
//              same meaning in every answer; 16 and up a reason of that frame
//              alone (the per-frame table below)
//
// The answer frame's own header msgId is always the sender's own fresh number
// (generateMessageId), never the request's: duplicate rejection, ACK matching
// and "delivered" marks are all keyed on (sender, msgId), and a borrowed
// number mixed the two peers' number spaces (a requester's ACK of an answer
// could mark the answerer's own unrelated send as delivered; a FILE_CANCEL
// could be dropped as a duplicate of the canceller's own frame).
//
// Answer frames: TOPO_START (33), TOPO_PEER (34), CMD_RESP (51),
// FS_LIST_RESP (71), FS_STAT_RESP (73), FS_GET_RESP (75), STREAM (90),
// FILE_RESULT (113, 1.0 step 7), FILE_CANCEL (115), and since 2026-11-19 CATCHUP_LIST_RESP (83) and
// CATCHUP_READ_RESP (85) (file catch-up, System_ESPNow_CatchupCore.h). The shared code 1 (busy) lets a later firmware answer
// "not now" in any answer, which is the NACK shape (opcode 2 stays reserved).
//
// A receiver that meets a status it does not know treats it as "not ok" and
// shows its number: a 1-15 code from a later firmware is still a failure, and
// a 16+ code is only ever read together with the frame it came in.
//
// Pure: no heap, no OS, no logging (host test test_espnow_answer_core.cpp, archived 2026-11-26).
// System_ESPNow_Wire.h states the structs and asserts the opcode numbers here.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace espnowanswer {

constexpr size_t kHeaderLen = 5;  // u32 reqId + u8 status

// The status byte. 0 and the shared reasons 1-15.
constexpr uint8_t kOk          = 0;
constexpr uint8_t kBusy        = 1;   // the receiver is doing another one; send it again later
constexpr uint8_t kRefused     = 2;   // not allowed for this sender (login, key, setting, policy)
constexpr uint8_t kUnsupported = 3;   // the receiver's build does not have this
constexpr uint8_t kTooLarge    = 4;   // the request or its result is over a limit
constexpr uint8_t kNoSlot      = 5;   // no free slot for it right now
constexpr uint8_t kBadRequest  = 6;   // the request is malformed
constexpr uint8_t kNotReady    = 7;   // the receiver is not ready yet (storage not mounted, early boot)
constexpr uint8_t kIoError     = 8;   // reading or writing storage failed
constexpr uint8_t kNotFound    = 9;   // what the request names does not exist
constexpr uint8_t kTimeout     = 10;  // it did not finish in time
constexpr uint8_t kCancelled   = 11;  // stopped by the side that started it
// 12-15: shared reasons not yet given out.
constexpr uint8_t kSharedLast  = 15;
constexpr uint8_t kFrameFirst  = 16;  // 16 and up: the per-frame reasons

// The per-frame reasons (16 and up), by the answer opcode they come in.
constexpr uint8_t kCmdFailed        = 16;  // CMD_RESP: the command ran and reported a failure
constexpr uint8_t kFsNotADir        = 16;  // FS_*: the path is a file, a directory was asked for
constexpr uint8_t kFileIncomplete   = 16;  // FILE_RESULT / FILE_CANCEL: chunks are missing
constexpr uint8_t kFileCrcMismatch  = 17;  // FILE_RESULT / FILE_CANCEL: every chunk came; the CRC32 did not match
constexpr uint8_t kCatchupNoLink    = 16;  // CATCHUP_LIST_RESP: link != 0 (the bond proxy is not built)
constexpr uint8_t kCatchupGone      = 16;  // CATCHUP_READ_RESP: no such file id, other ledger, or the file is gone
constexpr uint8_t kCatchupTrimmed   = 17;  // CATCHUP_READ_RESP: the offset was trimmed off; offset = the new start
constexpr uint8_t kCatchupPastEnd   = 18;  // CATCHUP_READ_RESP: the offset is past the file's end

// The answer opcodes (System_ESPNow_Wire.h asserts that these are its numbers).
constexpr uint8_t kOpTopoStart   = 33;
constexpr uint8_t kOpTopoPeer    = 34;
constexpr uint8_t kOpCmdResp     = 51;
constexpr uint8_t kOpFsListReply = 71;
constexpr uint8_t kOpFsStatReply = 73;
constexpr uint8_t kOpFsGetAck    = 75;
constexpr uint8_t kOpStream      = 90;
constexpr uint8_t kOpFileResult  = 113;
constexpr uint8_t kOpFileCancel  = 115;
constexpr uint8_t kOpCatchupListResp = 83;
constexpr uint8_t kOpCatchupReadResp = 85;

inline bool isAnswerOpcode(uint8_t opcode) {
  switch (opcode) {
    case kOpTopoStart: case kOpTopoPeer: case kOpCmdResp: case kOpFsListReply:
    case kOpFsStatReply: case kOpFsGetAck: case kOpStream: case kOpFileResult: case kOpFileCancel:
    case kOpCatchupListResp: case kOpCatchupReadResp:
      return true;
    default:
      return false;
  }
}

// The one name table. snake_case words for JSON and logs; nullptr for a code
// this firmware does not know (a caller shows the number).
inline const char* sharedStatusName(uint8_t status) {
  switch (status) {
    case kOk:          return "ok";
    case kBusy:        return "busy";
    case kRefused:     return "refused";
    case kUnsupported: return "unsupported";
    case kTooLarge:    return "too_large";
    case kNoSlot:      return "no_slot";
    case kBadRequest:  return "bad_request";
    case kNotReady:    return "not_ready";
    case kIoError:     return "io_error";
    case kNotFound:    return "not_found";
    case kTimeout:     return "timeout";
    case kCancelled:   return "cancelled";
    default:           return nullptr;
  }
}

inline const char* statusName(uint8_t opcode, uint8_t status) {
  if (status < kFrameFirst) return sharedStatusName(status);
  switch (opcode) {
    case kOpCmdResp:
      return status == kCmdFailed ? "failed" : nullptr;
    case kOpFsListReply: case kOpFsStatReply: case kOpFsGetAck:
      return status == kFsNotADir ? "not_a_dir" : nullptr;
    case kOpFileResult: case kOpFileCancel:
      return status == kFileIncomplete ? "incomplete" : status == kFileCrcMismatch ? "crc_mismatch" : nullptr;
    case kOpCatchupListResp:
      return status == kCatchupNoLink ? "no_link" : nullptr;
    case kOpCatchupReadResp:
      return status == kCatchupGone ? "gone" : status == kCatchupTrimmed ? "trimmed"
           : status == kCatchupPastEnd ? "past_end" : nullptr;
    default:
      return nullptr;
  }
}

inline bool isOk(uint8_t status) { return status == kOk; }

// Writes the five header bytes at `out` (little endian).
inline void packHeader(uint8_t* out, uint32_t reqId, uint8_t status) {
  out[0] = (uint8_t)(reqId & 0xFF);
  out[1] = (uint8_t)((reqId >> 8) & 0xFF);
  out[2] = (uint8_t)((reqId >> 16) & 0xFF);
  out[3] = (uint8_t)((reqId >> 24) & 0xFF);
  out[4] = status;
}

// Reads the header. False (and nothing written) when `len` is under five
// bytes; the receive table's minimum already drops such a frame.
inline bool parseHeader(const uint8_t* p, size_t len, uint32_t* reqId, uint8_t* status) {
  if (!p || len < kHeaderLen) return false;
  if (reqId) {
    *reqId = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
  }
  if (status) *status = p[4];
  return true;
}

}  // namespace espnowanswer
