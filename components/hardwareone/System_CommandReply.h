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

inline bool commandReplyIsFailure(const char* reply) {
  if (!reply) return false;
  return strncmp(reply, "Error", 5) == 0 ||   // the reply contract
         strncmp(reply, "ERROR", 5) == 0 ||   // a peer on older firmware
         strncmp(reply, "Usage:", 6) == 0 ||  // refused: arguments it could not use
         strncmp(reply, "Unknown command:", 16) == 0;  // no such command
}
