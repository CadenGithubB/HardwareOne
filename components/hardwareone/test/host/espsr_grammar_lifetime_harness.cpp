#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <set>
#include <sstream>
#include <string>
using esp_err_t = int;
constexpr int ESP_MN_MAX_PHRASE_NUM = 400, I2S_SR_SAMPLE_RATE = 16000, kSRMaxCommandId = 1023;
struct esp_mn_phrase_t { char* string; char* phonemes; int16_t command_id; float threshold; int16_t* wave; };
struct esp_mn_error_t { int16_t num; esp_mn_phrase_t** phrases; };
struct Phrase {
  std::string text, phonemes;
  esp_mn_phrase_t value;
  Phrase(int id, const char* t, const char* p): text(t), phonemes(p ? p : ""),
    value{text.data(), p ? phonemes.data() : nullptr, static_cast<int16_t>(id), 0, nullptr} {}
};
struct srmodel_data_t { int num; char** files; char** data; int* sizes; };
struct srmodel_list_t { int num; char** model_name; srmodel_data_t** model_data; };
static char fstName[] = "fst", fileName[] = "commands_en.txt";
static char stock[] = "44,DEHUMIDIFY MODE,DmhYoMgDcFi MbD\n", emptyPack[] = "";
static char* names[] = {fstName}; static char* files[] = {fileName};
static char* data[] = {stock}; static int sizes[] = {sizeof(stock) - 1};
static srmodel_data_t fst{1, files, data, sizes}; static srmodel_data_t* tables[] = {&fst};
static srmodel_list_t models{1, names, tables};
static srmodel_list_t* gSRModels = &models;
static std::deque<Phrase> phrases;
static unsigned created = 0, destroyed = 0, updateCalls = 0, createAttempts = 0, emptyCreateAttempts = 0;
static size_t vendorLiveBytes = 0;
static bool createFails = false, snapshotFails = false, mismatchedTable = false, extraPhrase = false;
static bool badRate = false, badChunk = false, updateFails = false;
static std::set<void*> snapshotBlocks;
struct model_iface_data_t { unsigned generation; size_t currentBuffers = 0; };
static size_t tableBufferBytes() {
  size_t maxId = 0;
  for (auto& p : phrases) if (p.value.command_id > static_cast<int>(maxId)) maxId = p.value.command_id;
  return maxId * 2 * sizeof(int32_t);
}
model_iface_data_t* createModel(const char* name, int duration) {
  assert(!strcmp(name, "mn7_en") && duration == 6000); ++createAttempts;
  if (createFails) return nullptr; // public-return cleanup only, not vendor internal OOM safety
  assert(gSRModels == &models && fst.num == 1 && data[0] && sizes[0] > 0);
  if (!data[0][0]) { ++emptyCreateAttempts; return nullptr; } // real vendor wrapper PANICS here
  phrases.clear(); // create allocates and fills the global command list
  std::istringstream input(std::string(data[0], sizes[0]));
  std::string line;
  while (std::getline(input, line)) {
    assert(!line.empty() && line.size() < 127); // vendor's 128-byte line buffer
    const auto comma = line.find(','), next = line.find(',', comma + 1);
    assert(comma != std::string::npos);
    const int id = std::stoi(line.substr(0, comma));
    std::string text = line.substr(comma + 1, next == std::string::npos ? next : next - comma - 1);
    std::string phonemes = next == std::string::npos ? "" : line.substr(next + 1);
    phrases.emplace_back(id, text.c_str(), next == std::string::npos ? nullptr : phonemes.c_str());
  }
  assert(!phrases.empty());
  auto* model = new model_iface_data_t{++created, tableBufferBytes()};
  vendorLiveBytes += model->currentBuffers; // create itself publishes the graph once
  if (mismatchedTable) phrases.pop_back();
  if (extraPhrase) phrases.emplace_back(2, "unexpected", nullptr);
  return model;
}
void destroyModel(model_iface_data_t* model) {
  assert(model); ++destroyed; vendorLiveBytes -= model->currentBuffers;
  phrases.clear(); delete model;
}
int modelRate(model_iface_data_t*) { return badRate ? 8000 : 16000; }
int modelChunk(model_iface_data_t*) { return badChunk ? 256 : 512; }
struct Interface {
  model_iface_data_t* (*create)(const char*,int); void (*destroy)(model_iface_data_t*);
  int (*get_samp_rate)(model_iface_data_t*); int (*get_samp_chunksize)(model_iface_data_t*);
};
static Interface iface{createModel, destroyModel, modelRate, modelChunk};
static Interface* gMNModel = &iface;
static model_iface_data_t* gMNData = nullptr;
static bool gMNCommandsAllocated = false, gMNGrammarPublished = false, gMnTableTrusted = true;
static std::atomic<bool> gSRFeedShouldRun{true}, gESPSRRunning{true};
static char gMnModelName[64] = "mn7_en";
static size_t gSRFetchSamples = 512;
static struct { int srCommandTimeout = 6000; } gSettings;
static std::string lastError;
void srSetError(const char* value) { lastError = value; }
enum class AllocPref { PreferPSRAM };
void* ps_alloc(size_t size, AllocPref, const char*) {
  if (snapshotFails) return nullptr;
  void* block = malloc(size); assert(block); snapshotBlocks.insert(block); return block;
}
void snapshotFree(void* ptr) { if (ptr) { assert(snapshotBlocks.erase(ptr) == 1); free(ptr); } }
esp_mn_phrase_t* esp_mn_commands_get_from_index(int index) {
  return index >= 0 && static_cast<size_t>(index) < phrases.size() ? &phrases[index].value : nullptr;
}
esp_mn_error_t* esp_mn_commands_update() {
  // Simulate the real replacement leak: overwrites the previous buffer pair.
  assert(gMNData && !phrases.empty()); ++updateCalls;
  gMNData->currentBuffers = tableBufferBytes(); vendorLiveBytes += gMNData->currentBuffers;
  static esp_mn_error_t error{1, nullptr}; return updateFails ? &error : nullptr;
}
static unsigned lockCalls = 0;
bool lockMN(uint32_t) { ++lockCalls; return gMNData != nullptr; }
void unlockMN() {}
#define free snapshotFree
// INSERT_FUNCTIONS
#undef free
static void seed() {
  phrases.clear();
  phrases.emplace_back(990, "cancel", "KaNSbL");
  phrases.emplace_back(992, "help", "HcLP");
  phrases.emplace_back(1, "system", "SgSTcM"); phrases.back().value.threshold = 0.42f;
  phrases.emplace_back(2, "status", nullptr);
}
static void assertTable() {
  assert(phrases.size() == 4);
  assert(phrases[0].value.command_id == 990 && phrases[0].text == "cancel" && phrases[0].phonemes == "KaNSbL");
  assert(phrases[1].value.command_id == 992 && phrases[1].text == "help");
  assert(phrases[2].value.command_id == 1 && phrases[2].text == "system" && phrases[2].phonemes == "SgSTcM");
  assert(phrases[2].value.threshold == 0.42f);
  assert(phrases[3].value.command_id == 2 && phrases[3].text == "status" && !phrases[3].value.phonemes);
}
static void assertMetadataRestored() { assert(data[0] == stock && sizes[0] == sizeof(stock) - 1); }
static void start() {
  assert(!gMNData && vendorLiveBytes == 0 && snapshotBlocks.empty());
  gMNModel = &iface; gSRModels = &models; data[0] = stock; sizes[0] = sizeof(stock) - 1;
  gMnTableTrusted = true; gSRFeedShouldRun = gESPSRRunning = true; lastError.clear();
  const char bootstrap[] = "1,TELL ME A JOKE,TfL Mm c qbK\n";
  gMNData = createMNWithCommandFile(bootstrap, sizeof(bootstrap) - 1);
  assert(gMNData && vendorLiveBytes == 8); assertMetadataRestored();
  gMNCommandsAllocated = true; gMNGrammarPublished = false;
  seed(); assert(mnUpdateLocked()); assertTable();
  assert(vendorLiveBytes == 7936 && gMNGrammarPublished && gMNCommandsAllocated); assertMetadataRestored();
}
static void stop() {
  const unsigned previousLocks = lockCalls; const bool hadModel = gMNData != nullptr;
  assert(deinitMultiNet()); assert(!gMNData && !gMNModel && !gMNGrammarPublished);
  assert(lockCalls == previousLocks + (hadModel ? 1 : 0));
  assert(vendorLiveBytes == 0 && snapshotBlocks.empty());
}
static void testVendorDefects() {
  // A stock graph followed by a direct update leaks the graph created at startup.
  gMNModel = &iface; gMNData = createModel(gMnModelName, 6000); assert(vendorLiveBytes == 352);
  seed(); esp_mn_commands_update(); destroyModel(gMNData); gMNData = nullptr;
  assert(vendorLiveBytes == 352); vendorLiveBytes = 0; updateCalls = 0; // simulated byte ledger
  data[0] = emptyPack; sizes[0] = 1;
  assert(!createModel(gMnModelName, 6000) && emptyCreateAttempts == 1);
  // Runtime override must tolerate either stock OR the installed empty bundle.
  const char bootstrap[] = "1,TELL ME A JOKE,TfL Mm c qbK\n";
  gMNData = createMNWithCommandFile(bootstrap, sizeof(bootstrap) - 1);
  assert(gMNData && emptyCreateAttempts == 1 && data[0] == emptyPack && sizes[0] == 1);
  stop(); data[0] = stock; sizes[0] = sizeof(stock) - 1; emptyCreateAttempts = 0;
}
static void testRepeatedReplacement() {
  start(); const unsigned initialCreated = created;
  for (unsigned i = 0; i < 40; ++i) {
    seed(); assert(mnUpdateLocked()); assertTable(); assertMetadataRestored();
    assert(created == initialCreated + i + 1 && vendorLiveBytes == 7936 && snapshotBlocks.empty());
    assert(updateCalls == 0 && emptyCreateAttempts == 0);
  }
  phrases.clear(); const unsigned attempts = createAttempts;
  assert(mnUpdateLocked()); assert(!gMNGrammarPublished && vendorLiveBytes == 7936 && createAttempts == attempts);
  assert(mnUpdateLocked() && createAttempts == attempts); // no unsafe empty create/update
  seed(); assert(mnUpdateLocked()); assert(gMNGrammarPublished && vendorLiveBytes == 7936);
  stop();
}
static void testCSVLineBoundary() {
  // 1 digit + 2 commas + 63 text bytes + 60 phoneme bytes + newline = 127.
  start();
  phrases.emplace_back(3, std::string(63, 'a').c_str(), std::string(60, 'b').c_str());
  assert(mnUpdateLocked() && phrases.size() == 5);
  assert(phrases.back().text.size() == 63 && phrases.back().phonemes.size() == 60);
  assertMetadataRestored(); stop();
  start(); const unsigned before = destroyed;
  phrases.emplace_back(3, std::string(63, 'a').c_str(), std::string(61, 'b').c_str());
  assert(!mnUpdateLocked() && destroyed == before); // 128 leaves an unconsumed newline
  assertMetadataRestored(); stop();
}
static void assertFailedClosed() {
  assert(!gSRFeedShouldRun && !gESPSRRunning && !gMnTableTrusted && !lastError.empty());
  assert(snapshotBlocks.empty()); assertMetadataRestored();
}
static void testFailures() {
  start(); auto* original = gMNData; const unsigned oldDestroyed = destroyed;
  snapshotFails = true; assert(!mnUpdateLocked()); snapshotFails = false;
  assertFailedClosed(); assert(gMNData == original && destroyed == oldDestroyed); stop();
  start(); createFails = true; assert(!mnUpdateLocked()); createFails = false;
  assertFailedClosed(); assert(!gMNData && !gMNCommandsAllocated && !gMNGrammarPublished); stop();
  for (bool* fail : {&mismatchedTable, &extraPhrase, &badRate, &badChunk}) {
    start(); *fail = true; assert(!mnUpdateLocked()); *fail = false;
    assertFailedClosed(); assert(gMNData && !gMNGrammarPublished); stop();
  }
  for (const char* invalid : {"bad,phrase", "bad\nphrase", "bad\rphrase", ""}) {
    start(); const unsigned before = destroyed; phrases.emplace_back(3, invalid, nullptr);
    assert(!mnUpdateLocked() && destroyed == before); assertFailedClosed(); stop();
  }
  start(); const unsigned before = destroyed;
  phrases.emplace_back(3, std::string(127, 'a').c_str(), nullptr);
  assert(!mnUpdateLocked() && destroyed == before); assertFailedClosed(); stop();
  start(); phrases.emplace_back(3, "text", "bad,phonemes");
  assert(!mnUpdateLocked()); assertFailedClosed(); stop();
  start(); const unsigned beforeOversizedId = destroyed;
  phrases.emplace_back(1024, "test", nullptr);
  assert(!mnUpdateLocked() && destroyed == beforeOversizedId); assertFailedClosed(); stop();
  start(); int16_t wave = 1; phrases[0].value.wave = &wave;
  assert(!mnUpdateLocked()); assertFailedClosed(); phrases[0].value.wave = nullptr; stop();
  start(); fst.num = 0; const unsigned originalDestroyed = destroyed;
  assert(!mnUpdateLocked() && destroyed == originalDestroyed); fst.num = 1;
  assertFailedClosed(); stop();
  // Bad metadata and empty caller input must be rejected BEFORE vendor create.
  gMNModel = &iface; const unsigned originalAttempts = createAttempts;
  assert(!createMNWithCommandFile("", 0)); gSRModels = nullptr;
  assert(!createMNWithCommandFile("1,test\n", 7)); gSRModels = &models;
  assert(createAttempts == originalAttempts && !gMNData); stop();
}
int main() {
  testVendorDefects(); testRepeatedReplacement(); testCSVLineBoundary(); testFailures();
  assert(created == destroyed && updateCalls == 0 && emptyCreateAttempts == 0);
  puts("ESP-SR create-time grammar ownership, bounded replacements, metadata restoration and rejection tests passed");
}
