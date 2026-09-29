#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
class String {
 public:
  std::string s;
  String() = default;
  String(const char* p): s(p ? p : "") {}
  String(std::string p): s(std::move(p)) {}
  size_t length() const { return s.length(); }
  const char* c_str() const { return s.c_str(); }
  char operator[](size_t i) const { return s[i]; }
  int indexOf(const char* p, size_t from=0) const { auto at=s.find(p, from); return at==s.npos ? -1 : static_cast<int>(at); }
  int indexOf(char p, size_t from=0) const { auto at=s.find(p, from); return at==s.npos ? -1 : static_cast<int>(at); }
  String substring(size_t begin) const { return s.substr(begin); }
  String substring(size_t begin, size_t end) const { return s.substr(begin,end-begin); }
  String operator+(const String& b) const { return s+b.s; }
  String& operator+=(const String& b) { s+=b.s; return *this; }
  String& operator+=(char c) { s+=c; return *this; }
  long toInt() const { return std::stol(s); }
  void reserve(size_t n) { s.reserve(n); }
};
#define DEBUG_CMD_FLOWF(...) ((void)0)
#define BROADCAST_PRINTF(...) ((void)0)
enum { ORIGIN_SERIAL, ORIGIN_WEB, ORIGIN_AUTOMATION, ORIGIN_BLUETOOTH,
       ORIGIN_G2_HIJACK, ORIGIN_ESPNOW, ORIGIN_LOCAL_DISPLAY, ORIGIN_MQTT,
       ORIGIN_VOICE, ORIGIN_UART, ORIGIN_SYSTEM };
enum { MSG_ROUTE_SERIAL=1, MSG_ROUTE_WEB=2, MSG_ROUTE_FILE=4, MSG_ROUTE_BLE=8,
       MSG_ROUTE_OLED=16, MSG_ROUTE_G2=32 };
constexpr uint32_t COMMAND_CONTEXT_REQUIRE_LIVE_SESSION = 1;
struct Auth { int transport=1; String user="owner",ip="local",sid=""; };
struct CommandContext { int origin=ORIGIN_SERIAL; Auth auth; uint32_t outputMask=0;
  uint32_t behaviorFlags=COMMAND_CONTEXT_REQUIRE_LIVE_SESSION,transportSessionEpoch=7;
  bool validateOnly=false; };
static uint32_t liveEpoch=7;
static bool finalDeliveryAllowed=true;
bool transportSessionEpochIsLive(int, uint32_t epoch) { return epoch==liveEpoch; }
bool serialTransportSessionBeginDelivery(uint32_t epoch) { return finalDeliveryAllowed && epoch==liveEpoch; }
void serialTransportSessionEndDelivery() {}
bool cliModeOwnedBySession(int, uint32_t) { return false; }
void debugWaitOutputDrained(int) {}
uint32_t gOutputFlags=MSG_ROUTE_SERIAL|MSG_ROUTE_WEB|MSG_ROUTE_FILE;
static std::vector<std::string> shared;
static std::string ble;
static uint16_t bleTarget=0;
struct SerialFake {
  std::string out;
  size_t write(const uint8_t* p,size_t n) { out.append(reinterpret_cast<const char*>(p),n); return n; }
  size_t write(uint8_t c) { out.push_back(c); return 1; }
} Serial;
String originPrefix(const char*, const String&, const String&) { return ""; }
void broadcastCommandResultQueued(const String&,const String& body,uint8_t) { shared.push_back(body.s); }
void sendBLEResponseToConn(uint16_t conn,const char* p,size_t n) { bleTarget=conn; ble.assign(p,n); }
void sendBLEResponse(const char*,size_t) { assert(false && "must target the session"); }
// INSERT_REDACTOR
// INSERT_BROADCAST
// INSERT_DELIVER
static void reset() { shared.clear(); ble.clear(); bleTarget=0; Serial.out.clear(); liveEpoch=7; finalDeliveryAllowed=true; }
int main() {
  const String secret=R"({"id":"abcdef0100000001","state":"done","sttText":"a quote: \"escaped\" then \\ and \n private sentence"})";
  assert(redactOutputForLog(secret).s=="[private STT result]");
  assert(redactOutputForLog(String("OK: ")+secret).s=="[private STT result]");
  assert(redactOutputForLog("{\"sttText\":\"truncated").s=="[private STT result]");
  assert(redactOutputForLog("ordinary result").s=="ordinary result");
  reset(); CommandContext ctx; ctx.outputMask=MSG_ROUTE_SERIAL|MSG_ROUTE_WEB|MSG_ROUTE_FILE;
  deliverCommandResult(secret,ctx);
  assert(Serial.out==secret.s+"\n");
  assert(shared.size()==1 && shared[0]=="[private STT result]");
  reset(); ctx.origin=ORIGIN_BLUETOOTH; ctx.auth.sid="23";
  ctx.outputMask=MSG_ROUTE_BLE|MSG_ROUTE_WEB|MSG_ROUTE_FILE;
  deliverCommandResult(secret,ctx);
  assert(bleTarget==23 && ble==secret.s);
  assert(shared.size()==1 && shared[0]=="[private STT result]");
  reset(); ctx.origin=ORIGIN_WEB; ctx.outputMask=MSG_ROUTE_WEB|MSG_ROUTE_FILE;
  broadcastOutput(secret,ctx);
  assert(shared.size()==1 && shared[0]=="[private STT result]");
  reset(); ctx.origin=ORIGIN_SERIAL; ctx.outputMask=MSG_ROUTE_SERIAL|MSG_ROUTE_WEB|MSG_ROUTE_FILE;
  liveEpoch=8; deliverCommandResult(secret,ctx);
  assert(shared.empty() && Serial.out.empty() && ble.empty());
  reset(); finalDeliveryAllowed=false; deliverCommandResult(secret,ctx);
  assert(shared.empty() && Serial.out.empty());
  reset(); deliverCommandResult("ordinary result",ctx);
  assert(Serial.out=="ordinary result\n" && shared.size()==1 && shared[0]=="ordinary result");
  const String path=R"({"state":"done","transcriptPath":"/stt/u2/example.txt"})";
  assert(redactOutputForLog(path).s=="[private STT result]");
  assert(redactOutputForLog("{\"transcriptPath\":\"partial").s=="[private STT result]");
  reset();ctx.origin=ORIGIN_SERIAL;ctx.outputMask=MSG_ROUTE_SERIAL|MSG_ROUTE_WEB|MSG_ROUTE_FILE;
  deliverCommandResult(path,ctx);
  assert(Serial.out==path.s+"\n"&&shared.size()==1&&shared[0]=="[private STT result]");
  puts("STT result privacy: escaped text, targeted serial/BLE, shared redaction and stale session suppression passed");
}
