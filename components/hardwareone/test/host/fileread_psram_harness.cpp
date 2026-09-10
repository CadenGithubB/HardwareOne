// The production owner, byte writers and complete handler are inserted below.
// This fixture does not model BLE cryptography, real task races, FS drivers or
// Arduino's String allocator. Those remain device/build integration checks.
#include <ArduinoJson.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

static unsigned assertions = 0;
static void check(bool condition, const char* reason) {
  ++assertions;
  if (!condition) throw std::runtime_error(reason);
}

class String {
 public:
  String() = default;
  String(const char* text) : text_(text ? text : "") {}
  String(const std::string& text) : text_(text) {}
  const char* c_str() const { return text_.c_str(); }
  size_t length() const { return text_.size(); }
  long toInt() const { return std::strtol(text_.c_str(), nullptr, 10); }
  bool operator==(const char* text) const { return text_ == text; }
 private:
  std::string text_;
};

enum class AllocPref { PreferPSRAM };
struct Allocation { size_t size; std::string tag; bool external; };
static std::unordered_map<void*, Allocation> allocations;
static bool externalAvailable = true;
static unsigned allocationAttempts = 0, fallbackAllocations = 0;
static std::string failAllocationTag;
static unsigned failAllocationCount = 0;

static bool allocationFails(AllocPref policy, const char* tag) {
  ++allocationAttempts;
  check(policy == AllocPref::PreferPSRAM, "all production allocations prefer PSRAM");
  if (failAllocationCount && failAllocationTag == tag) {
    --failAllocationCount;
    return true;
  }
  return false;
}
static void* ps_alloc(size_t size, AllocPref policy, const char* tag) {
  if (allocationFails(policy, tag)) return nullptr;
  void* result = std::malloc(size);
  check(result != nullptr, "host allocation succeeds");
  allocations[result] = {size, tag, externalAvailable};
  if (!externalAvailable) ++fallbackAllocations;
  return result;
}
static void* ps_realloc(void* old, size_t size, AllocPref policy, const char* tag) {
  if (allocationFails(policy, tag)) return nullptr;
  void* result = std::realloc(old, size);
  check(result != nullptr, "host realloc succeeds");
  if (old) allocations.erase(old);
  allocations[result] = {size, tag, externalAvailable};
  if (!externalAvailable) ++fallbackAllocations;
  return result;
}
static void ps_free(void* pointer) {
  if (!pointer) return;
  check(allocations.erase(pointer) == 1, "owned allocation is released exactly once");
  std::free(pointer);
}
static void fileReadTestFree(void* pointer) { ps_free(pointer); }

// INSERT_PRODUCTION_BUFFER_HERE
// INSERT_PRODUCTION_LIMITS_HERE

static std::vector<String> tokens;
static bool quotedPath = true;
class CommandArgs {
 public:
  explicit CommandArgs(const String&) {}
  bool has(int index) const { return index >= 0 && (size_t)index < tokens.size(); }
  int count() const { return (int)tokens.size(); }
  const String& arg(int index) const { return tokens.at((size_t)index); }
  int argInt(int index, int defaultValue) const {
    return has(index) ? (int)arg(index).toInt() : defaultValue;
  }
};
static const char* requireQuotedPath(const CommandArgs& args, int index, String& path) {
  if (!quotedPath || !args.has(index)) return "quoted path required";
  path = args.arg(index);
  return nullptr;
}
enum Source { SOURCE_SERIAL, SOURCE_HTTP, SOURCE_BLUETOOTH };
struct AuthContext { Source transport = SOURCE_SERIAL; String sid = "7"; };
static AuthContext auth;
static const AuthContext& currentAuthContext() { return auth; }
static bool filesystemReady = true, validationOnly = false;
#define RETURN_VALID_IF_VALIDATE_CSTR() do { if (validationOnly) return "VALID"; } while (false)

static unsigned fsDepth = 0, openCalls = 0, closeCalls = 0, readCalls = 0, seekCalls = 0;
static bool fileExists = true, fileDirectory = false;
static std::string fileBytes;
static size_t reportedSize = 0, shortReadLimit = std::numeric_limits<size_t>::max();
static uint32_t lastSeek = 0;
static std::vector<std::string> events;
struct FsLockGuard {
  explicit FsLockGuard(const char* name) { check(std::strcmp(name, "fileread") == 0, "FS guard tag"); ++fsDepth; }
  ~FsLockGuard() { --fsDepth; events.push_back("unlock"); }
};
class File {
 public:
  explicit File(bool valid) : valid_(valid) {}
  explicit operator bool() const { return valid_; }
  bool isDirectory() const { return fileDirectory; }
  size_t size() const { return reportedSize; }
  void close() { check(fsDepth == 1, "file closes under FS lock"); ++closeCalls; valid_ = false; events.push_back("close"); }
  bool seek(uint32_t offset) { check(fsDepth == 1, "seek remains under FS lock"); ++seekCalls; position_ = offset; lastSeek = offset; return true; }
  size_t read(uint8_t* destination, size_t wanted) {
    check(fsDepth == 1, "read remains under FS lock");
    ++readCalls;
    const size_t available = position_ < fileBytes.size() ? fileBytes.size() - position_ : 0;
    const size_t got = std::min({wanted, available, shortReadLimit});
    if (got) std::memcpy(destination, fileBytes.data() + position_, got);
    position_ += got;
    return got;
  }
 private:
  bool valid_;
  size_t position_ = 0;
};
namespace VFS {
static File openGuarded(const String& path, const char* mode, const AuthContext& ctx) {
  check(fsDepth == 1, "guarded VFS open remains under FS lock");
  check(&ctx == &auth, "installed caller identity is preserved");
  check(std::strcmp(mode, "r") == 0, "open mode is read-only");
  check(std::strcmp(path.c_str(), tokens[0].c_str()) == 0, "path is forwarded unchanged");
  ++openCalls;
  return File(fileExists);
}
}

static bool secureEstablished = true, binarySendSucceeds = true;
static unsigned binarySends = 0;
static std::string binaryBody;
static unsigned attemptsAtBinarySend = 0;
#if ENABLE_BLUETOOTH
static bool bleScEstablished(uint16_t id) { check(id == 7, "existing connection ID is preserved"); return secureEstablished; }
static bool bleScSendEncrypted(uint16_t id, const char* bytes, size_t length,
                               bool blocking, bool binaryFrame) {
  check(id == 7 && blocking && binaryFrame, "raw BLE framing/pacing flags are preserved");
  check(fsDepth == 0, "BLE send cannot hold the filesystem lock");
  check(length > 0, "zero-length EOF never sends a binary body");
  ++binarySends;
  bool foundMetadata = false;
  for (const auto& item : allocations) {
    if (item.second.tag != "fileread.reply") continue;
    JsonDocument metadata;
    check(!deserializeJson(metadata, (const char*)item.first), "complete valid metadata exists before body send");
    check(metadata["enc"] == "raw" && metadata["len"].as<size_t>() == length,
          "preflight metadata describes the binary body");
    check(metadata["data"].isNull(), "raw metadata has no data field");
    foundMetadata = true;
  }
  check(foundMetadata, "binary send requires persistent preflight metadata storage");
  attemptsAtBinarySend = allocationAttempts;
  binaryBody.assign(bytes, length);
  events.push_back("binary");
  return binarySendSucceeds;
}
#endif

#define free fileReadTestFree
// INSERT_PRODUCTION_FILEREAD_HERE
#undef free

static std::string referenceEscape(const std::string& text) {
  std::string result;
  constexpr char hex[] = "0123456789abcdef";
  for (unsigned char c : text) {
    switch (c) {
      case '"': result += "\\\""; break;
      case '\\': result += "\\\\"; break;
      case '\b': result += "\\b"; break;
      case '\f': result += "\\f"; break;
      case '\n': result += "\\n"; break;
      case '\r': result += "\\r"; break;
      case '\t': result += "\\t"; break;
      default:
        if (c < 32) { result += "\\u00"; result += hex[c / 16]; result += hex[c % 16]; }
        else result += (char)c;
    }
  }
  return result;
}
static std::string decodeBase64(const std::string& encoded) {
  const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string result;
  uint32_t bits = 0;
  int available = 0;
  for (char byte : encoded) {
    if (byte == '=') break;
    const size_t index = alphabet.find(byte);
    check(index != std::string::npos, "base64 output uses the standard alphabet");
    bits = (bits << 6) | (uint32_t)index;
    available += 6;
    if (available >= 8) { available -= 8; result += (char)((bits >> available) & 255); }
  }
  return result;
}
static const char* lastReply = nullptr;
static std::string invoke(const std::string& path = "/a", long offset = 0,
                          long length = 0, std::initializer_list<const char*> flags = {}) {
  tokens = {String(path), String(std::to_string(offset)), String(std::to_string(length))};
  for (const char* flag : flags) tokens.emplace_back(flag);
  events.clear();
  binaryBody.clear();
  lastReply = cmd_fileread(String("mock tokenization boundary"));
  events.push_back("metadata-return");
  check(fsDepth == 0, "every handler path releases the FS lock");
  check(lastReply != nullptr && std::strlen(lastReply) < CMD_RESULT_MAX,
        "every reply fits the command buffer including NUL");
  for (const auto& item : allocations) {
    check(item.second.tag != "fileread.chunk", "raw read buffer is released before return");
  }
  return std::string(lastReply);
}
static void fixture(const std::string& bytes, size_t total = 0) {
  fileBytes = bytes;
  reportedSize = total ? total : bytes.size();
  shortReadLimit = std::numeric_limits<size_t>::max();
  fileExists = true; fileDirectory = false;
  quotedPath = true; filesystemReady = true; validationOnly = false;
  secureEstablished = true; binarySendSucceeds = true;
  auth.transport = SOURCE_SERIAL; auth.sid = "7";
}
static JsonDocument parseSuccess(const std::string& reply) {
  JsonDocument document;
  check(!deserializeJson(document, reply), "reply is valid JSON");
  check(document["success"] == true, "successful read is marked successful");
  return document;
}
static void checkPayload(const std::string& reply, const std::string& expected,
                          const std::string& path, size_t offset, bool eof) {
  auto document = parseSuccess(reply);
  check(document["path"].as<std::string>() == path, "path round-trips byte-for-byte");
  check(document["size"].as<size_t>() == reportedSize, "total file size is preserved");
  check(document["offset"].as<size_t>() == offset, "returned offset is preserved");
  check(document["len"].as<size_t>() == expected.size(), "len counts source bytes, not encoded bytes");
  check(document["eof"].as<bool>() == eof, "EOF describes actual returned bytes");
  const auto encoding = document["enc"].as<std::string>();
  const auto data = document["data"].as<std::string>();
  check(encoding == "b64" || encoding == "utf8", "ordinary reply uses the established encoding names");
  check((encoding == "b64" ? decodeBase64(data) : data) == expected,
        "payload round-trips all source bytes");
  const std::string expectedWire = "{\"success\":true,\"path\":\"" + referenceEscape(path) +
      "\",\"size\":" + std::to_string(reportedSize) + ",\"offset\":" + std::to_string(offset) +
      ",\"len\":" + std::to_string(expected.size()) + ",\"eof\":" + (eof ? "true" : "false") +
      ",\"enc\":\"" + encoding + "\",\"data\":\"" + referenceEscape(data) + "\"}";
  check(reply == expectedWire, "wire field order, types, escaping and formatting match the old contract");
  const size_t measured = fileReadEnvelopeLength(fileReadJsonStringLength(
      (const uint8_t*)path.data(), path.size()), reportedSize, offset, expected.size(), eof,
      encoding.c_str(), true) + (encoding == "b64" ? ((expected.size() + 2) / 3) * 4 :
                                  fileReadJsonStringLength((const uint8_t*)expected.data(), expected.size()));
  check(measured == reply.size(), "budget calculation exactly matches serialized output");
}

int main(int argc, char** argv) {
  try {
    check(commandInputLengthAccepted(CMD_INPUT_MAX), "shared command input boundary remains available");
    externalAvailable = argc < 2 || std::strcmp(argv[1], "internal-fallback") != 0;
    fixture("hello");
    auth.transport = SOURCE_BLUETOOTH;
    failAllocationTag = "fileread.reply"; failAllocationCount = 1;
    const unsigned readsBeforeOOM = readCalls;
    check(invoke("/a", 0, 5, {"bin"}) == "{\"success\":false,\"error\":\"OOM\"}",
          "initial reply OOM returns a static complete error");
    check(binarySends == 0 && readCalls == readsBeforeOOM, "reply OOM cannot deliver body bytes");
    failAllocationTag = "fileread.chunk"; failAllocationCount = 1;
    check(invoke("/a", 0, 5, {"bin"}) == "{\"success\":false,\"error\":\"OOM\"}",
          "raw buffer OOM returns a static complete error");
    check(binarySends == 0, "chunk OOM cannot deliver body bytes");
    auth.transport = SOURCE_SERIAL;
    const std::string first = invoke();
    checkPayload(first, "hello", "/a", 0, true);
    const char* persistent = lastReply;
    check(allocations.at((void*)lastReply).size == CMD_RESULT_MAX, "response owns exactly one bounded 4096-byte allocation");
    check(allocations.at((void*)lastReply).external == externalAvailable, "mock fallback preserves functional output");
    check(invoke() == first && lastReply == persistent, "response survives return and retains stable storage on reuse");

    for (const std::string& bytes : std::vector<std::string>{"", "f", "fo", "foo", "foob", "fooba", "foobar",
             "quotes: \"slash\\\n\t\r", std::string("a\0b", 3), "\xc3\xa9", "\xff"}) {
      fixture(bytes);
      checkPayload(invoke(), bytes, "/a", 0, true);
      const auto forced = invoke("/a", 0, 0, {"b64", "ignored"});
      checkPayload(forced, bytes, "/a", 0, true);
      check(parseSuccess(forced)["enc"] == "b64", "explicit base64 flag is preserved");
    }
    fixture("foobar");
    check(parseSuccess(invoke("/a", 0, 0, {"b64"}))["data"] == "Zm9vYmFy", "canonical base64 vector");
    std::string allBytes;
    for (unsigned i = 0; i < 256; ++i) allBytes += (char)i;
    fixture(allBytes);
    checkPayload(invoke(), allBytes, "/a", 0, true);

    fixture("0123456789");
    checkPayload(invoke("/a", -4, 3), "012", "/a", 0, false);
    checkPayload(invoke("/a", 3, 4), "3456", "/a", 3, false);
    check(lastSeek == 3, "requested positive offset reaches File::seek");
    checkPayload(invoke("/a", 99, 5), "", "/a", 10, true);
    shortReadLimit = 2;
    checkPayload(invoke("/a", 0, 8), "01", "/a", 0, false);
    shortReadLimit = 0;
    checkPayload(invoke("/a", 0, 8), "", "/a", 0, false);

    fixture(std::string(6000, 'a'), 100000);
    auto ordinary = invoke();
    check(parseSuccess(ordinary)["len"] == 2925, "ordinary path preserves existing raw read window");
    checkPayload(ordinary, std::string(2925, 'a'), "/a", 0, false);
    const std::string escapedPath = "/" + std::string(1200, '\\');
    auto bounded = invoke(escapedPath, 0, 4096, {"b64"});
    size_t got = parseSuccess(bounded)["len"].as<size_t>();
    check(got > 0 && got < ((4096 - 192 - escapedPath.size()) / 4) * 3,
          "escape-heavy path reduces the formerly unsafe raw read window");
    checkPayload(bounded, std::string(got, 'a'), escapedPath, 0, false);
    const std::string boundary4094 = invoke(escapedPath + "a", 0, 4096, {"b64"});
    check(boundary4094.size() == CMD_RESULT_MAX - 2, "test reaches the 4094-character reply boundary");
    got = parseSuccess(boundary4094)["len"].as<size_t>();
    checkPayload(boundary4094, std::string(got, 'a'), escapedPath + "a", 0, false);
    const unsigned readsBeforeLongPath = readCalls;
    check(invoke("/" + std::string(700, '\x01')) == "Error: File path too long for this transport",
          "oversized escaped envelope fails explicitly instead of zero-progress/partial success");
    check(readCalls == readsBeforeLongPath, "unrepresentable path is rejected before reading payload");

    bool exactCapacityCovered = false, textFallbackCovered = false;
    for (size_t length = 1; length <= 1900; length += 17) {
      const std::string path = "/" + std::string(length, '\\');
      fixture(std::string(6000, '\xff'), 100000);
      const auto reply = invoke(path, 0, 4096);
      got = parseSuccess(reply)["len"].as<size_t>();
      checkPayload(reply, std::string(got, '\xff'), path, 0, false);
      if (reply.size() == CMD_RESULT_MAX - 1) exactCapacityCovered = true;
    }
    check(exactCapacityCovered, "test reaches the exact 4095-character valid reply boundary");
    for (size_t count = 1950; count < 2050; ++count) {
      fixture(std::string(count, '\t'));
      const auto reply = invoke();
      checkPayload(reply, fileBytes, "/a", 0, true);
      const bool expectedB64 = fileReadEnvelopeLength(2, count, 0, count, true, "utf8", true) + 2 * count >= CMD_RESULT_MAX - 1;
      check((parseSuccess(reply)["enc"] == "b64") == expectedB64, "escaped-text fallback preserves the established threshold");
      textFallbackCovered |= expectedB64;
    }
    check(textFallbackCovered, "test crosses escaped text to base64 fallback boundary");
    for (size_t offset : {0u, 9u, 10u, 99u, 100u, 9999u}) {
      fixture(std::string(16000, 'x'), 100000);
      auto reply = invoke("/num", (long)offset, 3, {"b64"});
      checkPayload(reply, "xxx", "/num", offset, false);
    }
    fixture("a");
    const std::string allEscapesPath = "/\"\\\b\f\n\r\t\x01\xc3\xa9";
    checkPayload(invoke(allEscapesPath), "a", allEscapesPath, 0, true);

    fixture("binary\0bytes", 12);
    fileBytes = std::string("binary\0bytes", 12);
    auth.transport = SOURCE_BLUETOOTH;
    auto raw = invoke("/a", 0, 12, {"b64", "bin"});
#if ENABLE_BLUETOOTH
    check(parseSuccess(raw)["enc"] == "raw", "secure BLE bin takes priority over explicit b64");
    check(binaryBody == fileBytes, "binary message contains original bytes");
    check(attemptsAtBinarySend == allocationAttempts, "no response allocation occurs after binary delivery");
    check(events == std::vector<std::string>({"close", "unlock", "binary", "metadata-return"}),
          "close/unlock/body/metadata order is preserved");
    check(raw.size() == fileReadEnvelopeLength(2, 12, 0, 12, true, "raw", false), "raw metadata budget is exact");
    unsigned sends = binarySends;
    raw = invoke("/a", 12, 12, {"bin"});
    check(parseSuccess(raw)["enc"] == "raw" && parseSuccess(raw)["len"] == 0, "raw EOF retains raw metadata");
    check(binarySends == sends, "raw EOF sends no body");
    binarySendSucceeds = false;
    auto fallback = invoke("/a", 0, 12, {"bin"});
    checkPayload(fallback, fileBytes, "/a", 0, true);
    check(attemptsAtBinarySend == allocationAttempts, "failed binary send fallback needs no late allocation");
    secureEstablished = false;
    sends = binarySends;
    checkPayload(invoke("/a", 0, 12, {"bin"}), fileBytes, "/a", 0, true);
    check(binarySends == sends, "unsecured BLE cannot send raw data");
    secureEstablished = true; auth.sid = "0";
    checkPayload(invoke("/a", 0, 12, {"bin"}), fileBytes, "/a", 0, true);
    check(binarySends == sends, "zero session ID cannot send raw data");
    fixture("plain text");
    auth.transport = SOURCE_BLUETOOTH;
    binarySendSucceeds = false;
    auto plainFallback = invoke("/a", 0, 0, {"bin"});
    checkPayload(plainFallback, fileBytes, "/a", 0, true);
    check(parseSuccess(plainFallback)["enc"] == "utf8", "failed bin send preserves printable-text utf8 fallback");
    checkPayload(invoke("/a", 0, 0, {"bin", "b64"}), fileBytes, "/a", 0, true);
    check(parseSuccess(std::string(lastReply))["enc"] == "b64", "failed bin send preserves explicit b64 priority");
#else
    checkPayload(raw, fileBytes, "/a", 0, true);
    check(parseSuccess(raw)["enc"] == "b64" && binarySends == 0, "non-BLE build ignores bin and honors b64");
    check(attemptsAtBinarySend == 0, "non-BLE build has no raw send path");
#endif
    fixture(std::string("binary\0bytes", 12));
    auth.transport = SOURCE_HTTP; auth.sid = "7";
    checkPayload(invoke("/a", 0, 12, {"bin"}), fileBytes, "/a", 0, true);

    fixture("secret");
    const unsigned opensBeforeEarly = openCalls;
    validationOnly = true;
    check(invoke() == "VALID", "validate-only returns without opening a file");
    validationOnly = false; filesystemReady = false;
    check(invoke() == "Error: LittleFS not ready", "not-ready literal remains unchanged");
    filesystemReady = true; quotedPath = false;
    check(invoke() == "{\"success\":false,\"error\":\"path must be a quoted token\"}", "quoted-path error contract remains unchanged");
    check(openCalls == opensBeforeEarly, "early errors do not touch VFS");
    quotedPath = true; fileExists = false;
    check(invoke() == "{\"success\":false,\"error\":\"Not found or access denied\"}", "guarded missing/denied response remains unchanged");
    fileExists = true; fileDirectory = true;
    check(invoke() == "{\"success\":false,\"error\":\"Not found or access denied\"}", "directories remain rejected");

    {
      PsramBuffer tiny(16, "fileread.test");
      check(!fileReadBuildReply(tiny, String("/a"), 0, 0, nullptr, 0, true, "utf8", true),
            "bounded encoder propagates capacity failure instead of partial success");
      check(!tiny.ok() && !tiny.append("x"), "writer failure remains sticky");
      tiny.clear();
      check(tiny.append("ok") && tiny.ok(), "explicit clear permits safe reuse");
    }
    check(allocations.size() == 1, "only the persistent command response remains allocated");
    check(closeCalls > 0 && seekCalls > 0, "file close and range seek paths were exercised");
    if (!externalAvailable) check(fallbackAllocations > 0, "internal-fallback mock profile was exercised");
    std::cout << "fileread BLE=" << ENABLE_BLUETOOTH << " "
              << (externalAvailable ? "PSRAM" : "internal-fallback") << ": "
              << assertions << " checks passed\n";
  } catch (const std::exception& error) {
    std::cerr << "fileread harness failed after " << assertions << " checks: " << error.what() << '\n';
    return 1;
  }
}
