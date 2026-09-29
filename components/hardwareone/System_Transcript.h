#pragma once
#include "System_BuildConfig.h"
#include "System_User.h"
#include <cstddef>
#include <cstdint>

// Admission-time preference/identity. POD: safe to copy under a short broker
// lock; captures no text and owns no heap or filesystem resource.
struct TranscriptOptions {
  bool enabled = false;
  CommandSource source = SOURCE_INTERNAL;
  TransportSessionEpoch epoch = 0;
  char user[65] = {};
  char started[24] = {};
};
struct TranscriptStatus {
  bool enabled = false;
  bool saved = false;
  bool complete = false;
  uint32_t chunks = 0;
  uint32_t bytes = 0;
  char path[128] = {};
  char error[64] = {};
};

#if ENABLE_DICTATION || ENABLE_LOCAL_STT
// Capture the setting and named initiating user, never the Pi's UART actor.
// No filesystem I/O. Unresolvable identities fail saving, not transcription.
TranscriptOptions transcriptCaptureOptions(CommandSource source,
                                            TransportSessionEpoch epoch);

// One worker owns each instance. All methods except begin/snapshot can block
// on bounded filesystem locking; never call from a critical section, display
// task, audio callback or UART callback. Each append opens/writes/closes so no
// live File or destructor-dependent resource survives a FreeRTOS self-delete.
class TranscriptSession {
 public:
  void begin(const TranscriptOptions& options, const char* provider, uint64_t id);
  bool append(uint32_t sequence, const char* text, size_t length);
  void finish(const char* outcome);
  const TranscriptStatus& snapshot() const { return status_; }
 private:
  bool fail(const char* reason);
  bool write(const char* text, size_t length, bool first);
  TranscriptOptions options_;
  TranscriptStatus status_;
  uint64_t id_ = 0;
  uint32_t userId_ = 0;
  uint32_t sequence_ = 0;
  bool finished_ = false;
  bool sd_ = false;
  char provider_[8] = {};
  char start_[24] = {};
};
#endif
