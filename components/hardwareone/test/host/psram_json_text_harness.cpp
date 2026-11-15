// Host harness for PsramJsonText (System_PsramJson.h), spliced in at the
// marker by test_psram_json_text.py. The Arduino core String is the real one
// with its heap calls counted (arduino_string_host.py), so the old path,
// serializeJson(doc, String&), runs as on the device; ArduinoJson is the
// vendored library. ps_alloc and ps_free record every block.
#define ARDUINOJSON_ENABLE_ARDUINO_STRING 1
#define ARDUINOJSON_ENABLE_ARDUINO_STREAM 0
#define ARDUINOJSON_ENABLE_ARDUINO_PRINT 0
#define ARDUINOJSON_ENABLE_PROGMEM 0
#include <Arduino.h>
#include <ArduinoJson.h>
#include "string_heap_count.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <string>

static int gFailures = 0;
static void check(bool ok, const std::string& what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    ++gFailures;
  }
}

enum class AllocPref : uint8_t { PreferPsram = 0, PreferInternal = 1 };
struct Block { size_t size; std::string tag; AllocPref pref; };
static std::map<void*, Block> gLive;
static unsigned gMade = 0;
static bool gRefuse = false;
static size_t gLastSize = 0;
static std::string gLastTag;
static AllocPref gLastPref = AllocPref::PreferInternal;
void* ps_alloc(size_t size, AllocPref pref, const char* tag) {
  ++gMade;
  gLastSize = size; gLastTag = tag ? tag : ""; gLastPref = pref;
  if (gRefuse) return nullptr;
  void* block = std::malloc(size);
  gLive[block] = {size, gLastTag, pref};
  return block;
}
void ps_free(void* block) {
  if (!block) return;
  check(gLive.erase(block) == 1, "ps_free releases a block ps_alloc made");
  std::free(block);
}

// INSERT_PRODUCTION

struct Case {
  const char* name;
  std::function<void(JsonDocument&)> fill;
};

static std::string repeat(char c, size_t n) { return std::string(n, c); }

int main() {
  const std::string escapes = std::string("quote\" back\\ slash/ nl\n cr\r tab\t bell\x07 nul") +
                              std::string(1, '\0') + "end \xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80";
  const Case cases[] = {
    {"null", [](JsonDocument&) {}},
    {"empty object", [](JsonDocument& d) { d.to<JsonObject>(); }},
    {"empty array", [](JsonDocument& d) { d.to<JsonArray>(); }},
    {"short object", [](JsonDocument& d) { d["ok"] = true; }},
    {"numbers", [](JsonDocument& d) {
       d["neg"] = -42; d["u32"] = 4294967295u; d["u64"] = 18446744073709551615ull;
       d["i64"] = -9223372036854775807ll - 1; d["f"] = 3.25f; d["d"] = 0.1; d["tiny"] = 1e-300;
       d["big"] = 1.5e300; d["zero"] = 0; d["false"] = false; d["null"] = nullptr; }},
    {"escapes", [&](JsonDocument& d) { d["text"] = escapes; d["k\"ey"] = "v"; }},
    {"30 characters", [](JsonDocument& d) { d["t"] = repeat('a', 22); }},
    {"31 characters", [](JsonDocument& d) { d["t"] = repeat('a', 23); }},
    {"32 characters", [](JsonDocument& d) { d["t"] = repeat('a', 24); }},
    {"62 characters", [](JsonDocument& d) { d["t"] = repeat('b', 54); }},
    {"nested", [](JsonDocument& d) {
       JsonArray sessions = d["sessions"].to<JsonArray>();
       for (int i = 0; i < 2; ++i) {
         JsonObject s = sessions.add<JsonObject>();
         s["user"] = "admin"; s["ip"] = "local"; s["current"] = i == 0; s["ageSec"] = 120 * i;
       }
       d["success"] = true; }},
    {"poll reply (about 1.5 KB)", [](JsonDocument& d) {
       d["done"] = false; d["len"] = 4096; d["text"] = repeat('x', 511) + "\"\\" + repeat('y', 900);
       d["next"] = 1413; }},
    {"status reply (about 6 KB)", [](JsonDocument& d) {
       for (int i = 0; i < 60; ++i) {
         JsonObject o = d["k" + std::to_string(i)].to<JsonObject>();
         o["name"] = "entry " + std::to_string(i); o["value"] = i * 1.5; o["on"] = (i % 2) == 0;
         o["detail"] = repeat('d', 40);
       } }},
    {"large (about 20 KB, above 16 KB)", [](JsonDocument& d) {
       JsonArray list = d["list"].to<JsonArray>();
       for (int i = 0; i < 400; ++i) list.add(repeat('w', 45));
     }},
  };

  unsigned long oldRegrowths = 0, oldBlocks = 0;
  for (const Case& c : cases) {
    JsonDocument doc;
    c.fill(doc);
    check(!doc.overflowed(), std::string(c.name) + ": fixture fits");

    // The old path, as the eight call sites wrote it.
    const StringHeapCount before = gStringHeap;
    String old;
    const size_t oldWritten = serializeJson(doc, old);
    const unsigned long made = gStringHeap.made - before.made;
    const unsigned long regrown = gStringHeap.regrown - before.regrown;
    oldBlocks += made; oldRegrowths += regrown;
    const std::string expected(old.c_str(), old.length());
    check(oldWritten == expected.size(), std::string(c.name) + ": old path wrote its text");

    // The new path: one block of measureJson + 1, no String at all.
    const StringHeapCount quiet = gStringHeap;
    const unsigned madeBefore = gMade;
    {
      const PsramJsonText text(doc, "test.reply");
      check(text.ok(), std::string(c.name) + ": text made");
      check(std::string(text.c_str(), text.length()) == expected,
            std::string(c.name) + ": bytes equal serializeJson(doc, String&)");
      check(std::strlen(text.c_str()) == text.length(),
            std::string(c.name) + ": NUL-terminated at its length (strlen senders see the same text)");
      check(gMade - madeBefore == 1 && gLastSize == expected.size() + 1 && gLastTag == "test.reply" &&
            gLastPref == AllocPref::PreferPsram,
            std::string(c.name) + ": one PreferPsram block of length + 1 with the caller's tag");
      check(gStringHeap.made == quiet.made && gStringHeap.regrown == quiet.regrown,
            std::string(c.name) + ": no String block made or regrown");
      std::fprintf(stderr, "%-34s %6zu bytes: String path %lu block(s) + %lu regrowth(s); "
                           "PsramJsonText 1 block\n", c.name, expected.size(), made, regrown);
    }
    check(gLive.empty(), std::string(c.name) + ": the block is released with the text");
  }
  check(oldRegrowths > 600, "the fixtures exercise the String regrowth the change removes");

  // A refused block: no partial text, the empty text, nothing kept.
  {
    JsonDocument doc;
    doc["text"] = repeat('r', 200);
    gRefuse = true;
    const PsramJsonText text(doc, "test.refused");
    gRefuse = false;
    check(!text.ok() && text.length() == 0 && std::strcmp(text.c_str(), "") == 0 && gLive.empty(),
          "refused block: ok() false, empty text, nothing allocated");
  }
  // wipe() clears every byte, the terminator included, before the release.
  {
    JsonDocument doc;
    doc["sttText"] = "private words";
    PsramJsonText text(doc, "test.private");
    const char* bytes = text.c_str();
    const size_t length = text.length();
    text.wipe();
    bool zero = true;
    for (size_t i = 0; i <= length; ++i) zero = zero && bytes[i] == 0;
    check(length > 0 && zero, "wipe() zeroes the text and its terminator");
  }
  check(gLive.empty(), "every block released");

  if (gFailures) {
    std::fprintf(stderr, "psram_json_text: %d failure(s)\n", gFailures);
    return 1;
  }
  std::printf("psram_json_text: %zu documents byte-equal to serializeJson(doc, String&); "
              "old path %lu blocks + %lu regrowths, new path one PSRAM block each\n",
              sizeof(cases) / sizeof(cases[0]), oldBlocks, oldRegrowths);
  return 0;
}
