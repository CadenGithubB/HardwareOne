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

// Replies for an argument that does not parse: a number that is not one
// (parseIntArg() in System_ArgParse.h) and an on/off value that is neither
// (parseBoolArg() in System_Utils.h).
constexpr char kCommandReplyNotANumber[] =
    "Error: Not a whole number. Give the value in digits, for example 250.";
constexpr char kCommandReplyNotOnOff[] =
    "Error: Not an on/off value. Use on or off.";

// ---- The words a reply leads with ------------------------------------------
// This header is the only place that spells them to read a reply (host test
// reply_wording). Everything else asks the functions below.
//   "OK: <text>"       the executor's stamp on a handler success (stampOkStatus)
//   "SUCCESS: <text>"  a handler's own confirmation that it did the action; the
//                      executor leaves it unstamped
//   "Error: <reason>"  a failure
constexpr char kCommandReplyOkStamp[] = "OK: ";
constexpr char kCommandReplyErrorLead[] = "Error: ";

// The executor's refusal of a caller without the privilege a command row
// needs. The executor appends the command name and the next action.
constexpr char kCommandReplyAdminRequired[] = "Error: Admin access required";
constexpr char kCommandReplySuperAdminRequired[] = "Error: Super-admin access required";

// The reply leads with "Error" or "ERROR" (a peer on older firmware): a
// failure that carries a reason. Narrower than commandReplyIsFailure().
inline bool commandReplyIsError(const char* reply) {
  return reply && (strncmp(reply, "Error", 5) == 0 || strncmp(reply, "ERROR", 5) == 0);
}

inline bool commandReplyIsFailure(const char* reply) {
  if (!reply) return false;
  return commandReplyIsError(reply) ||        // the reply contract
         strncmp(reply, "Usage:", 6) == 0 ||  // refused: arguments it could not use
         strncmp(reply, "Unknown command:", 16) == 0;  // no such command
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

// The reply is the executor's admin or super-admin refusal.
inline bool commandReplyIsAdminRefusal(const char* reply) {
  return reply &&
         (strncmp(reply, kCommandReplyAdminRequired, sizeof(kCommandReplyAdminRequired) - 1) == 0 ||
          strncmp(reply, kCommandReplySuperAdminRequired,
                  sizeof(kCommandReplySuperAdminRequired) - 1) == 0);
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
inline bool commandReplyJsonOk(const char* reply) {
  return reply && strstr(reply, "\"ok\":true") != nullptr;
}
inline bool commandReplyJsonFailed(const char* reply) {
  return reply && strstr(reply, "\"ok\":false") != nullptr;
}
