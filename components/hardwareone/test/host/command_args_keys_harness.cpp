// Host harness for the key=value reader of CommandArgs.
//
// test_command_args_keys.py inserts the production class (System_Command.h)
// and its constructor, parse(), nextKeyToken(), value() and hasKey() with
// their two file-local helpers (System_Command.cpp). Host boundary: a String
// with the Arduino calls these functions use.
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

class String {
 public:
  String() = default;
  String(const char* text) : text_(text ? text : "") {}
  String(const std::string& text) : text_(text) {}
  unsigned length() const { return (unsigned)text_.size(); }
  const char* c_str() const { return text_.c_str(); }
  char operator[](int index) const { return index >= 0 && (size_t)index < text_.size() ? text_[(size_t)index] : '\0'; }
  String substring(int from, int to) const { return String(text_.substr((size_t)from, (size_t)(to - from))); }
  void trim() {
    const auto first = text_.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) { text_.clear(); return; }
    text_ = text_.substr(first, text_.find_last_not_of(" \t\r\n") - first + 1);
  }
  String& operator+=(char c) { text_ += c; return *this; }

 private:
  std::string text_;
};

// INSERT_PRODUCTION

static int failures = 0;
static int checks = 0;
static void check(bool ok, const std::string& what) {
  ++checks;
  if (ok) return;
  ++failures;
  std::fprintf(stderr, "FAIL: %s\n", what.c_str());
}

// value(key) and hasKey(key) of one line.
static void expect(const char* line, const char* key, const char* value, bool present) {
  const CommandArgs args{String(line)};
  const String got = args.value(key);
  check(std::strcmp(got.c_str(), value) == 0 && args.hasKey(key) == present,
        std::string("[") + line + "] " + key + "= reads [" + got.c_str() + "], expected [" + value + "]" +
            (present ? "" : " and no such key"));
}

// A quoted value as the web UI emits it (WebPage_Automations.h):
// replace(/\\/g,'\\\\') and then replace(/"/g,'\\"').
static std::string webQuoted(const std::string& text) {
  std::string out = "\"";
  for (char c : text) {
    if (c == '\\' || c == '"') out += '\\';
    out += c;
  }
  return out + "\"";
}

// The tokens of one line as "key" for key=value and "-" for anything else.
static std::string keysOf(const char* line) {
  const CommandArgs args{String(line)};
  std::string keys;
  int cursor = 0, start = 0, keyLen = 0;
  while (args.nextKeyToken(cursor, start, keyLen)) {
    if (!keys.empty()) keys += ' ';
    keys += keyLen > 0 ? std::string(args.raw().c_str() + start, (size_t)keyLen) : std::string("-");
  }
  return keys;
}

int main() {
  // A key is read where it stands as a token.
  expect("name=a type=b", "name", "a", true);
  expect("name=a type=b", "type", "b", true);
  expect("  id=12\tname=x", "name", "x", true);
  expect("id=", "id", "", true);
  expect("id=1 id=2", "id", "1", true);
  expect("", "id", "", false);

  // A key inside a longer key is another key.
  expect("dayofmonth=5", "month", "", false);
  expect("dayofmonth=5 month=3", "month", "3", true);
  expect("bootdelayms=3000", "delayms", "", false);
  expect("bootdelayms=3000 delayms=7", "delayms", "7", true);
  expect("condition=x", "on", "", false);
  expect("condition=\"TEMP>5\" on=wifi_connected", "on", "wifi_connected", true);
  expect("commands=a;b command=c", "command", "c", true);
  expect("xid=12", "id", "", false);
  expect("foo=bar=id=3", "id", "", false);

  // A key inside a quoted value belongs to that value.
  expect("commands=\"automation run id=4711 ; print x\"", "id", "", false);
  expect("commands=\"automation run id=4711 ; print x\" id=5", "id", "5", true);
  expect("commands=\"say \\\"id=9\\\" now\" id=5", "id", "5", true);
  expect("\"quoted id=3 text\" id=4", "id", "4", true);
  expect("secondarytriggers=\"[{\\\"type\\\":\\\"event\\\",\\\"on\\\":\\\"x\\\"}]\" on=y", "on", "y", true);

  // A key needs white space before it: text glued to a token is part of it.
  expect("name=x\"id=3", "id", "", false);
  expect("name=\"a b\"id=3", "id", "", false);
  expect("=5 id=7", "id", "7", true);

  // Quoted values keep their spaces and unescape \".
  expect("name=\"Morning lights\" type=attime", "name", "Morning lights", true);
  expect("name=\"Morning lights\" type=attime", "type", "attime", true);
  expect("commands=\"say \\\"hi\\\" now\"", "commands", "say \"hi\" now", true);

  // \\ is an escape pair too: a backslash the web escaped reads back single,
  // and a value that ends in one closes at the quote after it.
  expect("name=\"a\\\\b\"", "name", "a\\b", true);
  expect("name=\"a\\\\\" type=x", "name", "a\\", true);
  expect("name=\"a\\\\\" type=x", "type", "x", true);
  check(keysOf("name=\"a\\\\\" type=x") == "name type", "a value ending in \\\\ does not swallow the next token");
  expect("name=\"\\\\\\\"\" id=3", "name", "\\\"", true);
  expect("name=\"\\\\\\\"\" id=3", "id", "3", true);
  // Any other backslash passes through: a typed regex is unchanged.
  expect("match=\"\\d+\" on=x", "match", "\\d+", true);
  expect("match=\"a\\b\\\\\" on=x", "match", "a\\b\\", true);
  expect("match=\"a\\b\\\\\" on=x", "on", "x", true);
  // Unquoted values are taken as they stand.
  expect("path=C:\\\\x id=2", "path", "C:\\\\x", true);

  // Text the web quotes reads back exactly, nested JSON included.
  {
    const std::string texts[] = {
        "Morning lights", "a\\b", "a\\", "\\", "say \"hi\" now", "ends in quote\"",
        "[{\"type\":\"event\",\"on\":\"wifi_connected\",\"match\":\"say \\\"hi\\\" C:\\\\tmp\\\\\"}]",
        "[{\"type\":\"attime\",\"time\":\"07:00\"},{\"type\":\"event\",\"on\":\"x\"}]",
        "led color=red; say \"done\\\\\"",
    };
    for (const std::string& text : texts) {
      const std::string line = "name=x secondarytriggers=" + webQuoted(text) + " id=9 on=y";
      expect(line.c_str(), "secondarytriggers", text.c_str(), true);
      expect(line.c_str(), "id", "9", true);
      check(keysOf(line.c_str()) == "name secondarytriggers id on", "[" + line + "] keeps its four tokens");
    }
  }

  // The token walk tells a key=value token from a bare word.
  check(keysOf("name=Morning lights type=x") == "name - type", "an unquoted value with a space leaves a bare word");
  check(keysOf("name=\"Morning lights\" type=x") == "name type", "a quoted value is one token");
  check(keysOf("\"q w\" k=\"a b\"z end") == "- k -", "a quoted text and a trailing word are bare tokens");
  check(keysOf("commands=\"a id=1; b\" id=2") == "commands id", "keys inside a quoted value are not tokens");
  check(keysOf("=5 a=1") == "- a", "a token that starts with = has no key");
  check(keysOf("   ").empty() && keysOf("").empty(), "an empty line has no tokens");

  if (failures != 0) {
    std::fprintf(stderr, "%d of %d command key checks failed\n", failures, checks);
    return 1;
  }
  std::printf("command args keys: %d checks passed\n", checks);
  return 0;
}
