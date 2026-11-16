// Runs the production command audit redactor (both redactCmdForAudit
// overloads, maskMacTokens, the rule table and its helpers from
// System_Utils.cpp, CommandArgs from System_Command.cpp) on the real Arduino
// String with its heap calls counted and the live String bytes tracked.
// resolveCommand is the production name matcher (System_CommandLookupCore.h)
// over a name list the test passes in. The tree under test is compiled into
// namespace hw1_new; with REDACT_HAVE_OLD an older tree's redactor is compiled
// beside it into namespace hw1_old and every output is compared in place.
//
//   report <names> <corpus> <report>
//     For each corpus line (hex): the input, redactCmdForAudit(line), the
//     two-argument form with no entry and with an entry, and
//     redactCmdForAudit of the first result (hex fields). With the old tree
//     built in, any output that differs from the old one fails the run.
//   stack <names> <corpus>
//     For each line: the stack bytes one redactCmdForAudit(line) used on a
//     painted 8 MB thread stack, and the peak of live String bytes during the
//     call; the same for the old tree, or -1 without it.
//   small <names> <corpus> <stack bytes>
//     Redacts each line on a thread whose stack is that size (system stack
//     with its guard page) and checks the text equals the main thread's.
#include <Arduino.h>
#include "string_heap_count.h"

#include <pthread.h>
#include <sys/mman.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "System_CommandLookupCore.h"

extern size_t gStringLive, gStringPeak;

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

// INSERT_COMMAND_ARGS

namespace hw1_new {
String redactCmdForAudit(const String&);
String redactCmdForAudit(const String&, const CommandResolution&);
// INSERT_NEW
}  // namespace hw1_new

#if REDACT_HAVE_OLD
namespace hw1_old {
String redactCmdForAudit(const String&);
String redactCmdForAudit(const String&, const CommandResolution&);
// INSERT_OLD
}  // namespace hw1_old
#endif

static void fail(const std::string& what) {
  std::fprintf(stderr, "redact audit nesting: %s\n", what.c_str());
  std::exit(1);
}

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

static std::vector<String> readCorpus(const char* path) {
  std::ifstream in(path);
  if (!in) fail(std::string("cannot read ") + path);
  std::vector<String> lines;
  std::string encoded;
  while (std::getline(in, encoded)) lines.push_back(unhex(encoded));
  return lines;
}

static bool same(const String& a, const String& b) {
  return a.length() == b.length() && std::memcmp(a.c_str(), b.c_str(), a.length()) == 0;
}

static int report(const char* corpusPath, const char* reportPath) {
  const std::vector<String> lines = readCorpus(corpusPath);
  std::ofstream out(reportPath, std::ios::binary);
  if (!out) fail("cannot open the report");
  CommandResolution none, known;
  known.entry = &gNames;
  unsigned long compared = 0, differ = 0;
  for (const String& line : lines) {
    const String plain = hw1_new::redactCmdForAudit(line);
    const String unresolved = hw1_new::redactCmdForAudit(line, none);
    const String resolved = hw1_new::redactCmdForAudit(line, known);
    const String again = hw1_new::redactCmdForAudit(plain);
    out << hex(line) << ' ' << hex(plain) << ' ' << hex(unresolved) << ' ' << hex(resolved) << ' '
        << hex(again) << '\n';
#if REDACT_HAVE_OLD
    const String oldPlain = hw1_old::redactCmdForAudit(line);
    const String outputs[][2] = {
        {plain, oldPlain},
        {unresolved, hw1_old::redactCmdForAudit(line, none)},
        {resolved, hw1_old::redactCmdForAudit(line, known)},
        {again, hw1_old::redactCmdForAudit(oldPlain)},
    };
    for (const auto& pair : outputs) {
      ++compared;
      if (same(pair[0], pair[1])) continue;
      if (++differ <= 5) {
        std::fprintf(stderr, "differs: input %s\n  now:    %s\n  before: %s\n", hex(line).c_str(),
                     hex(pair[0]).c_str(), hex(pair[1]).c_str());
      }
    }
#endif
  }
  if (differ) fail(std::to_string(differ) + " of " + std::to_string(compared) +
                   " outputs differ from the old redactor");
  if (REDACT_HAVE_OLD) {
    std::printf("redact audit nesting: %zu lines, %lu outputs equal to the old redactor's\n",
                lines.size(), compared);
  } else {
    std::printf("redact audit nesting: %zu lines redacted (no old redactor built in)\n", lines.size());
  }
  std::fflush(stdout);
  return 0;
}

// ---------------------------------------------------------------------------
// Stack and heap per call
// ---------------------------------------------------------------------------
struct Job {
  const String* line;
  bool old;
  String result;
  size_t heapPeak;
};

static void* runJob(void* arg) {
  Job* job = static_cast<Job*>(arg);
  const size_t before = gStringLive;
  gStringPeak = before;
#if REDACT_HAVE_OLD
  job->result = job->old ? hw1_old::redactCmdForAudit(*job->line) : hw1_new::redactCmdForAudit(*job->line);
#else
  job->result = hw1_new::redactCmdForAudit(*job->line);
#endif
  job->heapPeak = gStringPeak - before;
  return nullptr;
}

static constexpr size_t kPaintedStack = 8u << 20;
static constexpr unsigned char kPaint = 0xA5;

// The scan reads stack memory the thread used; the sanitizer's redzones on it
// are not this code's business.
__attribute__((no_sanitize("address")))
static size_t paintedBytesUsed(const unsigned char* base, size_t size) {
  size_t untouched = 0;
  while (untouched < size && base[untouched] == kPaint) ++untouched;
  return size - untouched;
}

static size_t stackUsed(Job& job) {
  void* memory = mmap(nullptr, kPaintedStack, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (memory == MAP_FAILED) fail("mmap");
  std::memset(memory, kPaint, kPaintedStack);
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  if (pthread_attr_setstack(&attr, memory, kPaintedStack) != 0) fail("pthread_attr_setstack");
  pthread_t thread;
  if (pthread_create(&thread, &attr, runJob, &job) != 0) fail("pthread_create");
  pthread_join(thread, nullptr);
  pthread_attr_destroy(&attr);
  const size_t used = paintedBytesUsed(static_cast<const unsigned char*>(memory), kPaintedStack);
  munmap(memory, kPaintedStack);
  return used;
}

static int stack(const char* corpusPath) {
  const std::vector<String> lines = readCorpus(corpusPath);
  for (const String& line : lines) {
    Job fresh{&line, false, String(), 0};
    const size_t newStack = stackUsed(fresh);
    long oldStack = -1, oldHeap = -1;
#if REDACT_HAVE_OLD
    Job previous{&line, true, String(), 0};
    oldStack = static_cast<long>(stackUsed(previous));
    oldHeap = static_cast<long>(previous.heapPeak);
    if (!same(fresh.result, previous.result)) fail("stack run differs from the old redactor: " + hex(line));
#endif
    std::printf("%zu %zu %ld %ld %u\n", newStack, fresh.heapPeak, oldStack, oldHeap, line.length());
  }
  return 0;
}

static int small(const char* corpusPath, size_t stackBytes) {
  const std::vector<String> lines = readCorpus(corpusPath);
  for (const String& line : lines) {
    const String expected = hw1_new::redactCmdForAudit(line);
    Job job{&line, false, String(), 0};
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (pthread_attr_setstacksize(&attr, stackBytes) != 0) fail("pthread_attr_setstacksize");
    pthread_t thread;
    if (pthread_create(&thread, &attr, runJob, &job) != 0) fail("pthread_create");
    pthread_join(thread, nullptr);
    pthread_attr_destroy(&attr);
    if (!same(job.result, expected)) fail("small-stack result differs: " + hex(line));
  }
  std::printf("redact audit nesting: %zu lines redacted on a %zu-byte thread stack\n", lines.size(),
              stackBytes);
  std::fflush(stdout);
  return 0;
}

int main(int argc, char** argv) {
  if (argc >= 5 && std::strcmp(argv[1], "report") == 0) {
    readNames(argv[2]);
    return report(argv[3], argv[4]);
  }
  if (argc >= 4 && std::strcmp(argv[1], "stack") == 0) {
    readNames(argv[2]);
    return stack(argv[3]);
  }
  if (argc >= 5 && std::strcmp(argv[1], "small") == 0) {
    readNames(argv[2]);
    return small(argv[3], std::strtoul(argv[4], nullptr, 10));
  }
  fail("usage: report <names> <corpus> <report> | stack <names> <corpus> | small <names> <corpus> <bytes>");
  return 1;
}
