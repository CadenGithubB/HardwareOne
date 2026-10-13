// The Python runner inserts the production tokenizer structs, loader, merge map,
// freeTokenizer, encode and decode unchanged. The File double serves an
// in-memory model image; the PSRAM allocator truncates the requested size to
// 32 bits, the width of size_t on the ESP32, so size arithmetic that wraps on
// the device wraps here too. Run one case by name, or all of them.
#include <algorithm>
#include <cassert>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

// INSERT_TYPES

class File {
 public:
  std::vector<uint8_t> data;
  size_t pos = 0;
  size_t read(uint8_t* out, size_t n) {
    const size_t count = pos >= data.size() ? 0 : std::min(n, data.size() - pos);
    if (count) memcpy(out, data.data() + pos, count);
    pos += count;
    return count;
  }
  bool seek(uint32_t to) {
    if (to > data.size()) return false;
    pos = to;
    return true;
  }
  size_t position() const { return pos; }
  size_t size() const { return data.size(); }
};

static struct {
  TokenizerState tokenizer;
  char* tokenizerData;
  struct { int vocab_size; } config;
} gLLM;

static int errors = 0;
// Log macros consume their arguments, as the device's formatters do.
template <typename... Args> static void logSink(const char*, Args&&...) {}
#define ERROR_LLMF(...) (++errors, logSink(__VA_ARGS__))
#define DEBUG_LLM_TOKENIZERF(...) logSink(__VA_ARGS__)
constexpr uint32_t DEBUG_LLM = 1, DEBUG_LLM_TOKENIZER = 2;
static bool isDebugOutputEnabled(uint32_t) { return false; }
// PSRAM blocks still owned by the tokenizer. The vocab pointer array comes
// from calloc and is also released through llmPsramFree, as on the device.
static std::set<void*> psramLive;
static void* llmPsramAlloc(size_t size, const char*) {
  const uint32_t deviceSize = static_cast<uint32_t>(size);
  void* p = malloc(deviceSize ? deviceSize : 1);
  if (p) psramLive.insert(p);
  return p;
}
static void llmPsramFree(void** p) {
  if (*p) { psramLive.erase(*p); free(*p); }
  *p = nullptr;
}

// INSERT_PRODUCTION

namespace {

struct Merge { uint32_t left, right, merged; };

void put32(std::vector<uint8_t>& out, uint32_t v) {
  for (int i = 0; i < 4; ++i) out.push_back(uint8_t(v >> (8 * i)));
}

struct Blob {
  std::vector<std::string> vocab;
  std::vector<Merge> merges;
  int64_t vocabCountOverride = -1;   // header vocab_size when it differs
  int64_t mergeCountOverride = -1;   // header merge_count when it differs
  int64_t lengthDelta = 0;           // tok_byte_len minus the real blob size
  size_t trailing = 32;              // bytes after the blob (the "weights")
};

const size_t kPrefix = 64;  // header bytes before the tokenizer

File build(const Blob& b) {
  std::vector<uint8_t> body;
  put32(body, b.vocabCountOverride >= 0 ? uint32_t(b.vocabCountOverride) : uint32_t(b.vocab.size()));
  put32(body, b.mergeCountOverride >= 0 ? uint32_t(b.mergeCountOverride) : uint32_t(b.merges.size()));
  for (const auto& s : b.vocab) {
    body.push_back(uint8_t(s.size()));
    body.insert(body.end(), s.begin(), s.end());
  }
  for (const auto& m : b.merges) { put32(body, m.left); put32(body, m.right); put32(body, m.merged); }
  File f;
  f.data.assign(kPrefix, 0);
  put32(f.data, uint32_t(int64_t(body.size()) + b.lengthDelta));
  f.data.insert(f.data.end(), body.begin(), body.end());
  f.data.insert(f.data.end(), b.trailing, 0xEE);
  f.pos = kPrefix;
  return f;
}

Blob sample() {
  Blob b;
  b.vocab = {"a", "b", "c", "ab", "abc", "<0x0A>"};
  b.merges = {{0, 1, 3}, {3, 2, 4}};
  return b;
}

void clean() {
  freeTokenizer();
  assert(!gLLM.tokenizerData && !gLLM.tokenizer.vocab && !gLLM.tokenizer.merges &&
         !gLLM.tokenizer.merge_map && !gLLM.tokenizer.presplit);
  assert(psramLive.empty());
}

void expectRejected(const Blob& b, int modelVocab = 8) {
  gLLM.config.vocab_size = modelVocab;
  errors = 0;
  File f = build(b);
  assert(!loadTokenizerFromFile(f));  // a malformed blob must fail the load
  assert(gLLM.tokenizer.vocab_size == 0 || gLLM.tokenizer.vocab_size <= modelVocab);
  clean();  // llmUnload releases every partial allocation
}

void caseValid() {
  Blob b = sample();
  gLLM.config.vocab_size = 8;
  File f = build(b);
  const size_t end = f.data.size() - b.trailing;
  assert(loadTokenizerFromFile(f));
  assert(f.position() == end);
  assert(gLLM.tokenizer.vocab_size == 6 && gLLM.tokenizer.merge_count == 2);
  int tokens[8] = {};
  assert(encode("abcab", tokens, 8) == 2 && tokens[0] == 4 && tokens[1] == 3);
  assert(std::string(decode(0, 4)) == "abc" && std::string(decode(0, 6)) == "");
  clean();
  // The tokenizer may be smaller than the model's embedding table.
  gLLM.config.vocab_size = 6;
  f = build(b);
  assert(loadTokenizerFromFile(f));
  clean();
}

void caseTruncatedBlob() {
  // tok_byte_len runs past the end of the file.
  Blob b = sample(); b.trailing = 0; b.lengthDelta = 40;
  expectRejected(b);
  // tok_byte_len ends inside the vocab strings.
  b = sample(); b.lengthDelta = -int64_t(2 * 12 + 4);
  expectRejected(b);
  // A blob of the header plus two bytes that still claims six vocab entries.
  b = sample(); b.lengthDelta = -42;
  expectRejected(b);
}

void caseMergeCountPastBlob() {
  Blob b = sample(); b.mergeCountOverride = 1000;
  expectRejected(b);
  b = sample(); b.mergeCountOverride = 1;  // one merge record left over
  expectRejected(b);
}

void caseMergeCountWraps() {
  // 0x15555556 * 12 wraps to 8 bytes in a 32-bit size_t.
  Blob b = sample(); b.mergeCountOverride = 0x15555556;
  expectRejected(b);
}

void caseIdsOutOfRange() {
  for (const Merge& bad : {Merge{6, 1, 3}, Merge{0, 6, 3}, Merge{0, 1, 6}, Merge{0, 1, 0x10003}}) {
    Blob b = sample(); b.merges.push_back(bad);
    expectRejected(b);
  }
}

void caseVocabLargerThanModel() {
  expectRejected(sample(), 5);
}

void caseVocabPast16BitKeys() {
  // 65536 ids still fit the 16-bit merge keys; 65537 would collide.
  Blob b;
  b.vocab.assign(65536, std::string());
  b.vocab[0] = "a"; b.vocab[1] = "b"; b.vocab[65535] = "ab";
  b.merges = {{0, 1, 65535}};
  gLLM.config.vocab_size = 131072;
  File f = build(b);
  assert(loadTokenizerFromFile(f));
  int tokens[4] = {};
  assert(encode("ab", tokens, 4) == 1 && tokens[0] == 65535);
  clean();
  b.vocab.push_back("x");
  b.merges = {{0, 1, 65536}};
  expectRejected(b, 131072);
}

void caseTrailingSlack() {
  Blob b = sample(); b.lengthDelta = 4;  // blob claims 4 bytes nothing reads
  expectRejected(b);
}

struct Case { const char* name; void (*run)(); };
const Case kCases[] = {
  {"valid", caseValid},
  {"truncated_blob", caseTruncatedBlob},
  {"merge_count_past_blob", caseMergeCountPastBlob},
  {"merge_count_wraps", caseMergeCountWraps},
  {"ids_out_of_range", caseIdsOutOfRange},
  {"vocab_larger_than_model", caseVocabLargerThanModel},
  {"vocab_past_16bit_keys", caseVocabPast16BitKeys},
  {"trailing_slack", caseTrailingSlack},
};

}  // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::string(argv[1]) == "--list") {
    for (const Case& c : kCases) std::printf("%s\n", c.name);
    return 0;
  }
  bool ran = false;
  for (const Case& c : kCases) {
    if (argc == 2 && std::string(argv[1]) != c.name) continue;
    c.run();
    ran = true;
    std::printf("PASS %s\n", c.name);
  }
  return ran ? 0 : 2;
}
