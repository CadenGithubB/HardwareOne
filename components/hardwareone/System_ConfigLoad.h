#ifndef SYSTEM_CONFIG_LOAD_H
#define SYSTEM_CONFIG_LOAD_H

// System_ConfigLoad.h — one place where "may a saver overwrite this file?" is
// decided.
//
// THE BUG CLASS THIS EXISTS TO KILL ("load-failure wipe"): a loader reads a
// config file into a fixed in-RAM table; on a parse error it returns early,
// leaving the table EMPTY, but leaves a "loaded" flag set (or has no flag at
// all). The next mutation calls the saver, which serialises that empty table
// over the good file. One corrupt file destroys the data permanently, with no
// reflash and no user action. Found at 12 sites in this component.
//
// This header generalises the gate already used by writeSettingsJson(), which
// refuses to write when this boot never successfully loaded settings.json.
//
// DELIBERATELY HAS NO FILE-READING HELPER. Each site must stream
// deserializeJson() straight off its own File, so a heap shortage surfaces as
// NoMemory (the file is FINE) rather than as a short String that looks like
// IncompleteInput (the file is damaged). That difference decides whether the
// operator is told to reboot or to quarantine — reading into a String first
// collapses the two and would tell someone to throw away an intact file.
//
// INCLUDE PLACEMENT: put this include INSIDE the consumer's existing build gate
// (after `#if ENABLE_ESPNOW`, after `#if ENABLE_HTTP_SERVER`), never at the top
// of the file.

#include <Arduino.h>
#include <ArduinoJson.h>
#include "System_Debug.h"    // logSystemEvent (declared here, not in System_Events.h)
#include "System_Events.h"   // SYSEVT_CONFIG_FILE_CORRUPT, systemEventPost

namespace ConfigLoad {

enum class Status : uint8_t {
  Ok = 0,      // parsed clean, every entry accepted — the file is the truth
  AbsentOk,    // no file on disk — first boot / post-erase; RAM is the truth
  NotMounted,  // filesystem not ready. Its OWN arm: folding it into AbsentOk is
               // literally the bug this header exists to prevent.
  OpenFailed,  // exists, would not open — transient; the file is intact
  EmptyFile,   // exists, zero bytes — always a truncated write here
  Truncated,   // parser hit end of input
  Corrupt,     // not JSON / nested too deep
  NoMemory,    // parse ran out of heap — THE FILE IS FINE, do not quarantine it
  Partial,     // parsed, but N entries were lost (undecryptable / malformed)
};

inline const char* statusName(Status s) {
  switch (s) {
    case Status::Ok:         return "ok";
    case Status::AbsentOk:   return "absent";
    case Status::NotMounted: return "fs-not-mounted";
    case Status::OpenFailed: return "open-failed";
    case Status::EmptyFile:  return "zero-length";
    case Status::Truncated:  return "truncated";
    case Status::Corrupt:    return "corrupt";
    case Status::NoMemory:   return "out-of-memory";
    case Status::Partial:    return "partial";
  }
  return "?";
}

// Absence is a FIRST-CLASS savable state, and must be decided by an explicit
// exists() probe — never inferred from a failed open. That is the bootstrap
// escape hatch: without it an erase-flashed device deadlocks forever (flag never
// set -> save refused -> file never created -> flag never set).
inline bool savable(Status s) {
  return s == Status::Ok || s == Status::AbsentOk;
}

// True when the on-disk file is known-good, so quarantining or deleting it would
// destroy intact data. NoMemory and OpenFailed are transient conditions of THIS
// BOOT, not properties of the file.
inline bool fileIsIntact(Status s) {
  return s == Status::NoMemory || s == Status::OpenFailed || s == Status::Partial;
}

inline Status classifyParse(DeserializationError err) {
  switch (err.code()) {
    case DeserializationError::Ok:              return Status::Ok;
    case DeserializationError::EmptyInput:      return Status::EmptyFile;
    case DeserializationError::IncompleteInput: return Status::Truncated;
    case DeserializationError::NoMemory:        return Status::NoMemory;
    default:                                    return Status::Corrupt;  // InvalidInput, TooDeep
  }
}

}  // namespace ConfigLoad

#endif  // SYSTEM_CONFIG_LOAD_H
