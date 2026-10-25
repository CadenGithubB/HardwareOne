// test_json_pretty.py compiles this beside the shared re-indenter
// (System_JsonPretty.h) and production code read from --component: the OLED
// browser's viewer state, its fbFillView body builder, its action rows
// (fbBuildActionRows) and its JSON decision (the `sFbActionJson = ...;`
// assignment). ArduinoJson is only the layout reference for valid input.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <ArduinoJson.h>

#include "FileBrowser_FileKind.h"
#include "System_JsonPretty.h"
#include "System_TextPager.h"
#include "System_TextWrapStream.h"

static unsigned checks = 0, failures = 0;
#define CHECK(condition) do { ++checks; if (!(condition)) { ++failures; \
  std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); } } while (false)

#define EXT_RAM_BSS_ATTR

// --- OLED browser: viewer state, body builder, action rows ------------------
// INSERT_OLED_VIEW_STATE
static bool sFbActionViewable = false;
static bool sFbActionJson = false;
// INSERT_OLED_ACTION_ROWS_MAX

// INSERT_OLED_FILL_VIEW

// INSERT_OLED_ACTION_ROWS

static bool oledJson(const char* name) {
  struct { char name[64]; } entry{};
  std::snprintf(entry.name, sizeof(entry.name), "%s", name);
  return INSERT_OLED_JSON;
}

// --- Helpers -----------------------------------------------------------------
static std::string pretty(const std::string& input) {
  std::string out;
  JsonPrettyFormatter().write(input.data(), input.size(), [&](char c) { out += c; });
  return out;
}

static std::string arduinoJsonPretty(const std::string& input) {
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, input);
  CHECK(!err);
  std::string out;
  serializeJsonPretty(doc, out);
  // ArduinoJson ends lines with "\r\n"; the re-indenter writes "\n" (the
  // display sinks drop '\r' either way).
  std::string lf;
  for (char c : out) if (c != '\r') lf += c;
  return lf;
}

static std::string wrapped(const std::string& input, size_t cap, size_t cols) {
  std::vector<char> out(cap, 0);
  bool ignored = false;
  textWrapInto(out.data(), cap, input.c_str(), cols, 0, true, &ignored);
  return out.data();
}

// --- Scenarios ---------------------------------------------------------------
static const char* const kValid[] = {
    "{}",
    "[]",
    "{\"a\":{}}",
    "{\"a\":[]}",
    "[1,2,3]",
    "{\"a\":1,\"b\":[true,false,null],\"c\":{\"d\":\"e\"}}",
    "[{\"x\":1},{\"y\":[{\"z\":[]}]},[[]]]",
    "{\"n\":-12,\"z\":0,\"f\":3.5}",
    "{\"q\":\"say \\\"hi\\\" \\\\ done\",\"b\":\"{[,:]} \\\\\",\"u\":\"caf\xc3\xa9\"}",
    ("{\"users\":[{\"name\":\"admin\",\"role\":\"admin\",\"tags\":[\"a\",\"b\"]},"
     "{\"name\":\"guest\",\"role\":\"guest\",\"tags\":[]}],\"version\":3}"),
};

static void testLayout() {
  for (const char* fixture : kValid) {
    const std::string expected = arduinoJsonPretty(fixture);
    CHECK(pretty(fixture) == expected);
    // Re-indenting already-pretty or CRLF/tab-spaced input gives the same text.
    CHECK(pretty(expected) == expected);
    std::string spaced;
    for (const char* p = fixture; *p; ++p) {
      spaced += *p;
      if (*p == ',' || *p == ':' || *p == '{' || *p == '[') spaced += "\r\n\t ";
    }
    // Only outside strings: the escape fixture's strings hold these bytes.
    if (!std::strchr(fixture, '\\')) CHECK(pretty(spaced) == expected);
  }
}

static void testChunks() {
  // State carries across write() calls, wherever a buffer boundary falls
  // (mid-escape, mid-string, between an opener and its closer).
  for (const char* fixture : kValid) {
    const std::string input = fixture, whole = pretty(input);
    for (size_t cut = 0; cut <= input.size(); ++cut) {
      std::string out;
      JsonPrettyFormatter formatter;
      auto emit = [&](char c) { out += c; };
      formatter.write(input.data(), cut, emit);
      formatter.write(input.data() + cut, input.size() - cut, emit);
      CHECK(out == whole);
    }
  }
}

static void testMalformed() {
  CHECK(pretty("{\"partial\": [1,2,") == "{\n  \"partial\": [\n    1,\n    2,\n    ");
  CHECK(pretty("{\"a\":\"open") == "{\n  \"a\": \"open");
  CHECK(pretty("{") == "{");
  CHECK(pretty("]]}") == "\n]\n]\n}");            // stray closers never go below zero
  CHECK(pretty("]{\"a\":1}") == "\n]{\n  \"a\": 1\n}");
  CHECK(pretty(std::string("[\"a\0b\"]", 7)) == std::string("[\n  \"a\0b\"\n]", 11));
  CHECK(pretty("") == "");
  JsonPrettyFormatter().write(nullptr, 5, [](char) { CHECK(false); });
}

static void testIndentCap() {
  const size_t depth = 40;
  const std::string input = std::string(depth, '[') + "1" + std::string(depth, ']');
  const std::string out = pretty(input);
  size_t maxLead = 0, lead = 0;
  bool atStart = true;
  for (char c : out) {
    if (c == '\n') { lead = 0; atStart = true; continue; }
    if (atStart && c == ' ') { if (++lead > maxLead) maxLead = lead; continue; }
    atStart = false;
  }
  CHECK(maxLead == 2 * JsonPrettyFormatter::kMaxIndentLevels);
  // Bounded growth: every byte of input adds at most one indented line.
  CHECK(out.size() <= input.size() * (2 + 2 * JsonPrettyFormatter::kMaxIndentLevels));
}

static void testOledView() {
  const std::string json = kValid[9];
  const size_t cap = sizeof(sFbViewBody);

  sFbViewScroll = 5;
  fbFillView(json.data(), json.size(), /*pretty=*/true);
  const std::string body = sFbViewBody;
  CHECK(body == wrapped(arduinoJsonPretty(json), cap, FB_VIEW_COLS));
  CHECK(!sFbViewTruncated && sFbViewScroll == 0);
  int lines = 1;
  for (char c : body) lines += (c == '\n');
  CHECK(sFbViewLineCount == lines);
  for (int i = 1; i < sFbViewLineCount; ++i) CHECK(sFbViewBody[sFbViewLineOff[i] - 1] == '\n');

  fbFillView(json.data(), json.size(), /*pretty=*/false);   // JSON View: as stored
  CHECK(std::string(sFbViewBody) == wrapped(json, cap, FB_VIEW_COLS));
  CHECK(sFbViewLineCount == (int)((json.size() + FB_VIEW_COLS - 1) / FB_VIEW_COLS));

  const std::string raw = "first\r\n\tsecond\001\n" + std::string(50, 'x');
  fbFillView(raw.data(), raw.size(), false);
  CHECK(std::string(sFbViewBody) == wrapped(raw, cap, FB_VIEW_COLS) && !sFbViewTruncated);

  const int maxLines = (int)(sizeof(sFbViewLineOff) / sizeof(sFbViewLineOff[0]));
  std::string full;
  for (int i = 0; i < maxLines; ++i) full += "a\n";
  fbFillView(full.data(), full.size(), false);
  CHECK(sFbViewLineCount == maxLines && !sFbViewTruncated);
  full += "b";
  fbFillView(full.data(), full.size(), false);
  CHECK(sFbViewLineCount == maxLines && sFbViewTruncated);   // line table ran out

  const std::string big(cap + 100, 'x');
  fbFillView(big.data(), big.size(), false);
  CHECK(sFbViewTruncated && std::strlen(sFbViewBody) == cap - 1);   // body ran out
}

static std::vector<std::string> rowsFor(const char* name) {
  sFbActionViewable = fileKindIsReadableText(fileKindForName(name));
  sFbActionJson = oledJson(name);
  const char* labels[FB_ACTION_ROWS_MAX] = {};
  const int n = fbBuildActionRows(labels);
  CHECK(n > 0 && n <= FB_ACTION_ROWS_MAX);
  return std::vector<std::string>(labels, labels + n);
}

static void testOledRows() {
  using Rows = std::vector<std::string>;
  CHECK(rowsFor("settings.json") ==
        (Rows{"Pretty View", "JSON View", "Rename", "Delete", "< Back"}));
  CHECK(rowsFor("USERS.JSON") ==
        (Rows{"Pretty View", "JSON View", "Rename", "Delete", "< Back"}));
  CHECK(rowsFor("notes.txt") == (Rows{"View", "Rename", "Delete", "< Back"}));
  CHECK(rowsFor("photo.jpg") == (Rows{"Rename", "Delete", "< Back"}));
  CHECK(rowsFor("json") == (Rows{"Rename", "Delete", "< Back"}));
}

int main() {
  testLayout();
  testChunks();
  testMalformed();
  testIndentCap();
  testOledView();
  testOledRows();
  if (failures) {
    std::fprintf(stderr, "json pretty: %u of %u checks FAILED\n", failures, checks);
    return 1;
  }
  std::printf("json pretty: %u checks passed\n", checks);
  return 0;
}
