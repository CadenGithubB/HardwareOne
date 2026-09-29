#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <new>
#include <string>
#include <vector>
class String{std::string s;public:String()=default;String(const char*t):s(t?t:""){}size_t length()const{return s.size();}const char*c_str()const{return s.c_str();}};
using TransportSessionEpoch=uint32_t;constexpr TransportSessionEpoch kNoTransportSessionEpoch=0;
enum {BLE_PEER_G2_GLASSES=1,ORIGIN_G2_HIJACK=3,MSG_ROUTE_FILE=8};
constexpr uint32_t COMMAND_CONTEXT_MODE_INDEPENDENT=2,COMMAND_CONTEXT_REQUIRE_LIVE_SESSION=4;
struct AuthContext{String user;int transport=7;};
struct BlePeerOwnerSession{String user="alice";uint32_t generation=1,transportEpoch=10;bool live()const{return user.length()&&generation&&transportEpoch;}};
static BlePeerOwnerSession currentOwner;
static bool replaceAfterCapture=false,queueAccepts=true;
static unsigned userCallbacks=0;
static uint32_t gMenuGen=8;static uint64_t gCmdSeq=0;
struct G2CmdCookie{uint64_t seq=0;uint32_t menuGen=0;int targetPage=23;uint8_t targetNetSub=0;};
// INSERT_CALLBACK
using ExecAsyncCallback=void(*)(bool,const char*,void*);
struct Command{String line;struct{int origin=0;AuthContext auth;uint32_t id=0,timestampMs=0;TransportSessionEpoch transportSessionEpoch=0;uint32_t behaviorFlags=0,outputMask=0;bool validateOnly=false,captureOutput=false;void*replyHandle=nullptr;void*httpReq=nullptr;}ctx;};
struct Queued{Command command;ExecAsyncCallback callback;void*data;};
static std::vector<Queued> queued;
AuthContext g2HijackAuthContextForOwner(BlePeerOwnerSession*out){*out=currentOwner;AuthContext auth;auth.user=out->user;if(replaceAfterCapture){++currentOwner.transportEpoch;++currentOwner.generation;}return auth;}
bool blePeerOwnerSessionIsCurrent(int,const BlePeerOwnerSession&expected){return currentOwner.live()&&expected.transportEpoch==currentOwner.transportEpoch&&expected.generation==currentOwner.generation&&std::string(expected.user.c_str())==currentOwner.user.c_str();}
String redactCmdForAudit(const String&s){return s;}
uint32_t millis(){return 99;}
bool commandInputLengthAccepted(size_t n){return n<=1024;}
bool submitCommandAsync(const Command&cmd,ExecAsyncCallback cb,void*data){if(!queueAccepts)return false;queued.push_back({cmd,cb,data});return true;}
template<class...T>void quietLog(const char*,T...){ }
#define WARN_COMMANDF(...) quietLog(__VA_ARGS__)
#define ERROR_MEMORYF(...) quietLog(__VA_ARGS__)
#define DEBUG_G2F(...) quietLog(__VA_ARGS__)
// INSERT_PRODUCTION
static void userCallback(bool ok,const char*result,const G2CmdCookie&cookie,void*data){assert(ok&&std::string(result)=="owned result"&&cookie.menuGen==gMenuGen&&data==reinterpret_cast<void*>(1));++userCallbacks;}
static void deliver(){assert(queued.size()==1);auto q=queued.front();queued.clear();q.callback(true,"owned result",q.data);}
int main(){
    const G2CmdCookie cookie{};
    assert(!g2SubmitHijackCommand("transcription start",cookie,userCallback,reinterpret_cast<void*>(1),9));assert(queued.empty());
    assert(g2SubmitHijackCommand("transcription start",cookie,userCallback,reinterpret_cast<void*>(1),10));
    assert(queued[0].command.ctx.transportSessionEpoch==10);
    assert((queued[0].command.ctx.behaviorFlags&COMMAND_CONTEXT_REQUIRE_LIVE_SESSION)!=0);
    assert((queued[0].command.ctx.behaviorFlags&COMMAND_CONTEXT_MODE_INDEPENDENT)!=0);
    deliver();assert(userCallbacks==1);
    assert(g2SubmitHijackCommand("transcription status",cookie,userCallback,reinterpret_cast<void*>(1),10));
    ++currentOwner.transportEpoch;++currentOwner.generation;deliver();assert(userCallbacks==1);
    // Replacement after the coherent capture cannot relabel the queued command.
    currentOwner.transportEpoch=20;replaceAfterCapture=true;
    assert(g2SubmitHijackCommand("transcription start",cookie,userCallback,reinterpret_cast<void*>(1),20));
    assert(currentOwner.transportEpoch==21&&queued[0].command.ctx.transportSessionEpoch==20);deliver();assert(userCallbacks==1);replaceAfterCapture=false;
    // Existing immediate four-argument callers retain their default semantics.
    assert(g2SubmitHijackCommand("settings",cookie,userCallback,reinterpret_cast<void*>(1)));deliver();assert(userCallbacks==2);
    queueAccepts=false;assert(!g2SubmitHijackCommand("transcription start",cookie,userCallback,reinterpret_cast<void*>(1),21));assert(queued.empty());queueAccepts=true;
    currentOwner.user="";assert(!g2SubmitHijackCommand("transcription start",cookie,userCallback,reinterpret_cast<void*>(1),21));assert(queued.empty());
    std::puts("G2 actual command admission/completion epoch tests passed");
}
