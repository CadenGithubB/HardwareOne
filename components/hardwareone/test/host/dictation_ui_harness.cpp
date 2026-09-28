#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#define DICTATION_MAX_TEXT 256
#define DEBUG_G2F(...) ((void)0)
#define DEBUG_DISPLAYF(...) ((void)0)
using TransportSessionEpoch=uint32_t;
constexpr uint32_t kNoTransportSessionEpoch=0;
enum CommandSource {SOURCE_INTERNAL,SOURCE_G2_GLASSES,SOURCE_LOCAL_DISPLAY};
// INSERT_TYPES
static std::string pending;
static uint16_t offset=0;
static uint32_t sequence=1;
static CommandSource owner=SOURCE_G2_GLASSES;
static bool live=true,commitOK=true;
static std::vector<size_t> commits;
static int fullCalls=0,cancelCalls=0,returnCalls=0,beginCalls=0,paints=0;
static DictationSnapshot snap{};
bool dictationPeekTextFor(CommandSource src,char* out,size_t cap,DictationTextReceipt* r){
 if(src!=owner||!live||pending.empty()){if(cap)out[0]=0;return false;}
 size_t n=std::min(pending.size(),cap-1);memcpy(out,pending.data(),n);out[n]=0;*r={123,sequence,offset,static_cast<uint16_t>(n)};return true;
}
bool dictationCommitTextFor(CommandSource src,const DictationTextReceipt& r,size_t n){
 if(!live||!commitOK||src!=owner||r.offset!=offset||n>r.length)return false;
 commits.push_back(n);offset+=n;pending.erase(0,n);return true;
}
void dictationFieldFullFor(CommandSource){++fullCalls;pending.clear();snap.state=DictationState::FAILED;}
void dictationCancelFor(CommandSource){++cancelCalls;pending.clear();}
void dictationCancel(){dictationCancelFor(SOURCE_LOCAL_DISPLAY);}
void dictationTick(){}
DictationSnapshot dictationSnapshotNow(){return snap;}
struct TextEntry {uint32_t serial=1;bool padMode=true,isSecret=false;size_t len=0,maxLen=256;char buf[257]={};};
static TextEntry gTE;
struct TextEntryExecGuard {TextEntryExecGuard(uint32_t,bool){} explicit operator bool() const{return live;}};
bool sessionCurrent(){return live;}
void pushBuffer(){++paints;}
size_t g2TextEntryPadRemaining(bool* needsSeparator=nullptr);
// INSERT_FIELD
static KbdPadState gKbdPad{};
static bool gKbdPadMicServicePending=false;
static uint32_t gKbdPadMicPresentationEpoch=7;
constexpr uint8_t kKbdPageMic=3;
constexpr uint32_t kKbdMicCountdownHoldMs=1000;
struct G2TextEntryOperationGuard{explicit operator bool() const{return live;}};
struct G2HijackCtxGuard {G2HijackCtxGuard(){} ~G2HijackCtxGuard(){}};
uint32_t g2PresentationEpochCurrent(){return 7;}
bool g2TextEntryIsActive(){return live;}
uint32_t millis(){return 10000;}
void kbdPadMicServiceSuspend(){}
void kbdPadMicServiceResume(){}
bool kbdPushGridMask(const char*,uint8_t){++paints;return true;}
bool kbdPushGridForce(const char*){++paints;return true;}
bool kbdPadMicStatusChanged(bool){return true;}
void g2TextEntryPadEvent(char){}
bool dictationBeginFor(CommandSource,uint32_t){++beginCalls;return true;}
bool kbdPadReturnToKeys(const char*,bool){++returnCalls;gKbdPad.micSessionStarted=false;return true;}
// INSERT_G2
enum class OLEDKeyboardDictationPolicy {DENY,ALLOW_PLAINTEXT};
constexpr int KEYBOARD_MODE_MIC=5,KEYBOARD_MODE_LOWERCASE=1;
struct OLEDKeyboardState{bool active=true;OLEDKeyboardDictationPolicy dictationPolicy=OLEDKeyboardDictationPolicy::ALLOW_PLAINTEXT;int mode=KEYBOARD_MODE_MIC,textLength=0,maxLength=256;char text[257]={};};
static OLEDKeyboardState gOledKeyboardState;
void oledMarkDirty(){++paints;}
// INSERT_OLED
static void reset(){pending.clear();offset=0;sequence=1;owner=SOURCE_G2_GLASSES;live=commitOK=true;commits.clear();fullCalls=cancelCalls=returnCalls=beginCalls=paints=0;gTE=TextEntry{};gOledKeyboardState=OLEDKeyboardState{};gKbdPad=KbdPadState{};gKbdPad.active=true;gKbdPad.page=kKbdPageMic;gKbdPad.micSessionStarted=true;snap=DictationSnapshot{};snap.state=DictationState::RECORDING;}
int main(){
 reset();pending="first";kbdPadDictationServiceOnTap(7);assert(std::string(gTE.buf)=="first"&&commits==std::vector<size_t>{5}&&!returnCalls);
 pending=" second piece";offset=5;kbdPadDictationServiceOnTap(7);assert(std::string(gTE.buf)=="first second piece"&&!returnCalls); // no artificial space inside chunk
 pending="next";offset=0;sequence=2;kbdPadDictationServiceOnTap(7);assert(std::string(gTE.buf)=="first second piece next");
 snap.state=DictationState::WAITING;kbdPadDictationServiceOnTap(7);assert(!returnCalls);snap.state=DictationState::IDLE;kbdPadDictationServiceOnTap(7);assert(returnCalls==1);
 reset();pending="abcde";gTE.maxLen=3;kbdPadDictationServiceOnTap(7);assert(std::string(gTE.buf)=="abc"&&commits==std::vector<size_t>{3}&&fullCalls==1&&!returnCalls);
 reset();strcpy(gTE.buf,"a");gTE.len=1;gTE.maxLen=2;pending="word";assert(kbdPadConsumeDictationText());assert(std::string(gTE.buf)=="a"&&commits.empty()&&fullCalls==1);
 reset();pending="a\"b";assert(kbdPadConsumeDictationText());assert(std::string(gTE.buf)=="ab"&&commits==std::vector<size_t>{3});
 reset();gTE.isSecret=true;pending="private";kbdPadDictationServiceOnTap(7);assert(!gTE.len&&commits.empty());
 reset();live=false;pending="revoked";kbdPadDictationServiceOnTap(7);assert(!gTE.len&&commits.empty());
 reset();gKbdPad.micCountdown=1;gTE.maxLen=0;kbdPadDictationServiceOnTap(7);assert(!beginCalls&&!gKbdPad.micSessionStarted);
 reset();gKbdPad.micCountdown=1;kbdPadDictationServiceOnTap(7);assert(beginCalls==1&&gKbdPad.micSessionStarted);
 reset();commitOK=false;pending="accepted before cancellation";assert(kbdPadConsumeDictationText());assert(cancelCalls==1&&commits.empty());
 reset();owner=SOURCE_LOCAL_DISPLAY;pending="first";oledKeyboardDictationTick();assert(std::string(gOledKeyboardState.text)=="first"&&commits==std::vector<size_t>{5});
 pending=" next";offset=5;oledKeyboardDictationTick();assert(std::string(gOledKeyboardState.text)=="first next");pending="phrase";offset=0;sequence=2;oledKeyboardDictationTick();assert(std::string(gOledKeyboardState.text)=="first next phrase");
 reset();owner=SOURCE_LOCAL_DISPLAY;gOledKeyboardState.maxLength=3;pending="abcdef";oledKeyboardDictationTick();assert(std::string(gOledKeyboardState.text)=="abc"&&commits==std::vector<size_t>{3}&&fullCalls==1);
 reset();owner=SOURCE_LOCAL_DISPLAY;gOledKeyboardState.dictationPolicy=OLEDKeyboardDictationPolicy::DENY;pending="private";oledKeyboardDictationTick();assert(!gOledKeyboardState.textLength&&commits.empty()&&cancelCalls==1);
 reset();owner=SOURCE_LOCAL_DISPLAY;live=false;pending="private";oledKeyboardDictationTick();assert(!gOledKeyboardState.textLength&&commits.empty());
 puts("Shared OLED/G2 consumers: real field filtering/capacity, piece commits, countdown and continuous service lifecycle passed");
}
