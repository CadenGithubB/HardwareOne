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
// collapses the two and would tell someone to throw away an intact file. A
// site that needs the text itself compares the length it read with the file
// size first, and records a short read as NoMemory.
//
// HOW A SITE USES IT: keep one Status per file, set it at every exit of the
// loader, and have the saver return early unless savable(status). A loader that
// finds damaged() bytes moves the file aside (quarantineCorruptFile in
// System_Settings) and records AbsentOk, so the device recovers as a fresh one
// does; if the move fails the damaged status stays and saves keep refusing.
//
// Partial is not savable, although the file is intact: memory holds less than
// the file does. Its saver first copies the file aside (keepPartialOriginal in
// System_Settings), sets the status to Ok and then saves; if the copy fails the
// save is refused.
//
// DEPENDENCY-FREE on purpose (only <stdint.h>): the host tests compile this
// real header, and consumers can include it anywhere. classifyParse is a
// template so the header does not need ArduinoJson to name its error type.

#include <stdint.h>

namespace ConfigLoad {

enum class Status : uint8_t {
  NotLoaded = 0,  // no load has run yet this boot. The zero value on purpose: a
                  // status that nobody set must never read as savable.
  Ok,          // parsed clean, every entry accepted — the file is the truth
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
    case Status::NotLoaded:  return "not-loaded";
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

// True when the bytes on disk are bad and will stay bad on every boot. Leaving
// such a file in place keeps its saver refusing forever, so the loader moves it
// aside and carries on as AbsentOk. Never true for a fileIsIntact() status.
inline bool damaged(Status s) {
  return s == Status::EmptyFile || s == Status::Truncated || s == Status::Corrupt;
}

// ParseError is ArduinoJson's DeserializationError (a template so this header
// stays free of includes).
template <typename ParseError>
inline Status classifyParse(const ParseError& err) {
  switch (err.code()) {
    case ParseError::Ok:              return Status::Ok;
    case ParseError::EmptyInput:      return Status::EmptyFile;
    case ParseError::IncompleteInput: return Status::Truncated;
    case ParseError::NoMemory:        return Status::NoMemory;
    default:                          return Status::Corrupt;  // InvalidInput, TooDeep
  }
}

}  // namespace ConfigLoad

#endif  // SYSTEM_CONFIG_LOAD_H
