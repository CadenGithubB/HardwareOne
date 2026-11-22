// G2_ReplyNotice.h - the short lens text for a command the firmware refused.
//
// A lens row holds a few words. When a tap's command comes back refused, the
// glasses show why in that space instead of failing silently:
//   admin refusal (incl. the logging one)  -> "Needs an admin"
//   super-admin refusal                    -> "Needs a super admin"
//   any other failure                      -> the reason's first sentence
//                                             ("This recording belongs to alice")
// The firmware stays the only decision point; this file only words its reply.
// Every G2 page asks g2ReplyNoticeText() (directly, or through the lens notice
// in G2_HijackCmd.h) rather than cutting reply strings itself.
//
// Pure header: no Arduino or ESP-IDF dependency, so host tests include it.
#pragma once

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "System_CommandReply.h"

constexpr char kG2NoticeNeedsAdmin[] = "Needs an admin";
constexpr char kG2NoticeNeedsSuperAdmin[] = "Needs a super admin";
constexpr char kG2NoticeFailed[] = "Command failed";

// Room for the longest first sentence a refusal carries; a row or readout
// clips it further to its own width.
constexpr size_t kG2NoticeMax = 80;

// Writes the lens text for a refused or failed reply and returns true. A
// completed command (ok and no failure lead) writes "" and returns false.
// `ok` is the executor's verdict; the reply text is read only through
// System_CommandReply.h.
inline bool g2ReplyNoticeText(bool ok, const char* reply, char* out, size_t cap) {
  if (out && cap) out[0] = '\0';
  if (ok && !commandReplyIsFailure(reply)) return false;
  if (!out || cap == 0) return true;
  if (commandReplyIsSuperAdminRefusal(reply)) {
    snprintf(out, cap, "%s", kG2NoticeNeedsSuperAdmin);
    return true;
  }
  if (commandReplyIsAdminRefusal(reply)) {
    snprintf(out, cap, "%s", kG2NoticeNeedsAdmin);
    return true;
  }
  const char* reason = commandReplyReason(reply);
  while (*reason == ' ') ++reason;
  // First sentence: up to ". ", a line break or a parenthetical aside.
  size_t n = 0;
  while (reason[n] && reason[n] != '\n' && reason[n] != '\r' &&
         !(reason[n] == '.' && (reason[n + 1] == ' ' || reason[n + 1] == '\0')) &&
         !(reason[n] == ' ' && reason[n + 1] == '(')) {
    ++n;
  }
  while (n > 0 && (reason[n - 1] == ' ' || reason[n - 1] == ':')) --n;
  if (n == 0) {
    snprintf(out, cap, "%s", kG2NoticeFailed);
    return true;
  }
  snprintf(out, cap, "%.*s", (int)n, reason);
  return true;
}

// True when the reply is one g2ReplyNoticeText words: the command was refused
// or failed.
inline bool g2ReplyRefused(bool ok, const char* reply) {
  return g2ReplyNoticeText(ok, reply, nullptr, 0);
}
