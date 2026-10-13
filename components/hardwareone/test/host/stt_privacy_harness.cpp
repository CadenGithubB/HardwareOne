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
// A session change at the two points delivery cannot hold a lock across: the
// output-drain wait before the physical write, and the write itself.
static bool revokeDuringDrain=false, revokeDuringWrite=false;
bool transportSessionEpochIsLive(int, uint32_t epoch) { return epoch==liveEpoch; }
bool cliModeOwnedBySession(int, uint32_t) { return false; }
void debugWaitOutputDrained(int) { if(revokeDuringDrain) liveEpoch=8; }
uint32_t gOutputFlags=MSG_ROUTE_SERIAL|MSG_ROUTE_WEB|MSG_ROUTE_FILE;
static std::vector<std::string> shared;
static std::string ble, mirror;
static uint16_t bleTarget=0;
static int bleSends=0;
// The epoch of the session that holds the target connection now. A re-login
// on the same connection id publishes a new epoch for it.
static uint32_t bleConnEpoch=7;
struct SerialFake {
  std::string out;
  size_t write(const uint8_t* p,size_t n) { if(revokeDuringWrite) liveEpoch=8; out.append(reinterpret_cast<const char*>(p),n); return n; }
  size_t write(uint8_t c) { out.push_back(c); return 1; }
} Serial;
String originPrefix(const char*, const String&, const String&) { return ""; }
void broadcastCommandResultQueued(const String&,const String& body,uint8_t) { shared.push_back(body.s); }
// Bluetooth.cpp's session-fenced sender: delivers only while the connection
// still holds the expected session epoch (0 is never a session).
bool sendBLEResponseToSession(uint16_t conn,uint32_t epoch,const char* p,size_t n,bool=true) {
  if(epoch==0||epoch!=bleConnEpoch) return false;
  bleTarget=conn; ble.assign(p,n); ++bleSends; return true;
}
// The pre-fix sender: whichever session holds the connection now.
bool sendBLEResponseToConn(uint16_t conn,const char* p,size_t n) { return sendBLEResponseToSession(conn,bleConnEpoch,p,n); }
// The console mirror (every logged-in non-guest session).
bool sendBLEResponse(const char* p,size_t n,bool=true) { mirror.append(p,n); return true; }
// INSERT_REDACTOR
// INSERT_BROADCAST
// INSERT_DELIVER
static void reset() { shared.clear(); ble.clear(); mirror.clear(); bleTarget=0; bleSends=0; bleConnEpoch=7; Serial.out.clear(); liveEpoch=7; revokeDuringDrain=revokeDuringWrite=false; }
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
  reset(); revokeDuringDrain=true; deliverCommandResult(secret,ctx);
  assert(shared.empty() && Serial.out.empty());
  // Bytes already on the wire cannot be recalled; the shared mirror must not follow them.
  reset(); revokeDuringWrite=true; deliverCommandResult(secret,ctx);
  assert(Serial.out==secret.s+"\n" && shared.empty());
  reset(); deliverCommandResult("ordinary result",ctx);
  assert(Serial.out=="ordinary result\n" && shared.size()==1 && shared[0]=="ordinary result");
  const String path=R"({"state":"done","transcriptPath":"/stt/u2/example.txt"})";
  assert(redactOutputForLog(path).s=="[private STT result]");
  assert(redactOutputForLog("{\"transcriptPath\":\"partial").s=="[private STT result]");
  reset();ctx.origin=ORIGIN_SERIAL;ctx.outputMask=MSG_ROUTE_SERIAL|MSG_ROUTE_WEB|MSG_ROUTE_FILE;
  deliverCommandResult(path,ctx);
  assert(Serial.out==path.s+"\n"&&shared.size()==1&&shared[0]=="[private STT result]");
  // QuickNote: every `note` reply carries the same producer tag, so a read
  // window or a page of titles reaches only the direct reply (R22).
  const String noteRead=R"({"success":true,"error":"","transcriptPath":"/stt/u7/notes/boot-0000000000000001.txt","sttText":"buy oat milk"})";
  const String noteList=R"({"notes":[{"title":"Buy oat milk"}],"success":true,"error":"","transcriptPath":true})";
  for(const String* note:{&noteRead,&noteList}){
    assert(redactOutputForLog(*note).s=="[private STT result]");
    reset();ctx.origin=ORIGIN_SERIAL;ctx.outputMask=MSG_ROUTE_SERIAL|MSG_ROUTE_WEB|MSG_ROUTE_FILE;
    deliverCommandResult(*note,ctx);
    assert(Serial.out==note->s+"\n"&&shared.size()==1&&shared[0]=="[private STT result]");
    reset();ctx.origin=ORIGIN_BLUETOOTH;ctx.auth.sid="23";ctx.outputMask=MSG_ROUTE_BLE|MSG_ROUTE_WEB|MSG_ROUTE_FILE;
    deliverCommandResult(*note,ctx);
    assert(bleTarget==23&&ble==note->s&&shared.size()==1&&shared[0]=="[private STT result]");
    ctx.auth.sid="";
  }
  // D1: the targeted BLE reply is fenced on the session the command ran for.
  // Connection 0 is a live Bluedroid link.
  int d1Failures=0;
  auto d1Check=[&](bool ok,const char* what){ if(!ok){fprintf(stderr,"FAIL D1: %s\n",what);++d1Failures;} };
  reset();ctx=CommandContext();ctx.origin=ORIGIN_BLUETOOTH;ctx.auth.sid="0";
  ctx.outputMask=MSG_ROUTE_BLE|MSG_ROUTE_FILE;
  deliverCommandResult(secret,ctx);
  d1Check(bleSends==1&&bleTarget==0&&ble==secret.s&&mirror.empty(),
          "a result for the session on conn 0 did not reach exactly that session");
  // A re-login on the same connection id after the liveness check: the old
  // session's result must not reach the replacement session.
  reset();ctx.auth.sid="23";bleConnEpoch=9;
  broadcastOutput(secret,ctx);
  d1Check(bleSends==0&&mirror.empty(),
          "a result for session epoch 7 reached the session that replaced it on conn 23 (epoch 9)");
  d1Check(shared.size()==1&&shared[0]=="[private STT result]","the shared copy was not redacted");
  // A BLE-routed context with no admitted session epoch has no verified recipient.
  reset();ctx.transportSessionEpoch=0;ctx.behaviorFlags=0;
  broadcastOutput("ordinary result",ctx);
  d1Check(bleSends==0&&mirror.empty(),
          "a BLE reply without a session epoch went to whichever session holds the connection");
  // No connection id: no direct BLE recipient, and never the console mirror
  // with the unredacted original.
  reset();ctx=CommandContext();ctx.origin=ORIGIN_BLUETOOTH;ctx.auth.sid="";
  ctx.outputMask=MSG_ROUTE_BLE|MSG_ROUTE_FILE;
  deliverCommandResult(secret,ctx);
  d1Check(bleSends==0&&mirror.empty(),
          "a BLE result without a connection id went to the console-mirror sessions with the private STT original");
  d1Check(shared.size()==1&&shared[0]=="[private STT result]","the shared copy was not redacted");
  reset();ctx.auth.sid="not-a-conn";
  deliverCommandResult("ordinary result",ctx);
  d1Check(bleSends==0&&mirror.empty(),
          "a BLE result with a non-numeric session id fell back to the console-mirror sessions");
  d1Check(shared.size()==1&&shared[0]=="ordinary result","the queued copy changed");
  if(d1Failures) return 1;
  puts("STT result privacy: escaped text, targeted serial/BLE, shared redaction, QuickNote replies and stale session suppression passed");
  puts("BLE command result: session-epoch fenced targeted reply (conn 0 live, re-login on the same conn, no epoch), no console-mirror fallback passed");
}
