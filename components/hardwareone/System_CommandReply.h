// System_CommandReply.h - the single owner of the command reply failure rule.
//
// A command handler returns its reply as text (OUTPUT CONTRACT channel 2 in
// System_Utils.cpp). A failed command returns
//   "Error: <known reason>. <next action>."
// (CONTRIBUTING.md), never a bare "ERROR" or "Error": the caller of the
// command gets only the reply, not the lines the command printed on the way.
//
// executeCommand() turns the reply into the bool it hands to every caller. A
// caller that holds that bool uses it and does not look at the text again.
// Code that has only the text (a handler called directly, a confirmation
// callback, a reply relayed from another device) asks commandReplyIsFailure().
//
// Pure header: no Arduino or ESP-IDF dependency, so host tests include it.
#pragma once

#include <string.h>

// Reply for the one failure every handler shares: no buffer to build a reply in.
constexpr char kCommandReplyOutOfMemory[] =
    "Error: Out of memory. Free memory and retry the command.";

// Replies for an argument that does not parse: a whole number that is not one
// (parseIntArg() in System_ArgParse.h) and an on/off value that is neither
// (parseBoolArg() in System_Utils.h).
constexpr char kCommandReplyNotANumber[] =
    "Error: Not a whole number. Give the value in digits, for example 250.";
constexpr char kCommandReplyNotOnOff[] =
    "Error: Not an on/off value. Use on or off.";
// A value with a fraction that is not a plain decimal number (parseFloatArg()
// in System_ArgParse.h).
constexpr char kCommandReplyNotADecimal[] =
    "Error: Not a number. Give the value in digits, for example 0.5.";

// ---- The words a reply leads with ------------------------------------------
// This header is the only place that spells them to read a reply (host test
// reply_wording). Everything else asks the functions below.
//   "OK: <text>"       the executor's stamp on a handler success (stampOkStatus)
//   "SUCCESS: <text>"  a handler's own confirmation that it did the action; the
//                      executor leaves it unstamped
//   "Error: <reason>"  a failure
constexpr char kCommandReplyOkStamp[] = "OK: ";
constexpr char kCommandReplyErrorLead[] = "Error: ";
// The executor's reply to a line with no command in it.
constexpr char kCommandReplyEmptyCommand[] =
    "Error: Empty command. Type a command, or help for the list.";

// The executor's refusal of a caller without the privilege a command row
// needs. The executor appends the command name and the next action.
constexpr char kCommandReplyAdminRequired[] = "Error: Admin access required";
constexpr char kCommandReplySuperAdminRequired[] = "Error: Super-admin access required";
// The executor's refusal of a guest (view-only) account for a line outside the
// guest surface (commandAllowedForGuest, System_Utils.cpp).
constexpr char kCommandReplyGuestViewOnly[] =
    "Error: Guest accounts are view-only. This command is not available to a guest.";
// Starting, stopping and setting up logging is an administrator's job; every
// signed-in account may read the status and the logs it may read.
constexpr char kCommandReplyLoggingAdminRequired[] =
    "Error: Admin access required to start, stop or change logging. Contact an administrator.";

// The reply leads with "Error" or "ERROR" (a peer on older firmware): a
// failure that carries a reason. Narrower than commandReplyIsFailure().
inline bool commandReplyIsError(const char* reply) {
  return reply && (strncmp(reply, "Error", 5) == 0 || strncmp(reply, "ERROR", 5) == 0);
}

// A JSON object reply that states a failure in its own top-level fields:
// "success":false or "ok":false (either one wins over the other), or an error
// object, the object's first key being "error" with a value other than null
// ({"error":"oom"}). A nested object, an array element or a string that only
// contains those words does not count, and neither does an "error" field later
// in a status object (the last error a running service reports).
inline bool commandReplyJsonStatesFailure(const char* reply) {
  if (!reply) return false;
  auto skipSpace = [](const char* p) {
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    return p;
  };
  auto keyIs = [](const char* key, size_t len, const char* word) {
    return strlen(word) == len && strncmp(key, word, len) == 0;
  };
  auto literalIs = [](const char* p, const char* word) {
    const size_t n = strlen(word);
    if (strncmp(p, word, n) != 0) return false;
    const char next = p[n];
    return next == ',' || next == '}' || next == ' ' || next == '\t' || next == '\r' ||
           next == '\n';
  };
  const char* p = skipSpace(reply);
  if (*p != '{') return false;
  int depth = 0;
  bool expectKey = false;
  bool firstKey = true;
  for (; *p; ++p) {
    const char c = *p;
    if (c == '"') {
      const char* text = p + 1;
      const char* end = text;
      while (*end && *end != '"') end += (end[0] == '\\' && end[1]) ? 2 : 1;
      if (!*end) return false;  // unterminated string: not a JSON reply
      if (depth == 1 && expectKey) {
        const size_t len = (size_t)(end - text);
        const char* value = skipSpace(end + 1);
        if (*value == ':') {
          value = skipSpace(value + 1);
          if ((keyIs(text, len, "success") || keyIs(text, len, "ok")) && literalIs(value, "false")) {
            return true;
          }
          if (firstKey && keyIs(text, len, "error") && !literalIs(value, "null")) return true;
        }
        expectKey = false;
        firstKey = false;
      }
      p = end;
      continue;
    }
    if (c == '{' || c == '[') {
      ++depth;
      if (depth == 1) expectKey = true;
    } else if (c == '}' || c == ']') {
      if (--depth <= 0) break;
    } else if (c == ',' && depth == 1) {
      expectKey = true;
    }
  }
  return false;
}

inline bool commandReplyIsFailure(const char* reply) {
  if (!reply) return false;
  return commandReplyIsError(reply) ||        // the reply contract
         strncmp(reply, "Usage:", 6) == 0 ||  // refused: arguments it could not use
         strncmp(reply, "Unknown command:", 16) == 0 ||  // no such command
         commandReplyJsonStatesFailure(reply);  // a JSON reply's own failure verdict
}

// The whole reply is "OK": a handler that printed its output on the way and
// has nothing else to return.
inline bool commandReplyIsBareOk(const char* reply) {
  return reply && strcmp(reply, "OK") == 0;
}

// The reply leads with "OK": a completed command as the executor reports it.
// A prompt, a status dump or a structured payload does not carry the stamp.
inline bool commandReplyHasOkStamp(const char* reply) {
  return reply && strncmp(reply, "OK", 2) == 0;
}

// The reply leads with "SUCCESS": the handler confirms that it carried out the
// action it was asked for (a logger started, a track toggled).
inline bool commandReplyConfirms(const char* reply) {
  return reply && strncmp(reply, "SUCCESS", 7) == 0;
}

// The reply is the executor's super-admin refusal.
inline bool commandReplyIsSuperAdminRefusal(const char* reply) {
  return reply && strncmp(reply, kCommandReplySuperAdminRequired,
                          sizeof(kCommandReplySuperAdminRequired) - 1) == 0;
}

// The reply is the executor's admin or super-admin refusal.
inline bool commandReplyIsAdminRefusal(const char* reply) {
  return reply &&
         (strncmp(reply, kCommandReplyAdminRequired, sizeof(kCommandReplyAdminRequired) - 1) == 0 ||
          strncmp(reply, kCommandReplySuperAdminRequired,
                  sizeof(kCommandReplySuperAdminRequired) - 1) == 0);
}

// The reply is the executor's guest (view-only) refusal.
inline bool commandReplyIsGuestRefusal(const char* reply) {
  return reply && strncmp(reply, kCommandReplyGuestViewOnly,
                          sizeof(kCommandReplyGuestViewOnly) - 1) == 0;
}

// The text after the "OK: " stamp, for a row or a toast; the reply itself when
// it carries no stamp. Never nullptr.
inline const char* commandReplyBody(const char* reply) {
  if (!reply) return "";
  const size_t lead = sizeof(kCommandReplyOkStamp) - 1;
  return strncmp(reply, kCommandReplyOkStamp, lead) == 0 ? reply + lead : reply;
}

// A failure's reason: the text after "Error: "; the reply itself when it does
// not lead with it. Never nullptr.
inline const char* commandReplyReason(const char* reply) {
  if (!reply) return "";
  const size_t lead = sizeof(kCommandReplyErrorLead) - 1;
  return strncmp(reply, kCommandReplyErrorLead, lead) == 0 ? reply + lead : reply;
}

// A structured reply states its own verdict: {"schema":1,"ok":true|false,...}.
// commandReplyIsFailure() reads it too (commandReplyJsonStatesFailure), so the
// executor's bool already says a JSON failure failed.
inline bool commandReplyJsonOk(const char* reply) {
  return reply && strstr(reply, "\"ok\":true") != nullptr;
}
inline bool commandReplyJsonFailed(const char* reply) {
  return reply && strstr(reply, "\"ok\":false") != nullptr;
}
