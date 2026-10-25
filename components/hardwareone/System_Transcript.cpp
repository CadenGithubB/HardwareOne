#include "System_Transcript.h"
#if ENABLE_DICTATION || ENABLE_LOCAL_STT
#include "System_AuthIdentity.h"
#include "System_BootState.h"
#include "System_Clock.h"
#include "System_Mutex.h"
#include "System_Settings.h"
#include "System_VFS.h"
#include "System_Utils.h"
#include "Transcript_TextPolicy.h"
#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
#include "BLE_Peers.h"
#endif
#include <cstdio>
#include <cstring>
#include <ctime>

extern uint32_t gBootCounter;  // HardwareOne.cpp: the NVS boot counter, "boot #N"

static void transcriptStartTime(char* out, size_t size) {
  const time_t now = Clock::epochSeconds();
  struct tm utc{};
  if (Clock::isSynced() && gmtime_r(&now, &utc))
    strftime(out, size, "%Y%m%dT%H%M%SZ", &utc);
  else snprintf(out, size, "boot");
}

TranscriptOptions transcriptCaptureOptions(CommandSource source, TransportSessionEpoch epoch) {
  TranscriptOptions options;
  options.enabled = gSettings.sttSaveTranscripts;
  options.source = source;
  options.epoch = epoch;
  if (!options.enabled || !epoch) return options;
  transcriptStartTime(options.started, sizeof(options.started));
  (void)transcriptCaptureIdentity(source, epoch, options.user, sizeof(options.user));
  return options;
}

bool transcriptCaptureIdentity(CommandSource source, TransportSessionEpoch epoch,
                               char* out, size_t cap) {
  if (!out || !cap) return false;
  out[0] = '\0';
  if (!epoch) return false;
  String user;
  if (source == SOURCE_LOCAL_DISPLAY) {
    bool authed = false;
    const auto current = localDisplayTransportSessionSnapshot(user, authed);
    if (!authed || current != epoch) secureClearString(user);
#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
  } else if (source == SOURCE_G2_GLASSES) {
    BlePeerOwnerSession owner;
    if (blePeerOwnerSessionSnapshot(BLE_PEER_G2_GLASSES, owner) &&
        owner.live() && owner.transportEpoch == epoch) user = owner.user;
    secureClearString(owner.user);
#endif
  } else {
    const auto& ctx = currentAuthContext();
    if (ctx.transport == source && captureTransportSessionEpoch(ctx) == epoch) user = ctx.user;
  }
  const bool resolved = user.length() && user.length() < cap &&
      transportSessionEpochIsLive(source, epoch);
  if (resolved) memcpy(out, user.c_str(), user.length() + 1);
  secureClearString(user);
  return resolved;
}

void TranscriptSession::begin(const TranscriptOptions& options, const char* provider, uint64_t id) {
  options_ = options;
  status_ = {};
  status_.enabled = options.enabled;
  id_ = id;
  userId_ = sequence_ = 0;
  finished_ = sd_ = false;
  snprintf(provider_, sizeof(provider_), "%s", provider ? provider : "");
  // Stamp the session's beginning, not its first recognized phrase. UTC remains
  // unambiguous across timezone changes. The random boot nonce/run ID is also
  // present when the clock is unsynchronized or several sessions share a second.
  if (options.started[0] && strnlen(options.started, sizeof(options.started)) < sizeof(options.started))
    snprintf(start_, sizeof(start_), "%s", options.started);
  else transcriptStartTime(start_, sizeof(start_));
  // Startup ordering for an offline device: with "Started: boot" the boot and
  // power-on counters and this boot's session number still put sessions in
  // order, within one NVS ordering domain. Read here, before any audio, never
  // on the first write.
  boot_ = gBootCounter;
  powerOn_ = bootStateGetPowerOnCount();
  domain_ = bootStateGetOrderingDomain();
  if (options.enabled && (!id || !options.epoch || !options.user[0] ||
      strnlen(options.user, sizeof(options.user)) == sizeof(options.user) ||
      (strcmp(provider_, "local") && strcmp(provider_, "pi"))))
    fail("Transcript owner unavailable");
}

bool TranscriptSession::fail(const char* reason) {
  if (!status_.error[0]) snprintf(status_.error, sizeof(status_.error), "%s", reason);
  return false;
}

bool TranscriptSession::write(const char* text, size_t length, bool first) {
  if (length > UINT32_MAX - status_.bytes) return fail("Transcript size limit reached");
  FsLockGuard lock("stt.transcript", pdMS_TO_TICKS(1000));
  if (!lock.held && !isFsLockedByCurrentTask()) return fail("Transcript storage busy");
  // A deferred first write cannot resolve a newly logged-in user's identity.
  // Once bound, account-ID checks also prevent username deletion/recreation
  // from granting ownership of an older transcript.
  if (first && !transportSessionEpochIsLive(options_.source, options_.epoch))
    return fail("Transcript session expired before saving");
  uint32_t currentId = 0;
  if (!getUserIdByUsername(options_.user, currentId) || !currentId ||
      (userId_ && currentId != userId_)) return fail("Transcript account unavailable");
  if (first && !transportSessionEpochIsLive(options_.source, options_.epoch))
    return fail("Transcript session expired before saving");
  if (first) {
    userId_ = currentId;
    sd_ = VFS::isSDWritable();
    snprintf(status_.path, sizeof(status_.path), "%s/u%lu/%s-%s-%08lx%08lx.txt",
             sd_ ? "/sd/stt" : "/stt", static_cast<unsigned long>(userId_), start_, provider_,
             static_cast<unsigned long>(id_ >> 32), static_cast<unsigned long>(id_ & 0xffffffffu));
  }
  const char* root = sd_ ? "/sd/stt" : "/stt";
  char folder[32];
  snprintf(folder, sizeof(folder), "%s/u%lu", root, static_cast<unsigned long>(userId_));
  AuthContext ctx{};
  ctx.transport = options_.source;
  ctx.user = options_.user;
  ctx.scope = folder;
  uint64_t total = 0, used = 0, free = 0;
  constexpr uint64_t reserve = 100 * 1024;
  // Fresh capacity, no tier switching and no automatic deletion/rotation.
  if (!VFS::getStats(sd_ ? VFS::SDCARD : VFS::INTERNAL, total, used, free) ||
      free < reserve + length + 256) return fail("Transcript storage full or unavailable");
  if (first) {
    // trusted: only create the protected transcript root; owner folder/file
    // creation below always uses the admitted named user's guarded context.
    if (!VFS::exists(root) && !VFS::mkdirGuarded(root, VFS::systemAuth(root, "transcript root")))
      return fail("Cannot create transcript folder");
    if (!VFS::existsGuarded(folder, ctx) && !VFS::mkdirGuarded(folder, ctx))
      return fail("Cannot create transcript folder");
    if (VFS::exists(status_.path)) return fail("Transcript filename already exists");
  }
  if (!first && !VFS::existsGuarded(status_.path, ctx)) return fail("Transcript file missing");
  File file = VFS::openGuarded(status_.path, first ? "w" : "a", ctx, true);
  if (!file || file.isDirectory()) return fail("Cannot open transcript file");
  if (!first && file.size() != status_.bytes) {
    file.close();
    return fail("Transcript file changed during session");
  }
  status_.saved = true;
  size_t expected = length;
  size_t written = 0;
  if (first) {
    // The header names the session's place in time two ways: the UTC start
    // when the clock was synced ("boot" otherwise) and the startup counters
    // (Transcript_TextPolicy.h and System_BootState.h describe them). Session
    // is this boot's run number (the low half of the run id in the file name).
    // The title is the first phrase's leading words, the same rule as a note.
    char header[352];
    const int n = snprintf(header, sizeof(header),
        "HardwareOne transcript\nStarted: %s\nBackend: %s\nBoot: %lu\nPower-on: %lu\nSession: %lu\nDomain: %08lx\nTitle: %s\n"
        "An End marker below confirms finalization.\n\n", start_, provider_,
        static_cast<unsigned long>(boot_), static_cast<unsigned long>(powerOn_),
        static_cast<unsigned long>(id_ & 0xffffffffu), static_cast<unsigned long>(domain_), title_);
    if (n < 0 || static_cast<size_t>(n) >= sizeof(header)) {
      file.close();
      return fail("Transcript header too long");
    }
    expected += static_cast<size_t>(n);
    written = file.write(reinterpret_cast<const uint8_t*>(header), n);
    if (written != static_cast<size_t>(n)) {
      file.close();
      if (sd_) VFS::noteSDWriteFailure("transcript header");
      else VFS::noteLittleFsBytesWritten(written);
      status_.bytes += written;
      return fail("Transcript write incomplete");
    }
  }
  written += file.write(reinterpret_cast<const uint8_t*>(text), length);
  file.flush();
  const bool ok = written == expected && !file.getWriteError() &&
      file.size() == status_.bytes + written;
  file.close();
  status_.bytes += written;
  if (sd_ && !ok) VFS::noteSDWriteFailure("transcript append");
  if (!sd_) VFS::noteLittleFsBytesWritten(written);
  return ok || fail("Transcript write incomplete");
}

bool TranscriptSession::append(uint32_t sequence, const char* text, size_t length) {
  if (!status_.enabled) return true;
  if (finished_ || status_.error[0]) return false;
  if (!sequence || !text || length > 512 || memchr(text, '\0', length) != nullptr)
    return fail("Invalid transcript chunk");
  if (sequence <= sequence_) return true; // Producer replay, independent of UI ACK.
  if (sequence != sequence_ + 1) return fail("Transcript sequence gap");
  sequence_ = sequence;
  if (!length) return true; // No file for silence-only sessions.
  if (!status_.saved) {
    // The title skips a speaker tag ("[Person 1] ") so it names the words.
    const char* words = text;
    const char* close = text[0] == '[' ? static_cast<const char*>(memchr(text, ']', length)) : nullptr;
    if (close && close + 1 < text + length && close[1] == ' ') words = close + 2;
    TranscriptText::title(words, length - size_t(words - text), title_, sizeof(title_));
  }
  char line[514];
  memcpy(line, text, length);
  line[length] = '\n';
  const bool ok = write(line, length + 1, !status_.saved);
  volatile char* wipe = line;
  for (size_t i = 0; i <= length; ++i) wipe[i] = 0;
  if (ok) ++status_.chunks;
  return ok;
}

void TranscriptSession::finish(const char* outcome) {
  if (finished_) return;
  finished_ = true;
  if (!status_.enabled || status_.error[0] || !status_.saved) return;
  if (!outcome || (strcmp(outcome, "done") && strcmp(outcome, "cancelled") && strcmp(outcome, "failed"))) {
    fail("Invalid transcript outcome");
    return;
  }
  char footer[40];
  const int n = snprintf(footer, sizeof(footer), "\n[End: %s]\n", outcome);
  status_.complete = write(footer, static_cast<size_t>(n), false);
}
#endif
