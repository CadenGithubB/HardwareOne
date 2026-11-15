// Host harness for the voice text that tasks other than the recognizer read.
//
// test_espsr_voice_text.py inserts, from the production sources, the size
// constants of System_ESPSR.h and, from System_ESPSR.cpp, the recognizer's two
// Strings, the shared text with its lock, voiceSetContext, voiceSetLastCommand,
// voiceTextSnapshot, voiceCopyText and the three public accessors.
// Host boundaries: String, the critical section (a mutex that knows its owner),
// strlcpy (a host copy that fails when the lock is not held) and the esp-sr
// phrase limit.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

static int failures = 0;
#define CHECK(condition, what) do { \
  if (!(condition)) { std::fprintf(stderr, "FAIL (line %d): %s\n", __LINE__, what); ++failures; } \
} while (0)

class String {
 public:
  String(const char* text = "") : text_(text ? text : "") {}
  String(const std::string& text) : text_(text) {}
  const char* c_str() const { return text_.c_str(); }
  size_t length() const { return text_.size(); }
  String operator+(const char* more) const { return String(text_ + more); }
  String operator+(const String& more) const { return String(text_ + more.text_); }
 private:
  std::string text_;
};

#define ESP_MN_MAX_PHRASE_LEN 63

struct portMUX_TYPE {
  std::mutex mutex;
  std::atomic<std::thread::id> owner{};
};
#define portMUX_INITIALIZER_UNLOCKED {}
static std::atomic<unsigned> gLockEntries{0};
static void portENTER_CRITICAL(portMUX_TYPE* mux) {
  mux->mutex.lock();
  mux->owner.store(std::this_thread::get_id());
  gLockEntries.fetch_add(1);
}
static void portEXIT_CRITICAL(portMUX_TYPE* mux) {
  mux->owner.store(std::thread::id());
  mux->mutex.unlock();
}

static bool voiceTextLockHeldByThisThread();
static std::atomic<unsigned> gUnlockedCopies{0};
// Every strlcpy in the inserted production code copies into or out of the
// shared text, so each one must run with the lock held by the copying thread.
static size_t hostStrlcpy(char* out, const char* text, size_t capacity) {
  if (!voiceTextLockHeldByThisThread()) gUnlockedCopies.fetch_add(1);
  const size_t length = std::strlen(text);
  if (capacity) {
    const size_t copied = length < capacity - 1 ? length : capacity - 1;
    std::memcpy(out, text, copied);
    out[copied] = '\0';
  }
  return length;
}
#define strlcpy hostStrlcpy

// INSERT_HEADER
// INSERT_PRODUCTION

static bool voiceTextLockHeldByThisThread() {
  return sVoiceTextMux.owner.load() == std::this_thread::get_id();
}

static std::string category() {
  char out[kEspSrPhraseTextSize];
  getEspSrCurrentCategory(out, sizeof(out));
  return out;
}
static std::string subCategory() {
  char out[kEspSrPhraseTextSize];
  getEspSrCurrentSubCategory(out, sizeof(out));
  return out;
}
static std::string lastCommand() {
  char out[kEspSrLastCommandTextSize];
  getEspSrLastCommand(out, sizeof(out));
  return out;
}

static void testValues() {
  CHECK(category().empty() && subCategory().empty() && lastCommand().empty(), "all three texts start empty");

  voiceSetContext("sensor", "");
  CHECK(category() == "sensor" && subCategory().empty(), "a category is published with an empty subcategory");
  voiceSetContext(nullptr, "thermal camera");
  CHECK(category() == "sensor" && subCategory() == "thermal camera", "a subcategory alone leaves the category");
  CHECK(std::string(gCurrentCategory.c_str()) == "sensor" &&
        std::string(gCurrentSubCategory.c_str()) == "thermal camera",
        "the recognizer's own Strings hold the same values");
  voiceSetLastCommand(gCurrentCategory + " " + gCurrentSubCategory + " " + "open");
  CHECK(lastCommand() == "sensor thermal camera open", "the last command is published as it was built");

  VoiceSharedText all;
  voiceTextSnapshot(all);
  CHECK(std::string(all.category) == "sensor" && std::string(all.subCategory) == "thermal camera" &&
        std::string(all.lastCommand) == "sensor thermal camera open", "a snapshot holds all three texts");

  voiceSetContext("", nullptr);  // srcmdssync
  CHECK(category().empty() && subCategory() == "thermal camera", "emptying the category alone leaves the subcategory");
  voiceSetContext("", "");
  CHECK(category().empty() && subCategory().empty(), "the context is cleared");
  CHECK(std::string(gCurrentCategory.c_str()).empty() && std::string(gCurrentSubCategory.c_str()).empty(),
        "the recognizer's own Strings are cleared with it");
  CHECK(lastCommand() == "sensor thermal camera open", "clearing the context keeps the last command");
}

static void testBounds() {
  // The longest texts MultiNet can produce are published whole.
  const std::string phrase(ESP_MN_MAX_PHRASE_LEN, 'p');
  voiceSetContext(phrase.c_str(), phrase.c_str());
  voiceSetLastCommand(String(phrase + " " + phrase + " " + phrase));
  CHECK(category() == phrase && subCategory() == phrase, "a phrase of the esp-sr limit is published whole");
  CHECK(lastCommand() == phrase + " " + phrase + " " + phrase, "three such phrases and two spaces are published whole");

  // A longer text is cut in the shared copy only.
  const std::string longText(300, 'x');
  voiceSetContext(longText.c_str(), longText.c_str());
  voiceSetLastCommand(String(longText));
  CHECK(category() == std::string(kEspSrPhraseTextSize - 1, 'x'), "an over-long category is cut in the shared copy");
  CHECK(subCategory() == std::string(kEspSrPhraseTextSize - 1, 'x'), "an over-long subcategory is cut in the shared copy");
  CHECK(lastCommand() == std::string(kEspSrLastCommandTextSize - 1, 'x'), "an over-long last command is cut");
  CHECK(gCurrentCategory.length() == 300 && gCurrentSubCategory.length() == 300,
        "the recognizer's own Strings keep the whole text for its lookups");

  // A reader's buffer bounds its copy; nothing is written past it.
  char small[16 + 4];
  std::memset(small, '#', sizeof(small));
  getEspSrLastCommand(small, 16);
  CHECK(std::strlen(small) == 15 && small[15] == '\0', "a 16-byte buffer gets 15 characters and the terminator");
  CHECK(std::memcmp(small + 16, "####", 4) == 0, "nothing is written past the reader's buffer");
  std::memset(small, '#', sizeof(small));
  getEspSrCurrentCategory(small, 0);
  getEspSrCurrentSubCategory(nullptr, 16);
  CHECK(small[0] == '#', "a zero-size or absent buffer is left alone");
  getEspSrCurrentCategory(small, 1);
  CHECK(small[0] == '\0' && small[1] == '#', "a 1-byte buffer gets the terminator only");
  voiceSetContext("", "");
  voiceSetLastCommand(String(""));
}

// A reader or writer must wait while another task is inside the lock.
template <typename Call>
static bool waitsForLock(Call call) {
  std::atomic<bool> done{false};
  sVoiceTextMux.mutex.lock();
  std::thread other([&] { call(); done.store(true); });
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  const bool waited = !done.load();
  sVoiceTextMux.mutex.unlock();
  other.join();
  return waited && done.load();
}

static void testLock() {
  char out[kEspSrLastCommandTextSize];
  VoiceSharedText all;
  CHECK(waitsForLock([&] { getEspSrCurrentCategory(out, sizeof(out)); }), "the category reader takes the lock");
  CHECK(waitsForLock([&] { getEspSrCurrentSubCategory(out, sizeof(out)); }), "the subcategory reader takes the lock");
  CHECK(waitsForLock([&] { getEspSrLastCommand(out, sizeof(out)); }), "the last-command reader takes the lock");
  CHECK(waitsForLock([&] { voiceTextSnapshot(all); }), "the snapshot takes the lock");
  CHECK(waitsForLock([] { voiceSetContext("sensor", "GPS"); }), "the context writer takes the lock");
  CHECK(waitsForLock([] { voiceSetLastCommand(String("sensor GPS open")); }), "the last-command writer takes the lock");
  CHECK(sVoiceTextMux.owner.load() == std::thread::id(), "the lock is released after every call");
}

static bool uniform(const char* text, size_t length) {
  if (std::strlen(text) != length) return false;
  for (size_t i = 1; i < length; ++i) if (text[i] != text[0]) return false;
  return true;
}

// The recognizer rewrites all three texts with full-length values while two
// other tasks read them. A reader must only ever see one whole value: a copy
// made outside the lock mixes two.
static void testReadersDuringWrites() {
  constexpr int kRounds = 60000;
  // Start from empty texts: a reader that runs before the first write must not
  // find the short values the lock test left behind.
  voiceSetContext("", "");
  voiceSetLastCommand(String(""));
  std::atomic<bool> stop{false};
  std::atomic<unsigned> torn{0};
  std::thread recognizer([&] {
    const std::string phrases[2] = {std::string(ESP_MN_MAX_PHRASE_LEN, 'a'), std::string(ESP_MN_MAX_PHRASE_LEN, 'b')};
    const std::string commands[2] = {std::string(kEspSrLastCommandTextSize - 1, 'c'),
                                     std::string(kEspSrLastCommandTextSize - 1, 'd')};
    for (int round = 0; round < kRounds; ++round) {
      voiceSetContext(phrases[round & 1].c_str(), phrases[(round >> 1) & 1].c_str());
      voiceSetLastCommand(String(commands[round & 1]));
    }
    stop.store(true);
  });
  std::thread page([&] {  // the OLED speech page: one text at a time
    char text[kEspSrLastCommandTextSize];
    while (!stop.load()) {
      getEspSrCurrentCategory(text, sizeof(text));
      if (text[0] && !uniform(text, ESP_MN_MAX_PHRASE_LEN)) torn.fetch_add(1);
      getEspSrCurrentSubCategory(text, sizeof(text));
      if (text[0] && !uniform(text, ESP_MN_MAX_PHRASE_LEN)) torn.fetch_add(1);
      getEspSrLastCommand(text, sizeof(text));
      if (text[0] && !uniform(text, kEspSrLastCommandTextSize - 1)) torn.fetch_add(1);
    }
  });
  std::thread status([&] {  // the status JSON: all three at once
    VoiceSharedText all;
    while (!stop.load()) {
      voiceTextSnapshot(all);
      if (all.category[0] && !uniform(all.category, ESP_MN_MAX_PHRASE_LEN)) torn.fetch_add(1);
      if (all.subCategory[0] && !uniform(all.subCategory, ESP_MN_MAX_PHRASE_LEN)) torn.fetch_add(1);
      if (all.lastCommand[0] && !uniform(all.lastCommand, kEspSrLastCommandTextSize - 1)) torn.fetch_add(1);
    }
  });
  recognizer.join();
  page.join();
  status.join();
  CHECK(torn.load() == 0, "a reader never sees a text that mixes two values");
}

int main() {
  CHECK(kEspSrPhraseTextSize == ESP_MN_MAX_PHRASE_LEN + 1, "a phrase buffer holds the esp-sr phrase limit");
  CHECK(kEspSrLastCommandTextSize == 3 * kEspSrPhraseTextSize, "the last command holds three phrases and two spaces");
  CHECK(sizeof(sVoiceText.category) == kEspSrPhraseTextSize && sizeof(sVoiceText.subCategory) == kEspSrPhraseTextSize &&
        sizeof(sVoiceText.lastCommand) == kEspSrLastCommandTextSize, "the shared text uses the header's sizes");
  testValues();
  testBounds();
  testLock();
  testReadersDuringWrites();
  CHECK(gUnlockedCopies.load() == 0, "every copy into or out of the shared text ran with the lock held");
  CHECK(gLockEntries.load() > 0, "the lock was used");
  if (failures) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("voice text: readers on other tasks get whole copies made under the lock\n");
  return 0;
}
