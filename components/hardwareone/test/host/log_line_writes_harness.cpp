// Host harness for the log writer's line output (writeLogLine,
// System_Debug.cpp), spliced in at the marker by test_log_line_writes.py.
// Print is the real Arduino core Print.cpp, compiled on the host with its
// malloc and free counted, so the old call, Print::printf, runs as on the
// device: it formats into 64 bytes on its stack and mallocs a block for a
// longer line. A recording Print stands in for the serial port and the file.
#include <Arduino.h>
#include <Print.h>
#include "print_heap_count.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int gFailures = 0;
static void check(bool ok, const std::string& what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    ++gFailures;
  }
}

// INSERT_DEBUG_MSG_SIZE
// INSERT_PRODUCTION

struct RecordingPrint : Print {
  std::string bytes;
  unsigned writes = 0;
  size_t write(uint8_t c) override { bytes.push_back(static_cast<char>(c)); ++writes; return 1; }
  size_t write(const uint8_t* buffer, size_t size) override {
    bytes.append(reinterpret_cast<const char*>(buffer), size); ++writes; return size;
  }
};

int main() {
  std::vector<std::string> texts = {"", "x", "boot", "[WIFI] connected", std::string(40, 'a'),
                                    std::string(49, 'b'), std::string(50, 'c'), std::string(51, 'd'),
                                    "100% done, %s %d %n are text here", "caf\xc3\xa9 \xe2\x82\xac",
                                    std::string(200, 'e'), std::string(DEBUG_MSG_SIZE - 1, 'f')};
  const unsigned long stamps[] = {0ul, 7ul, 123456ul, 4294967295ul};
  const std::string tag47(47, 't'), tag48(48, 'u'), tag80(80, 'v');
  const char* tags[] = {nullptr, "WIFI", "SENSORS_THERMAL_FRAME", tag47.c_str()};
  unsigned long oldBlocks = 0, cases = 0;
  auto compare = [&](unsigned long stamp, const char* tag, const std::string& text, bool fits) {
    ++cases;
    RecordingPrint before, after;
    const PrintHeapCount start = gPrintHeap;
    if (tag) before.printf("[%lu] [%s] %s\n", stamp, tag, text.c_str());
    else before.printf("[%lu] %s\n", stamp, text.c_str());
    const unsigned long oldMade = gPrintHeap.made - start.made;
    oldBlocks += oldMade;
    const PrintHeapCount middle = gPrintHeap;
    writeLogLine(after, stamp, tag, text.c_str());
    const unsigned long newMade = gPrintHeap.made - middle.made;
    const std::string what = std::string(tag ? tag : "(no tag)").substr(0, 12) + "/" + std::to_string(stamp) +
                             "/" + std::to_string(text.size());
    check(after.bytes == before.bytes, what + ": the same bytes as Print::printf");
    check(after.writes == 1 && before.writes == 1, what + ": one write, as printf makes");
    check(oldMade == (before.bytes.size() >= 64 ? 1u : 0u), what + ": printf's block for a line of 64+ bytes");
    check(newMade == (fits ? 0u : oldMade), what + (fits ? ": no heap block" : ": an overlong line goes through printf"));
  };
  for (unsigned long stamp : stamps)
    for (const char* tag : tags)
      for (const std::string& text : texts) compare(stamp, tag, text, true);
  // A tag over 47 characters with a full message does not fit: printf as before.
  compare(4294967295ul, tag48.c_str(), std::string(DEBUG_MSG_SIZE - 1, 'g'), false);
  compare(1ul, tag80.c_str(), std::string(DEBUG_MSG_SIZE - 1, 'h'), false);
  // A long tag with a short message still fits.
  compare(1ul, tag80.c_str(), "short", true);
  check(oldBlocks >= 90, "the cases exercise printf's heap path (93 of the 195 lines are 64 bytes or more)");
  if (gFailures) {
    std::fprintf(stderr, "log_line_writes: %d failure(s)\n", gFailures);
    return 1;
  }
  std::printf("log_line_writes: %lu lines byte-equal to Print::printf, one write each; "
              "printf made %lu heap blocks, writeLogLine none for a line that fits\n", cases, oldBlocks);
  return 0;
}
