// Runs the production command redactor (both redactCmdForAudit overloads,
// maskMacTokens, the rule table and its helpers from System_Utils.cpp) and
// CommandArgs (System_Command.cpp) on the real Arduino String with its heap
// calls counted. resolveCommand is the production name matcher
// (System_CommandLookupCore.h) over a name list the test passes in; the
// redactor reads only whether it found an entry.
//
//   report <names file> <corpus file> <report file>
//     For each corpus line (hex), one report line (hex fields, space
//     separated): the input, redactCmdForAudit(line), the two-argument form
//     with no entry and with an entry, and redactCmdForAudit of the first
//     result (the redactor is not idempotent, and that must not change).
//     With REDACT_RULES_IN_PLACE the rule match of every table row is also
//     compared, for every line, with the match of the tree before (a copy
//     below under another name): any difference fails.
//   counts <names file>
//     String blocks made by one call, for the lines the README names.
#include <Arduino.h>
#include "string_heap_count.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "System_CommandLookupCore.h"

struct CommandResolution {
  const void* entry = nullptr;
};
static std::vector<std::string> gNames;
CommandResolution resolveCommand(const String& line) {
  const hw1_command_lookup::Match match = hw1_command_lookup::resolve(
      line.c_str(), line.length(), gNames.size(),
      [](size_t index) -> const char* { return gNames[index].c_str(); });
  CommandResolution resolution;
  if (match.found()) resolution.entry = &gNames[match.registryOrdinal];
  return resolution;
}
String redactCmdForAudit(const String&);
String redactCmdForAudit(const String&, const CommandResolution&);

// INSERT_COMMAND_ARGS

// INSERT_REDACTION

static void fail(const std::string& what) {
  std::fprintf(stderr, "redact audit: %s\n", what.c_str());
  std::exit(1);
}

#if REDACT_RULES_IN_PLACE
// redactRuleMatches as it was before the rule words were read in place: the
// line and the rule's prefix each parsed by CommandArgs, the words compared
// with equalsIgnoreCase. Kept here only as the reference.
static bool referenceRuleMatches(const String& command, const char* rulePrefix) {
  CommandArgs actual(command);
  String expectedLine(rulePrefix ? rulePrefix : "");
  expectedLine.trim();
  CommandArgs expected(expectedLine);
  if (expected.count() == 0 || actual.count() < expected.count()) return false;
  for (int i = 0; i < expected.count(); ++i) {
    if (!actual.arg(i).equalsIgnoreCase(expected.arg(i))) return false;
  }
  return true;
}
#endif

static std::string hex(const String& value) {
  static const char kDigits[] = "0123456789abcdef";
  std::string out;
  const char* text = value.c_str();
  for (unsigned i = 0; text && i < value.length(); ++i) {
    const unsigned char byte = static_cast<unsigned char>(text[i]);
    out += kDigits[byte >> 4];
    out += kDigits[byte & 15];
  }
  return out.empty() ? std::string("-") : out;
}

static String unhex(const std::string& text) {
  std::string bytes;
  if (text != "-") {
    for (size_t i = 0; i + 1 < text.size(); i += 2) {
      bytes += static_cast<char>(std::strtoul(text.substr(i, 2).c_str(), nullptr, 16));
    }
  }
  return String(bytes.data(), static_cast<unsigned>(bytes.size()));
}

static void readNames(const char* path) {
  std::ifstream in(path);
  if (!in) fail(std::string("cannot read ") + path);
  std::string name;
  while (std::getline(in, name)) {
    if (!name.empty()) gNames.push_back(name);
  }
}

static int report(const char* corpusPath, const char* reportPath) {
  std::ifstream in(corpusPath);
  std::ofstream out(reportPath, std::ios::binary);
  if (!in || !out) fail("cannot open the corpus or the report");
  CommandResolution none, known;
  known.entry = &gNames;
  std::string encoded;
  unsigned long lines = 0, matches = 0;
  while (std::getline(in, encoded)) {
    const String line = unhex(encoded);
    const String plain = redactCmdForAudit(line);
    const String unresolved = redactCmdForAudit(line, none);
    const String resolved = redactCmdForAudit(line, known);
    const String again = redactCmdForAudit(plain);
    out << hex(line) << ' ' << hex(plain) << ' ' << hex(unresolved) << ' ' << hex(resolved) << ' '
        << hex(again) << '\n';
#if REDACT_RULES_IN_PLACE
    for (const String* text : {&line, &plain}) {
      const CommandArgs parsed(*text);
      for (const RedactRule& rule : kRules) {
        const bool now = redactRuleMatches(parsed, rule.prefix);
        if (now != referenceRuleMatches(*text, rule.prefix)) {
          fail("rule \"" + std::string(rule.prefix) + "\" matches differently for line " + hex(*text));
        }
        matches += now ? 1 : 0;
      }
    }
#endif
    ++lines;
  }
  std::printf("redact audit: %lu lines, %lu rule matches compared with the reference\n", lines, matches);
  return 0;
}

// The lines of the finder's model (a line under 14 characters, a longer one of
// short words, one with a long word) and one of each rule kind.
static int counts() {
  static const char* const kLines[] = {
      "status",
      "sensorlog start 5000",
      "openfile /logging_captures/sensors/sensors_2026-11-15.csv",
      "nosuchverb with some arguments",
      "login alice example-password",
      "wifiadd example-network example-secret",
      "espnowremote peer alice example-password status",
      "phone pair new",
  };
  for (const char* text : kLines) {
    const String line(text);
    gStringHeap = StringHeapCount{};
    const String result = redactCmdForAudit(line);
    std::printf("%lu %s\n", gStringHeap.made, text);
  }
  return 0;
}

int main(int argc, char** argv) {
  if (argc >= 3 && std::strcmp(argv[1], "counts") == 0) {
    readNames(argv[2]);
    return counts();
  }
  if (argc >= 5 && std::strcmp(argv[1], "report") == 0) {
    readNames(argv[2]);
    return report(argv[3], argv[4]);
  }
  fail("usage: report <names> <corpus> <report> | counts <names>");
  return 1;
}
