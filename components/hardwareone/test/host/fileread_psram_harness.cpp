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
    std::string text_;
 public:
    String()=default;
    String(const char* s):text_(s?s:""){}
    String(const std::string& s):text_(s){}
    long toInt()const{return std::strtol(text_.c_str(),nullptr,10);}
    size_t length()const{return text_.size();}
    const char* c_str()const{return text_.c_str();}
    char operator[](size_t i)const{return i<length()?text_[i]:0;}
    bool startsWith(const String& s)const{return text_.rfind(s.text_,0)==0;}
    bool endsWith(const String& s)const{return length()>=s.length() && text_.compare(length()-s.length(),s.length(),s.text_)==0;}
    int indexOf(const char* s)const{auto n=text_.find(s);return n==std::string::npos?-1:int(n);}
    int lastIndexOf(char c)const{auto n=text_.rfind(c);return n==std::string::npos?-1:int(n);}
    String substring(size_t i)const{return text_.substr(std::min(i,length()));}
    void reserve(size_t n){text_.reserve(n);}
    void remove(size_t i){text_.erase(i);}
    void toLowerCase(){for(char& c:text_)c=char(std::tolower(static_cast<unsigned char>(c)));}
    void trim(){const auto a=text_.find_first_not_of(" \t\r\n"),b=text_.find_last_not_of(" \t\r\n");text_=a==std::string::npos?"":text_.substr(a,b-a+1);}
    String& operator+=(char c){text_+=c;return *this;}
    String& operator+=(const String& s){text_+=s.text_;return *this;}
    friend String operator+(const String& a,const String& b){return a.text_+b.text_;}
    friend bool operator==(const String& a,const String& b){return a.text_==b.text_;}
    friend bool operator!=(const String& a,const String& b){return !(a==b);}
};

// INSERT_POLICY_HERE
// INSERT_PAGER_HERE

enum class AllocPref { PreferPSRAM };
struct Allocation { size_t size; std::string tag; bool external; };
static std::unordered_map<void*, Allocation> allocations;
static bool externalAvailable = true;
static unsigned allocationAttempts = 0, fallbackAllocations = 0;
static std::string failAllocationTag;
static unsigned failAllocationCount = 0;
static std::unordered_map<std::string, size_t> lastAllocationSize;  // by tag

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
  lastAllocationSize[tag] = size;
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
  bool argWasQuoted(int index) const { return index == 0 && quotedPath; }
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
static unsigned existsCalls=0,viewReads=0;
static std::vector<std::string> broadcasts;
static char debugBuffer[1024];
static bool ensureDebugBuffer(){return true;}
static char* getDebugBuffer(){return debugBuffer;}
static void broadcastOutput(const char* text){broadcasts.emplace_back(text);}
template<class... Args>static void cliHintf(const char*,Args...){ }
static bool readTextLimited(const char*,String& text,size_t cap){++viewReads;text=fileBytes.substr(0,cap);return true;}
static void captureCryptoRevealText(String&){ }
namespace VFS {
static bool existsGuarded(const String&,const AuthContext& ctx){check(&ctx==&auth,"fileview preserves caller identity");++existsCalls;return fileExists;}
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
static uint16_t expectedConnId = 7;
// The running command's session epoch (CommandContext) and the session's live
// one: a session renewed since the command was admitted is not confidential.
using TransportSessionEpoch = uint32_t;
struct CommandContext { TransportSessionEpoch transportSessionEpoch = 41; };
#if ENABLE_BLUETOOTH
static CommandContext commandContext;
static bool haveCommandContext = true;
static void* currentCommandContext() { return haveCommandContext ? &commandContext : nullptr; }
static TransportSessionEpoch liveEpoch = 41;
// Bluetooth.cpp: an established Secure Channel or the phone link's console row,
// for this epoch; the body goes as a binary message or a BLOB chain, fenced on it.
static bool bleSessionConfidential(uint16_t id, TransportSessionEpoch epoch) {
  check(id == expectedConnId, "existing connection ID is preserved");
  return secureEstablished && epoch != 0 && epoch == liveEpoch;
}
static bool bleSendBinaryToSession(uint16_t id, TransportSessionEpoch epoch,
                                   const uint8_t* body, size_t length) {
  const char* bytes = reinterpret_cast<const char*>(body);
  check(id == expectedConnId && epoch == commandContext.transportSessionEpoch,
        "the raw body is fenced on the command's connection and session epoch");
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
  auth.transport = SOURCE_SERIAL; auth.sid = "7"; expectedConnId = 7;
}
static JsonDocument parseSuccess(const std::string& reply) {
  JsonDocument document;
  check(!deserializeJson(document, reply), "reply is valid JSON");
  check(document["success"] == true, "successful read is marked successful");
  return document;
}
static void checkPayload(const std::string& reply, const std::string& expected,
                          const std::string& path, size_t offset, bool eof,
                          bool privateTranscript = false) {
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
  check(document["transcriptPath"].isNull() == !privateTranscript,
        "only private transcript replies carry the shared-sink privacy tag");
  if(privateTranscript)check(document["transcriptPath"] == true,"privacy tag is boolean true");
  const std::string expectedWire = "{\"success\":true,\"path\":\"" + referenceEscape(path) +
      "\",\"size\":" + std::to_string(reportedSize) + ",\"offset\":" + std::to_string(offset) +
      ",\"len\":" + std::to_string(expected.size()) + ",\"eof\":" + (eof ? "true" : "false") +
      ",\"enc\":\"" + encoding + "\"" + (privateTranscript ? ",\"transcriptPath\":true" : "") +
      ",\"data\":\"" + referenceEscape(data) + "\"}";
  check(reply == expectedWire, "wire field order, types, escaping and formatting match the old contract");
  const size_t measured = fileReadEnvelopeLength(fileReadJsonStringLength(
      (const uint8_t*)path.data(), path.size()), reportedSize, offset, expected.size(), eof,
      encoding.c_str(), true, privateTranscript) + (encoding == "b64" ? ((expected.size() + 2) / 3) * 4 :
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
    // Bluedroid numbers connections from 0: the first link is a real one.
    secureEstablished = true; binarySendSucceeds = true; auth.sid = "0"; expectedConnId = 0;
    raw = invoke("/a", 0, 12, {"bin"});
    check(parseSuccess(raw)["enc"] == "raw", "connection ID 0 takes the raw path");
    check(binarySends == sends + 1 && binaryBody == fileBytes, "connection ID 0 sends the raw body");
    auth.sid = "65535"; expectedConnId = 65535; sends = binarySends;
    check(parseSuccess(invoke("/a", 0, 12, {"bin"}))["enc"] == "raw" && binarySends == sends + 1,
          "the largest connection ID takes the raw path");
    // The session was renewed (logout, revoke, a new console stream) since the
    // command was admitted, or no command context: no raw body, base64.
    auth.sid = "7"; expectedConnId = 7; sends = binarySends;
    commandContext.transportSessionEpoch = 40;
    checkPayload(invoke("/a", 0, 12, {"bin"}), fileBytes, "/a", 0, true);
    check(binarySends == sends, "a command from a renewed session sent a raw body");
    commandContext.transportSessionEpoch = 41;
    haveCommandContext = false;
    checkPayload(invoke("/a", 0, 12, {"bin"}), fileBytes, "/a", 0, true);
    check(binarySends == sends, "a command without its context sent a raw body");
    haveCommandContext = true;
    // No usable connection ID: bleSessionConfidential is never asked (its
    // check would reject any ID) and the reply falls back to base64.
    expectedConnId = 7;
    for (const char* sid : {"", "abc", "7x", "-1", " 7", "65536", "99999999999"}) {
      auth.sid = sid; sends = binarySends;
      checkPayload(invoke("/a", 0, 12, {"bin"}), fileBytes, "/a", 0, true);
      check(binarySends == sends, "an empty or non-numeric session ID cannot send raw data");
    }
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

    // Exercise the actual legacy viewer: private aliases stop before any
    // existence/read/broadcast path, while an ordinary page still renders.
    for(const char* path:{"/stt/u2/session.txt","/sd/stt/u2/session.txt"," //./stt//u2/session.txt ",
                         "/sd/STT/u2/a.txt","/sd/stt./u2/a.txt","/sd/\\stt/u2/a.txt",
                         "/sd/.\\stt/u2/a.txt","/sd/stt/u2~1/a.txt"}) {
      fixture("private spoken words");tokens={String(path)};broadcasts.clear();
      const unsigned beforeExists=existsCalls,beforeRead=viewReads;
      check(std::strcmp(cmd_fileview(""),"Use fileread or the file browser to read private transcripts")==0,
            "private fileview redirects to owner-scoped readers");
      check(existsCalls==beforeExists&&viewReads==beforeRead&&broadcasts.empty(),
            "private fileview cannot read or broadcast content");
    }
    fixture("ordinary page");tokens={String("/ordinary.txt")};broadcasts.clear();
    check(std::strcmp(cmd_fileview(""),"[FS] File displayed")==0,"ordinary fileview remains available");
    check(std::find(broadcasts.begin(),broadcasts.end(),"ordinary page")!=broadcasts.end(),
          "ordinary fileview still broadcasts its page");

    // JSON is re-indented by default (the shared re-indenter); `raw`, before or
    // after the page number, shows the stored text. Other files ignore `raw`.
    auto view=[&](std::vector<String> args){tokens=std::move(args);broadcasts.clear();return std::string(cmd_fileview(""));};
    auto shown=[&](const std::string& text){return std::find(broadcasts.begin(),broadcasts.end(),text)!=broadcasts.end();};
    auto headerHas=[&](const char* text){return !broadcasts.empty()&&broadcasts.front().find(text)!=std::string::npos;};
    const std::string json="{\"a\":1,\"b\":[],\"c\":{\"d\":\"x,y\"}}";
    const std::string indented="{\n  \"a\": 1,\n  \"b\": [],\n  \"c\": {\n    \"d\": \"x,y\"\n  }\n}";
    fixture(json);
    check(view({String("/cfg.json")})=="[FS] File displayed"&&shown(indented)&&headerHas(", re-indented)"),
          "fileview re-indents JSON by default");
    check(view({String("/CFG.JSON"),String("1")})=="[FS] File displayed"&&shown(indented),
          "JSON is recognised by the shared classifier, page given");
    for(const auto& args:{std::vector<String>{String("/cfg.json"),String("raw")},
                          std::vector<String>{String("/cfg.json"),String("raw"),String("1")},
                          std::vector<String>{String("/cfg.json"),String("1"),String("raw")}})
      check(view(args)=="[FS] File displayed"&&shown(json)&&!headerHas("re-indented"),
            "raw shows the stored JSON in either argument order");
    fixture("ordinary page");
    check(view({String("/ordinary.txt"),String("raw")})=="[FS] File displayed"&&shown("ordinary page")&&
              !headerHas("re-indented"),
          "raw is accepted and changes nothing for a text file");
    fixture(json);
    for(const auto& args:{std::vector<String>{String("/cfg.json"),String("raw"),String("raw")},
                          std::vector<String>{String("/cfg.json"),String("1"),String("2")}})
      check(view(args)=="Error: unexpected argument — usage: fileview \"<path>\" [page] [raw]"&&broadcasts.empty(),
            "a repeated page or raw is refused before reading");
    check(view({String("/cfg.json"),String("0")})=="Error: page must be >= 1 — usage: fileview \"<path>\" [page] [raw]",
          "page validation names the raw keyword");

    // A re-indented body is sized from a counting pass and stops at the
    // pager's 64 KB offset range, marked truncated; raw stays exact.
    std::string list="[";
    for(int i=0;i<14000;++i) list+="1,";  // ~28 KB stored, ~70 KB re-indented
    list+="1]";
    fixture(list);
    check(view({String("/big.json")})=="[FS] File displayed"&&headerHas(", re-indented, truncated)"),
          "a re-indented body past 64 KB is marked truncated");
    check(lastAllocationSize["fs.fileview.body"]==65535,"re-indented body is capped at the uint16_t page range");
    check(view({String("/big.json"),String("raw")})=="[FS] File displayed"&&!headerHas("truncated"),
          "the same file pages raw without truncation");
    check(lastAllocationSize["fs.fileview.body"]==list.size()+list.size()/200+16,"raw body keeps its exact sizing");
    fixture(json);
    check(view({String("/small.json")})=="[FS] File displayed"&&
              lastAllocationSize["fs.fileview.body"]==indented.size()+indented.size()/200+16,
          "a small re-indented body is sized from the counted output");
    for(const char* path:{"/stt/u2/session.txt","/sd/stt/u2/session.txt"," //./stt//u2/session.txt "}) {
      fixture("words: \"quoted\"\n");
      checkPayload(invoke(path),fileBytes,path,0,true,true);
      checkPayload(invoke(path,0,0,{"b64"}),fileBytes,path,0,true,true);
      checkPayload(invoke(path,5,4),fileBytes.substr(5,4),path,5,false,true);
      checkPayload(invoke(path,999,4),"",path,fileBytes.size(),true,true);
    }
    // Include the tag in exact-capacity calculations, also on escape-heavy
    // private names and both possible encodings.
    bool privateBoundary=false;
    for(size_t n=1;n<1900;n+=17){
      const std::string path="/stt/u2/"+std::string(n,'\\');
      fixture(std::string(6000,'\xff'),100000);
      const auto reply=invoke(path,0,4096);const size_t count=parseSuccess(reply)["len"].as<size_t>();
      checkPayload(reply,std::string(count,'\xff'),path,0,false,true);
      privateBoundary|=reply.size()==CMD_RESULT_MAX-1;
    }
    check(privateBoundary,"private replies exercise exact capacity with the marker included");
#if ENABLE_BLUETOOTH
    fixture("private raw bytes");auth.transport=SOURCE_BLUETOOTH;
    const std::string privatePath="/stt/u2/session.txt";
    const auto privateRaw=invoke(privatePath,0,0,{"bin"});
    auto privateMeta=parseSuccess(privateRaw);
    check(privateMeta["enc"]=="raw"&&privateMeta["transcriptPath"]==true&&privateMeta["data"].isNull(),
          "secure raw delivery returns tagged metadata without copying body into JSON");
    check(binaryBody==fileBytes,"private secure raw body remains byte-identical");
    check(privateRaw.size()==fileReadEnvelopeLength(privatePath.size(),fileBytes.size(),0,fileBytes.size(),true,"raw",false,true),
          "private raw metadata budget is exact");
    binarySendSucceeds=false;
    checkPayload(invoke(privatePath,0,0,{"bin"}),fileBytes,privatePath,0,true,true);
#endif

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
