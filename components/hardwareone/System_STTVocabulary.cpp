#include "System_STTVocabulary.h"
#if ENABLE_LOCAL_STT
#include "System_Debug.h"
#include "System_MemUtil.h"
#include "System_Mutex.h"
#include "System_VFS.h"
#include <cstdio>
#include <cstring>
#include <new>

using hw1::transcript::Vocabulary;
using hw1::transcript::Word;

namespace {
constexpr const char* kFileName = "user_vocab.tsv";
constexpr size_t kMaxFileBytes = 48 * 1024;      // 256 lines of DISPLAY<TAB>spoken is under 25 KiB
constexpr size_t kLineBytes = 160;
constexpr uint32_t kLockMs = 250;
struct Store {
  Vocabulary* active = nullptr;    // PSRAM
  SemaphoreHandle_t mutex = nullptr;
  uint32_t generation = 0;
  char path[48] = {};              // "/sd/STT Models/user_vocab.tsv"
  bool sd = false;
};
Store gStore;
portMUX_TYPE gInitMux = portMUX_INITIALIZER_UNLOCKED;

bool fail(char* error, size_t cap, const char* why) {
  if (error && cap) snprintf(error, cap, "%s", why);
  return false;
}
struct Lock {
  bool held = false;
  explicit Lock(uint32_t ms) {
    if (gStore.mutex) held = xSemaphoreTake(gStore.mutex, pdMS_TO_TICKS(ms)) == pdTRUE;
  }
  ~Lock() { if (held) xSemaphoreGive(gStore.mutex); }
};
// The mutex and the active table exist from the first use on, so a reader that
// runs before sttVocabularyInit (or a build that never calls it) sees an empty
// vocabulary rather than a crash.
bool ensure() {
  if (gStore.mutex && gStore.active) return true;
  // Build both outside the critical section (the table is 24 KiB of PSRAM to
  // zero); only the pointer swap is protected, and a loser frees its copy.
  SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
  void* storage = ps_alloc(sizeof(Vocabulary), AllocPref::PreferPSRAM, "stt.vocab");
  Vocabulary* table = storage ? new (storage) Vocabulary : nullptr;
  portENTER_CRITICAL(&gInitMux);
  if (!gStore.mutex && mutex) { gStore.mutex = mutex; mutex = nullptr; }
  if (!gStore.active && table) { gStore.active = table; table = nullptr; }
  portEXIT_CRITICAL(&gInitMux);
  if (mutex) vSemaphoreDelete(mutex);
  if (table) { table->~Vocabulary(); ps_free(storage); }
  return gStore.mutex && gStore.active;
}
AuthContext auth(bool sd, const char* reason) {
  // trusted: the fixed vocabulary file beside the speech model; confined to that folder.
  return VFS::systemAuth(sd ? VFS::Scopes::STT_MODELS_SD : VFS::Scopes::STT_MODELS, reason);
}
void pathFor(bool sd, char* out, size_t cap) {
  snprintf(out, cap, "%s/%s", sd ? VFS::Scopes::STT_MODELS_SD : VFS::Scopes::STT_MODELS, kFileName);
}
// Chooses the file: the card copy when it exists, else the internal copy, else
// (nothing saved yet) the card when it is present. Caller holds the mutex.
void choosePathLocked() {
  char candidate[48];
  for (bool sd : {true, false}) {
    pathFor(sd, candidate, sizeof(candidate));
    if (VFS::existsGuarded(candidate, auth(sd, "stt.vocab_probe"))) {
      gStore.sd = sd; memcpy(gStore.path, candidate, sizeof(candidate)); return;
    }
  }
  gStore.sd = VFS::isSDAvailable();
  pathFor(gStore.sd, gStore.path, sizeof(gStore.path));
}
// Parses the whole file into `into`. False (with error) rejects the file as a
// whole: a vocabulary is replaced completely or not at all.
bool parseFile(Vocabulary& into, char* error, size_t cap) {
  into.clear();
  const AuthContext ctx = auth(gStore.sd, "stt.vocab_read");
  if (!VFS::existsGuarded(gStore.path, ctx)) return true;   // no file: an empty list
  File file = VFS::openGuarded(gStore.path, "r", ctx);
  if (!file || file.isDirectory()) return fail(error, cap, "Vocabulary file unreadable");
  const size_t bytes = file.size();
  if (bytes > kMaxFileBytes) { file.close(); return fail(error, cap, "Vocabulary file too large"); }
  char* text = static_cast<char*>(ps_alloc(bytes + 1, AllocPref::PreferPSRAM, "stt.vocab_file"));
  if (!text) { file.close(); return fail(error, cap, "No memory to read the vocabulary"); }
  size_t got = 0;
  while (got < bytes) {
    const size_t n = file.read(reinterpret_cast<uint8_t*>(text) + got, bytes - got);
    if (!n) break;
    got += n;
  }
  file.close();
  bool ok = got == bytes;
  if (!ok) fail(error, cap, "Vocabulary file read incomplete");
  text[got] = '\0';
  size_t rows = 0;
  for (char* line = text; ok && *line;) {
    char* end = strchr(line, '\n');
    char* next = end ? end + 1 : line + strlen(line);
    if (end) *end = '\0';
    if (end && end > line && end[-1] == '\r') end[-1] = '\0';
    if (*line) {
      if (strlen(line) >= kLineBytes) { ok = fail(error, cap, "Vocabulary line too long"); break; }
      if (++rows > hw1::transcript::kMaxWords) { ok = fail(error, cap, "Vocabulary has more than 256 lines"); break; }
      char* spoken = strchr(line, '\t');
      if (spoken) *spoken++ = '\0';
      char why[96];
      if (!into.put(line, spoken, why, sizeof(why))) {
        if (error && cap) snprintf(error, cap, "Line %u: %s", unsigned(rows), why);
        ok = false; break;
      }
    }
    line = next;
  }
  ps_free(text);
  if (!ok) into.clear();
  return ok;
}
// Writes `list` as the vocabulary file through the staged writer: the previous
// file stays until the new one is complete and checked. Caller holds the mutex.
bool writeFile(const Vocabulary& list, char* error, size_t cap) {
  FsLockGuard lock("stt.vocab", pdMS_TO_TICKS(1000));
  if (!lock.held && !isFsLockedByCurrentTask()) return fail(error, cap, "Storage busy");
  const AuthContext ctx = auth(gStore.sd, "stt.vocab_write");
  const char* folder = gStore.sd ? VFS::Scopes::STT_MODELS_SD : VFS::Scopes::STT_MODELS;
  if (gStore.sd && !VFS::isSDWritable()) return fail(error, cap, "SD card not writable");
  if (!VFS::existsGuarded(folder, ctx) && !VFS::mkdirGuarded(folder, ctx))
    return fail(error, cap, "Cannot create the STT Models folder");
  if (!VFS::canWriteGuarded(gStore.path, ctx)) return fail(error, cap, "Vocabulary file not writable");
  VFS::StagedWrite save("stt.vocab");
  if (!save.begin(gStore.path, ctx)) return fail(error, cap, "Cannot stage the vocabulary file (storage full or unavailable)");
  bool ok = true;
  for (size_t i = 0; ok && i < list.count(); ++i) {
    const Word& w = list.at(i);
    char line[kLineBytes];
    const int n = snprintf(line, sizeof(line), "%s\t%s\n", w.display, w.spoken);
    ok = n > 0 && size_t(n) < sizeof(line) &&
         save.file().write(reinterpret_cast<const uint8_t*>(line), size_t(n)) == size_t(n);
  }
  if (!ok) { save.discard(ctx); return fail(error, cap, "Vocabulary write incomplete; previous file kept"); }
  if (!save.commit(ctx)) return fail(error, cap, "Vocabulary file replacement failed; previous file kept");
  return true;
}
// Replaces the active list with `list` and announces the change to decoders.
void adoptLocked(const Vocabulary& list) {
  *gStore.active = list;
  if (++gStore.generation == 0) ++gStore.generation;
}
bool edit(bool (*change)(Vocabulary&, const char*, const char*, char*, size_t),
          const char* a, const char* b, char* error, size_t cap) {
  if (error && cap) error[0] = '\0';
  if (!ensure()) return fail(error, cap, "Vocabulary unavailable");
  Lock lock(kLockMs);
  if (!lock.held) return fail(error, cap, "Vocabulary busy");
  if (!gStore.path[0]) choosePathLocked();
  // Stage a copy; the active list is untouched until the file is replaced.
  void* storage = ps_alloc(sizeof(Vocabulary), AllocPref::PreferPSRAM, "stt.vocab_edit");
  if (!storage) return fail(error, cap, "No memory to edit the vocabulary");
  Vocabulary* staged = new (storage) Vocabulary(*gStore.active);
  const bool ok = change(*staged, a, b, error, cap) && writeFile(*staged, error, cap);
  if (ok) adoptLocked(*staged);
  staged->~Vocabulary();
  ps_free(storage);
  return ok;
}
bool putChange(Vocabulary& v, const char* display, const char* spoken, char* error, size_t cap) {
  return v.put(display, spoken, error, cap);
}
bool removeChange(Vocabulary& v, const char* spoken, const char*, char* error, size_t cap) {
  return v.remove(spoken) || fail(error, cap, "Word not in the vocabulary");
}
bool reloadLocked(char* error, size_t cap) {
  choosePathLocked();
  void* storage = ps_alloc(sizeof(Vocabulary), AllocPref::PreferPSRAM, "stt.vocab_load");
  if (!storage) return fail(error, cap, "No memory to load the vocabulary");
  Vocabulary* staged = new (storage) Vocabulary;
  const bool ok = parseFile(*staged, error, cap);
  if (ok) adoptLocked(*staged);
  staged->~Vocabulary();
  ps_free(storage);
  return ok;
}
}  // namespace

void sttVocabularyInit() {
  char error[96] = {};
  if (!sttVocabularyReload(error, sizeof(error)))
    WARN_SYSTEMF("[STT] Vocabulary not loaded (%s); recognized text is shown as decoded", error);
  else if (sttVocabularyCount())
    INFO_SYSTEMF("[STT] Vocabulary: %u terms from %s", unsigned(sttVocabularyCount()), sttVocabularyPath());
}

bool sttVocabularyReload(char* error, size_t cap) {
  if (error && cap) error[0] = '\0';
  if (!ensure()) return fail(error, cap, "Vocabulary unavailable");
  Lock lock(kLockMs);
  if (!lock.held) return fail(error, cap, "Vocabulary busy");
  return reloadLocked(error, cap);
}

bool sttVocabularyAdd(const char* display, const char* spoken, char* error, size_t cap) {
  return edit(putChange, display, spoken, error, cap);
}

bool sttVocabularyRemove(const char* spoken, char* error, size_t cap) {
  if (!spoken || !*spoken) return fail(error, cap, "Word not in the vocabulary");
  return edit(removeChange, spoken, nullptr, error, cap);
}

size_t sttVocabularyCount() {
  if (!ensure()) return 0;
  Lock lock(kLockMs);
  return lock.held ? gStore.active->count() : 0;
}

bool sttVocabularyEntry(size_t index, Word* out) {
  if (!out || !ensure()) return false;
  Lock lock(kLockMs);
  if (!lock.held || index >= gStore.active->count()) return false;
  *out = gStore.active->at(index);
  return true;
}

const char* sttVocabularyPath() { return gStore.path; }

uint32_t sttVocabularyGeneration() { return gStore.generation; }

uint32_t sttVocabularyHotwords(char* out, size_t cap, size_t* bytes) {
  if (bytes) *bytes = 0;
  if (!out || !cap) return 0;
  out[0] = '\0';
  if (!ensure()) return 0;
  Lock lock(kLockMs);
  if (!lock.held) return 0;
  size_t used = 0;
  for (size_t i = 0; i < gStore.active->count(); ++i) {
    const char* spoken = gStore.active->at(i).spoken;
    const size_t n = strlen(spoken);
    if (used + n + 1 >= cap) break;
    memcpy(out + used, spoken, n); used += n; out[used++] = '\n';
  }
  out[used] = '\0';
  if (bytes) *bytes = used;
  return gStore.generation;
}

bool sttFormatDisplay(const char* raw, bool paragraphStart, char* out, size_t cap) {
  if (out && cap) out[0] = '\0';
  if (!raw || !out || !cap || !ensure()) return false;
  Lock lock(kLockMs);
  if (!lock.held) return false;
  return hw1::transcript::format(raw, *gStore.active, paragraphStart, out, cap);
}
#endif
