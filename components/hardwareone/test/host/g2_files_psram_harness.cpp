// Production definitions are inserted by test_g2_files_psram.py at every run.
#include "System_PsramBuffer.h"
#include "System_TextPager.h"
#include "System_TextWrapStream.h"
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <climits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

static size_t checks = 0;
static void check(bool condition, const char* why) {
  ++checks;
  if (!condition) throw std::runtime_error(why);
}
static void debugStub(const char*, ...) {}
#define DEBUG_G2F(...) debugStub(__VA_ARGS__)
#define WARN_SYSTEMF(...) debugStub(__VA_ARGS__)

// Exact-capacity String boundary mock: exercises the real reveal wrapper's
// reserve/concat/remove protocol; it is not an Arduino allocator model.
static bool failStringReserve = false;
class String {
 public:
  explicit String(const char* text = "") {
    const size_t n = strlen(text);
    check(reserve(n), "fixture String allocation");
    memcpy(bytes_, text, n + 1);
    length_ = n;
  }
  ~String() { free(bytes_); }
  String(const String&) = delete;
  String& operator=(const String&) = delete;
  const char* c_str() const { return bytes_; }
  size_t length() const { return length_; }
  bool reserve(size_t length) {
    if (capacity_ >= length + 1) return true;
    if (failStringReserve) return false;
    char* grown = static_cast<char*>(realloc(bytes_, length + 1));
    if (!grown) return false;
    bytes_ = grown;
    capacity_ = length + 1;
    bytes_[length_] = '\0';
    return true;
  }
  bool concat(const char* bytes, unsigned int count) {
    if (!reserve(length_ + count)) return false;
    memcpy(bytes_ + length_, bytes, count);
    length_ += count;
    bytes_[length_] = '\0';
    return true;
  }
  void remove(size_t length) {
    if (length >= length_) return;
    length_ = length;
    bytes_[length_] = '\0';
  }
 private:
  char* bytes_ = nullptr;
  size_t length_ = 0, capacity_ = 0;
};
// Include the real Arduino String reader after declaring the boundary mock.
// This proves embedded-NUL parsing parity with the former String input path.
#include <ArduinoJson/Deserialization/Readers/ArduinoStringReader.hpp>

// Mock heap-capability backends; routing/fallback is the real MemUtil.cpp.
static std::map<const void*, bool> externalRegions;
static bool failExternal = false, failInternal = false;
static bool failAfterRead = false, haveExternal = true;
static size_t externalCalls = 0, internalCalls = 0, jsonCalls = 0, inputCalls = 0;
static bool inputWasExternal = false;
static void* heapMock(void* old, size_t n, uint32_t caps, bool zero = false) {
  const bool ext = (caps & MALLOC_CAP_SPIRAM) != 0;
  ext ? ++externalCalls : ++internalCalls;
  if (ext ? failExternal : failInternal) return nullptr;
  externalRegions.erase(old);
  void* result = zero ? calloc(1, n) : realloc(old, n);
  if (result) externalRegions[result] = ext;
  return result;
}
extern "C" size_t heap_caps_get_total_size(uint32_t caps) {
  return (caps & MALLOC_CAP_SPIRAM) && !haveExternal ? 0 : 8 * 1024 * 1024;
}
extern "C" size_t heap_caps_get_free_size(uint32_t c) { return heap_caps_get_total_size(c); }
extern "C" size_t heap_caps_get_minimum_free_size(uint32_t c) { return heap_caps_get_total_size(c); }
extern "C" size_t heap_caps_get_largest_free_block(uint32_t c) { return heap_caps_get_total_size(c); }
extern "C" bool esp_ptr_external_ram(const void* p) {
  const auto it = externalRegions.find(p);
  return it != externalRegions.end() && it->second;
}
extern "C" void* heap_caps_malloc(size_t n, uint32_t c) { return heapMock(nullptr, n, c); }
extern "C" void* heap_caps_calloc(size_t n, size_t s, uint32_t c) { return heapMock(nullptr, n * s, c, true); }
extern "C" void* heap_caps_realloc(void* p, size_t n, uint32_t c) { return heapMock(p, n, c); }
extern "C" void* heap_caps_malloc_prefer(size_t n, size_t count, ...) {
  va_list args;
  va_start(args, count);
  void* p = nullptr;
  for (size_t i = 0; i < count; ++i) {
    const auto c = va_arg(args, uint32_t);
    if (!p) p = heapMock(nullptr, n, c);
  }
  va_end(args);
  return p;
}
extern "C" void* heap_caps_calloc_prefer(size_t n, size_t s, size_t count, ...) {
  va_list args;
  va_start(args, count);
  void* p = nullptr;
  for (size_t i = 0; i < count; ++i) {
    const auto c = va_arg(args, uint32_t);
    if (!p) p = heapMock(nullptr, n * s, c, true);
  }
  va_end(args);
  return p;
}
extern "C" void* heap_caps_realloc_prefer(void* old, size_t n, size_t count, ...) {
  va_list args;
  va_start(args, count);
  void* p = nullptr;
  for (size_t i = 0; i < count; ++i) {
    const auto c = va_arg(args, uint32_t);
    if (!p) p = heapMock(old, n, c);
  }
  va_end(args);
  return p;
}
extern "C" void memAllocDebug(const char*, void* p, size_t, bool requested,
                              bool usedPS, bool, const char* tag) {
  check(requested, "G2 and JSON allocations prefer PSRAM");
  if (tag && !strcmp(tag, "json.pool")) ++jsonCalls;
  if (tag && !strcmp(tag, "g2.files.input")) {
    ++inputCalls;
    if (p) inputWasExternal = usedPS;
  }
}

// INSERT_PRODUCTION_CRYPTO_MACROS_HERE
static constexpr size_t kMagicPfxLen = sizeof(CAPCRYPT_MAGIC_PREFIX) - 1;
static constexpr size_t kPrefixLen = sizeof(CAPCRYPT_ROW_PREFIX) - 1;
static constexpr char kUndecryptable[] = CAPCRYPT_UNDECRYPTABLE;
static bool haveKey = true;
static size_t openCalls = 0, keyCalls = 0;
static char plaintext[1024];
static bool captureCryptoEnsureKey() { ++keyCalls; return haveKey; }
static int scratchDepth = 0;
struct ScratchGuard {
  ScratchGuard() { ++scratchDepth; }
  ~ScratchGuard() { --scratchDepth; }
};
static char* plainScratch() { return plaintext; }
static int openLineLocked(const char* row, size_t n) {
  check(scratchDepth == 1, "shared decrypt scratch remains mutex-protected");
  ++openCalls;
  const std::string line(row, n);
  if (line == "ENC1:VALID-ENOUGH-LENGTH-FOR-OPENED-ROW") {
    strcpy(plaintext, "opened,row");
    return 10;
  }
  if (line == "ENC1:EMPTY-ENOUGH-LENGTH") { plaintext[0] = 0; return 0; }
  return -1;
}
bool captureCryptoRevealText(char*, size_t&, size_t, size_t* = nullptr);
// INSERT_PRODUCTION_CRYPTO_REVEAL_HERE

// Filesystem, identity, and rendering boundary mocks.
static bool identity = false, allowed = true, fileExists = true;
static int fsDepth = 0;
static size_t opens = 0, reads = 0, renders = 0, blockedPages = 0;
static size_t maxReadChunk = SIZE_MAX, readStop = SIZE_MAX;
static std::string fileContents;
struct G2HijackCtxGuard {
  G2HijackCtxGuard() { check(!identity, "new paired identity scope"); identity = true; }
  ~G2HijackCtxGuard() { identity = false; }
};
struct FsLockGuard {
  explicit FsLockGuard(const char*) { ++fsDepth; }
  ~FsLockGuard() { --fsDepth; }
};
struct File {
  size_t position = 0;
  explicit operator bool() const { return fileExists; }
  size_t size() const { return fileContents.size(); }
  size_t readBytes(char* dst, size_t n) {
    check(fsDepth == 1 && identity, "reader retains lock and paired identity");
    ++reads;
    if (position >= readStop) return 0;
    n = std::min({n, maxReadChunk, fileContents.size() - position, readStop - position});
    memcpy(dst, fileContents.data() + position, n);
    position += n;
    return n;
  }
  void close() {
    if (failAfterRead) failExternal = failInternal = true;
  }
};
namespace VFS {
static File open(const String&, const char*) {
  check(fsDepth == 1 && identity, "VFS dispatch follows lock and authorization");
  ++opens;
  return {};
}
}
static int currentAuthContext() { check(identity, "read gate uses paired identity"); return 0; }
static bool canRead(const String&, int) { return allowed; }
#define FILE_MANAGER_MAX_PATH 256
#define FILES_ROW_LEN 48
#define EXT_RAM_BSS_ATTR
static constexpr size_t G2_TEXT_DEFAULT_COLS = 48;
static constexpr int G2_GEOM_LARGE = 0;
// INSERT_PRODUCTION_G2_STATE_HERE
static void buildChooserPath(char* out, size_t capacity) { snprintf(out, capacity, "/sd/test.json"); }
static void exitTextViewBackToFiles() {}
static bool g2ShowTextPage(const char*, int, void(*)(), void*) { ++blockedPages; return true; }
static bool filesRenderTextPage() {
  check(fsDepth == 0 && identity, "render outside filesystem lock, inside paired identity scope");
  check(sizeof(gFilesTextPageBuf) == 304, "render scratch capacity unchanged");
  snprintf(gFilesTextPageBuf, sizeof(gFilesTextPageBuf), "%s", gFilesTextTitle);
  ++renders;
  return true;
}
// INSERT_PRODUCTION_G2_VIEWER_HERE

static void reset() {
  failExternal = failInternal = failAfterRead = false;
  haveExternal = haveKey = fileExists = allowed = true;
  failStringReserve = false;
  inputWasExternal = false;
  externalCalls = internalCalls = inputCalls = jsonCalls = 0;
  opens = reads = renders = blockedPages = openCalls = keyCalls = 0;
  maxReadChunk = readStop = SIZE_MAX;
  externalRegions.clear();
  setPsramBypass(false);
  check(!identity && fsDepth == 0, "previous scopes released");
}
static void expectReveal(const std::string& source, const std::string& expected, size_t rows) {
  const size_t cap = captureCryptoRevealCapacity(source.data(), source.size());
  check(cap >= expected.size() + 1, "preflight accounts for output growth");
  auto bytes = std::make_unique<char[]>(cap); // exact allocation: ASan checks bounds
  memcpy(bytes.get(), source.c_str(), source.size() + 1);
  size_t len = source.size(), encountered = SIZE_MAX;
  check(captureCryptoRevealText(bytes.get(), len, cap, &encountered), "bounded reveal succeeds");
  check(encountered == rows && len == expected.size(), "row count/output length exact");
  check(std::string(bytes.get(), len) == expected && bytes[len] == '\0', "output/tail/NUL preserved");
  String text(source.c_str());
  check(captureCryptoRevealText(text) == rows, "legacy String row count preserved");
  check(text.length() == expected.size() && std::string(text.c_str()) == expected,
        "legacy String length grows and shrinks correctly");
}
static void testReveal() {
  reset();
  expectReveal("hello\nENC1:x\ntail", "hello\nENC1:x\ntail", 0);
  check(keyCalls == 0 && openCalls == 0, "ordinary text never opens ciphertext");
  expectReveal("#HW1ENC v1 k1\nENC1:x", "#HW1ENC v1 k1\n[undecryptable row]", 1);
  expectReveal("#HW1ENC\nENC1:x\nFOLLOWING TEXT\nENC1:y\n",
               "#HW1ENC\n[undecryptable row]\nFOLLOWING TEXT\n[undecryptable row]\n", 2);
  expectReveal("#HW1ENC\nENC1:\nplain", "#HW1ENC\nENC1:\nplain", 0);
  expectReveal("#HW1ENC\nENC1:VALID-ENOUGH-LENGTH-FOR-OPENED-ROW\nENC1:x\ntail",
               "#HW1ENC\nopened,row\n[undecryptable row]\ntail", 2);
  expectReveal("#HW1ENC\nENC1:EMPTY-ENOUGH-LENGTH\nend", "#HW1ENC\n\nend", 1);
  expectReveal("#HW1ENC\nENC1:xxxxxxxxxxxxxxxxxxxxxxxxxxxx\r\nend",
               "#HW1ENC\n[undecryptable row]\nend", 1);
  haveKey = false;
  expectReveal("#HW1ENC\nENC1:x\nENC1:VALID-ENOUGH-LENGTH-FOR-OPENED-ROW\ntail",
               "#HW1ENC\n[undecryptable row]\n[undecryptable row]\ntail", 2);
  const std::string damaged = "#HW1ENC\nENC1:x\nTAIL";
  std::vector<char> tooSmall(damaged.begin(), damaged.end());
  tooSmall.push_back(0);
  size_t len = damaged.size(), rows = 99;
  check(!captureCryptoRevealText(tooSmall.data(), len, tooSmall.size(), &rows), "small capacity rejected");
  check(len == damaged.size() && rows == 0 && std::string(tooSmall.data()) == damaged,
        "capacity failure leaves original input unchanged");
  String text(damaged.c_str());
  failStringReserve = true;
  check(captureCryptoRevealText(text) == 0 && std::string(text.c_str()) == damaged,
        "legacy String reserve failure leaves input sealed and intact");
  failStringReserve = false;
  check(captureCryptoRevealCapacity(nullptr, 0) == 0, "invalid input rejected");
  check(captureCryptoRevealCapacity("x", SIZE_MAX) == 0, "size overflow rejected before access");
  char oneByte = 'x';
  size_t impossibleLength = SIZE_MAX;
  check(!captureCryptoRevealText(&oneByte, impossibleLength, 1),
        "bounded entry rejects length outside caller storage before scanning");
  // Every short/truncated row length and newline shape exercises the expansion budget.
  for (size_t n = 0; n <= 32; ++n) {
    const std::string row = "ENC1:" + std::string(n, 'x');
    for (bool newline : {false, true}) {
      const std::string tail = newline ? "\nTAIL\n" : "";
      expectReveal("#HW1ENC\n" + row + tail,
                   "#HW1ENC\n" + (n ? "[undecryptable row]" : row) + tail, n ? 1 : 0);
    }
  }
}
static void expectWrap(const std::string& source, size_t cap, size_t cols, size_t indent, bool strip) {
  std::vector<char> reference(cap, 0), actual(cap, 0);
  bool truncated = false;
  const size_t expectedLen = textWrapInto(reference.data(), cap, source.c_str(), cols, indent, strip, &truncated);
  for (size_t chunk : {size_t(1), size_t(2), size_t(7), size_t(128)}) {
    TextWrapStream sink(actual.data(), cap, cols, indent, strip);
    for (size_t pos = 0; pos < source.size(); pos += chunk)
      sink.write(source.data() + pos, std::min(chunk, source.size() - pos));
    check(sink.size() == expectedLen && sink.truncated() == truncated, "stream matches legacy length/truncation");
    check(!memcmp(reference.data(), actual.data(), expectedLen + 1), "stream matches legacy wrapped bytes");
  }
}
static void testWrap() {
  for (size_t cap : {size_t(1), size_t(2), size_t(9), size_t(48), size_t(100), size_t(4352)}) {
    for (size_t cols : {size_t(0), size_t(1), size_t(8), size_t(48), SIZE_MAX}) {
      for (size_t indent : {size_t(0), size_t(2), size_t(50)}) {
        for (bool strip : {false, true}) {
          expectWrap("", cap, cols, indent, strip);
          expectWrap("abc\r\n\tdef\001\177ghijklmnop\n\nEND", cap, cols, indent, strip);
          expectWrap(std::string(100, 'x') + "\nEND", cap, cols, indent, strip);
          expectWrap(std::string("abc\0ignored", 11), cap, cols, indent, strip);
        }
      }
    }
  }
}
static std::string wrapped(const std::string& input) {
  char output[FILES_TEXT_BODY_CAP];
  bool ignored;
  textWrapInto(output, sizeof(output), input.c_str(), G2_TEXT_DEFAULT_COLS, 0, true, &ignored);
  return output;
}
static void testViewer() {
  reset();
  fileContents = "first\r\n\tsecond\001\n" + std::string(100, 'x');
  maxReadChunk = 7;
  check(showTextFileViaWidget(false), "raw viewer succeeds on short reads");
  check(std::string(gFilesTextBody) == wrapped(fileContents), "raw output unchanged without display copy");
  check(inputCalls == 1 && jsonCalls == 0 && reads > 1, "raw has one input allocation and no JSON work");
  check(inputWasExternal == hasPSRAMAvail(), "input uses PSRAM only on capable build");
  check(gFilesPager.pageCount > 0 && !gFilesPager.truncated, "small file page contract");
  check(!strcmp(gFilesTextTitle, "test.json"), "raw title unchanged");
  reset();
  fileContents = "{\"array\":[1,true,null,\"hello world\"],\"object\":{\"long\":\"" + std::string(100, 'x') + "\"}}";
  JsonDocument reference;
  check(!deserializeJson(reference, fileContents), "fixture parses");
  std::string pretty;
  serializeJsonPretty(reference, pretty);
  check(showTextFileViaWidget(true), "pretty viewer succeeds");
  check(std::string(gFilesTextBody) == wrapped(pretty), "streamed pretty matches prior pretty-then-wrap");
  check(jsonCalls > 0 && !strcmp(gFilesTextTitle, "Pretty test.json"), "JSON allocator and title preserved");
  for (const std::string& input : {
      std::string("{\"value\":\"before") + '\0' + "after\"}",
      std::string("{\"value\":1}") + '\0' + "trailing",
      std::string("{\"value\":") + '\0' + "false}",
      std::string("{\"truncated\": [1,2,")}) {
    reset();
    fileContents = input;
    String oldInput;
    check(oldInput.concat(input.data(), static_cast<unsigned int>(input.size())), "legacy JSON fixture copied");
    JsonDocument oldDocument;
    const auto oldError = deserializeJson(oldDocument, oldInput);
    std::string oldDisplay;
    if (oldError) {
      oldDisplay = "// Pretty parse failed: " + std::string(oldError.c_str()) +
          "\n// Falling back to raw JSON text\n\n" + input;
    } else {
      serializeJsonPretty(oldDocument, oldDisplay);
    }
    check(showTextFileViaWidget(true) && std::string(gFilesTextBody) == wrapped(oldDisplay),
          "embedded-NUL/truncated JSON matches actual legacy Arduino String reader");
  }
  reset();
  fileContents = "{\"partial\": [1,2,";
  const auto error = deserializeJson(reference, fileContents);
  const std::string fallback = "// Pretty parse failed: " + std::string(error.c_str()) +
      "\n// Falling back to raw JSON text\n\n" + fileContents;
  check(showTextFileViaWidget(true) && std::string(gFilesTextBody) == wrapped(fallback),
        "malformed JSON retains full raw fallback and prefix");
  reset();
  fileContents = "{\"a\":1}";
  failAfterRead = true;
  check(showTextFileViaWidget(true), "JSON OOM still shows raw fallback");
  check(std::string(gFilesTextBody) == wrapped("// Pretty parse failed: NoMemory\n// Falling back to raw JSON text\n\n" + fileContents),
        "JSON OOM fallback is explicit and original input unchanged");
  reset();
  fileContents = "fallback";
  failExternal = true;
  check(showTextFileViaWidget(false) && !inputWasExternal && internalCalls > 0,
        "unavailable PSRAM falls back to internal and displays correctly");
  reset();
  haveExternal = false;
  check(showTextFileViaWidget(false) && externalCalls == 0, "unregistered external heap is never required");
  reset();
  setPsramBypass(true);
  check(showTextFileViaWidget(false) && externalCalls == 0, "global allocator bypass honored");
  reset();
  failExternal = failInternal = true;
  check(!showTextFileViaWidget(false) && renders == 0 && reads == 0, "input OOM publishes no partial viewer");
  reset();
  fileContents = "#HW1ENC\n";
  for (size_t i = 0; i < 40; ++i) fileContents += "ENC1:x\n";
  failAfterRead = true;
  check(!showTextFileViaWidget(false) && renders == 0, "reveal growth OOM publishes no corrupt viewer");
  reset();
  fileContents = "#HW1ENC\nENC1:x\nTAIL";
  check(showTextFileViaWidget(false) && std::string(gFilesTextBody) == "#HW1ENC\n[undecryptable row]\nTAIL",
        "damaged capture retains following text in G2");
  reset();
  allowed = false;
  check(showTextFileViaWidget(true) && opens == 0 && keyCalls == 0 && blockedPages == 1,
        "denied file never opens, decrypts, or allocates input");
  reset();
  fileExists = false;
  check(!showTextFileViaWidget(false) && reads == 0 && renders == 0, "missing file has no partial display");
  reset();
  fileContents.clear();
  check(showTextFileViaWidget(false) && gFilesPager.pageCount == 1 && gFilesTextPageOff[1] == 0,
        "empty file retains one empty page");
  reset();
  fileContents.assign(13000, '\r');
  check(showTextFileViaWidget(false) && gFilesPager.truncated && gFilesPager.pageCount == 1,
        "12 KiB read cap recorded even when sanitizing removes all bytes");
  reset();
  fileContents.assign(13000, 'x');
  check(showTextFileViaWidget(false) && strlen(gFilesTextBody) == FILES_TEXT_BODY_CAP - 1 &&
        gFilesPager.truncated && gFilesPager.pageCount == FILES_TEXT_MAX_PAGES,
        "body and page bounds preserve truncation");
  for (int i = 1; i <= gFilesPager.pageCount; ++i)
    check(gFilesTextPageOff[i] > gFilesTextPageOff[i - 1] &&
          gFilesTextPageOff[i] - gFilesTextPageOff[i - 1] <= FILES_TEXT_PAGE_BUDGET,
          "page offsets remain monotonic and within BLE page budget");
  reset();
  fileContents = "#HW1ENC\n";
  while (fileContents.size() < kFilesTextReadCapBytes)
    fileContents += "ENC1:VALID-ENOUGH-LENGTH-FOR-OPENED-ROW\n";
  check(showTextFileViaWidget(false) && gFilesPager.truncated, "read-cap flag survives shrinking decryption");
}
int main() {
  testReveal();
  testWrap();
  testViewer();
  printf("G2 Files PSRAM + capture reveal: %zu checks passed (%s)\n", checks,
         hasPSRAMAvail() ? "PSRAM capable" : "no PSRAM build");
}
