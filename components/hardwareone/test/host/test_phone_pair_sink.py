#!/usr/bin/env python3
"""`phone pair new`'s code reaches exactly one sink (phone link v2).

The command and its pairing window (BLE_PhonePipe.cpp) run as shipped on the
phone runtime co-simulation (phone_runtime_harness.cpp, built by
test_phone_runtime.generated with HW1_PAIR_SINK=1), together with the real
one-shot serial sink from HardwareOne.cpp (serialCommandSecretSinkReady,
serialWriteCommandSecret) and the real code-screen sink from OLED_Utils.cpp
(oledPhonePairCodeSinkReady, oledShowPhonePairCode). Every output path the
firmware has is a recording stand-in here: Serial (write/print/println/printf),
broadcastOutput and broadcastOutputCore_Routed, the debug queue
(debugQueuePrintf and the BROADCAST_PRINTF/DEBUG macros), the OLED console
(gOledConsole), logSystemEvent and every ESP_LOG level. Checks:
- serial: one Serial call, a single write of the whole line after the output
  drain; the code is in nothing else (the command result, broadcasts, the debug
  queue, the OLED console, the logs), also through the window's attempts;
- the serial sink's gates (the same as deliverCommandResult's): a non-serial
  origin, serial output off for the command or the console, validate-only, a
  session that is not live or ends during the drain, no command context:
  nothing written, and the command refuses before the secret changes;
- a short write closes the window and the command reports it;
- the OLED: no Serial call at all, the code only in the window the code screen
  reads; the screen's gates (display origin, live display session, a running
  display) and its one mode request;
- a session (display or serial) that ends after the entry checks, while the
  pipe stops and the link restarts: the sink is checked again under the lock
  that opens the window, so no window opens, no mode request is posted, no
  Serial call is made and the code screen has nothing to draw;
- the audit line for `phone pair new` (System_Utils.cpp kRules) is pinned by
  test_llm_http_settings.py, which runs the real redactor.

--component points at another copy of components/hardwareone (mutation runs).
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

from test_phone_runtime import COMPONENT, generated
from test_web_batch_handlers import extract_block

HERE = Path(__file__).resolve().parent

FAKES = r'''
// ---- The command context the sinks read (System_CommandTypes.h) -------------------
#define ENABLE_OLED_DISPLAY 1
enum CommandOrigin{ORIGIN_SERIAL,ORIGIN_WEB,ORIGIN_AUTOMATION,ORIGIN_LOCAL_DISPLAY,ORIGIN_UART};
struct CommandContext{CommandOrigin origin=ORIGIN_WEB;AuthContext auth;uint32_t outputMask=0;
 uint32_t transportSessionEpoch=0;bool validateOnly=false;};
static CommandContext sinkCtx;static bool haveCtx=true;
static void* currentCommandContext(){return haveCtx?&sinkCtx:nullptr;}
enum:uint32_t{MSG_ROUTE_SERIAL=1u<<0,MSG_ROUTE_WEB=1u<<1,MSG_ROUTE_FILE=1u<<2,MSG_ROUTE_OLED=1u<<3,MSG_ROUTE_BLE=1u<<4,
 MSG_ROUTE_G2=1u<<5};
static volatile uint32_t gOutputFlags=MSG_ROUTE_SERIAL;
// ---- Every output path, recorded --------------------------------------------------
static std::vector<std::string> callOrder;  // Serial calls and the drain, in order
static std::vector<std::string> serialOut,broadcasts,debugQueue,oledConsole,events_;
static size_t serialShortBy=0;              // a USB-CDC short write
static std::function<void()> onDrain;
struct FakeSerial{
 size_t write(const uint8_t* b,size_t n){callOrder.push_back("write");serialOut.emplace_back(reinterpret_cast<const char*>(b),n);
  return n>serialShortBy?n-serialShortBy:0;}
 size_t write(uint8_t c){callOrder.push_back("write");serialOut.emplace_back(1,char(c));return 1;}
 size_t print(const char* s){callOrder.push_back("print");serialOut.emplace_back(s);return std::strlen(s);}
 size_t println(const char* s=""){callOrder.push_back("println");serialOut.emplace_back(s);return std::strlen(s)+1;}
 __attribute__((format(printf,2,3))) size_t printf(const char* f,...){
  char b[512];va_list a;va_start(a,f);std::vsnprintf(b,sizeof(b),f,a);va_end(a);
  callOrder.push_back("printf");serialOut.emplace_back(b);return std::strlen(b);}
};
static FakeSerial Serial;
static void debugWaitOutputDrained(uint32_t ms){assert(ms==120);callOrder.push_back("drain");if(onDrain)onDrain();}
[[maybe_unused]] static void broadcastOutput(const char* s){broadcasts.emplace_back(s);}
[[maybe_unused]] static void broadcastOutputCore_Routed(const char* s,size_t n,uint8_t){broadcasts.emplace_back(s,n);}
__attribute__((format(printf,2,3))) [[maybe_unused]] static void debugQueuePrintf(uint32_t,const char* f,...){
 char b[512];va_list a;va_start(a,f);std::vsnprintf(b,sizeof(b),f,a);va_end(a);debugQueue.emplace_back(b);}
#define BROADCAST_PRINTF(...) debugQueuePrintf(0,__VA_ARGS__)
#define DEBUG_G2F(...) debugQueuePrintf(1,__VA_ARGS__)
#define DEBUG_BLE_COREF(...) debugQueuePrintf(2,__VA_ARGS__)
[[maybe_unused]] static void logSystemEvent(const char* a,const char* b){events_.push_back(std::string(a)+b);}
// gOledConsole: the OLED CLI's history (OLED_ConsoleBuffer.h).
struct FakeConsole{void append(const char* s){oledConsole.emplace_back(s);}};
[[maybe_unused]] static FakeConsole gOledConsole;
// ---- The display the code screen sink posts to ------------------------------------
enum OLEDMode{OLED_BLUETOOTH_PHONE,OLED_BLUETOOTH_PHONE_CODE};
static bool oledConnected=true,gOledRunning=true;
static std::vector<std::pair<OLEDMode,std::string>> modeRequests;
static void requestOLEDMode(OLEDMode mode,const char* reason){assert(ownerDepth==0);modeRequests.emplace_back(mode,reason);}
'''

TESTS = r'''
static bool anyCarries(const std::vector<std::string>& v){for(const auto& s:v)if(carriesCode(s))return true;return false;}
static bool nowhereElse(const std::string& reply){
 return !carriesCode(reply) && !anyCarries(broadcasts) && !anyCarries(debugQueue) && !anyCarries(oledConsole) &&
        !anyCarries(events_) && !anyCarries(logs);
}
static void sinkReset(){
 pairNewReset();callOrder.clear();serialOut.clear();broadcasts.clear();debugQueue.clear();oledConsole.clear();events_.clear();
 modeRequests.clear();serialShortBy=0;onDrain=nullptr;haveCtx=true;oledConnected=gOledRunning=true;
 sinkCtx=CommandContext{};sinkCtx.origin=ORIGIN_SERIAL;sinkCtx.auth.transport=SOURCE_SERIAL;
 sinkCtx.outputMask=MSG_ROUTE_SERIAL|MSG_ROUTE_FILE;sinkCtx.transportSessionEpoch=5;gOutputFlags=MSG_ROUTE_SERIAL;
}
static void displayCommand(){
 authContext.transport=SOURCE_LOCAL_DISPLAY;sinkCtx.origin=ORIGIN_LOCAL_DISPLAY;sinkCtx.auth.transport=SOURCE_LOCAL_DISPLAY;
 sinkCtx.outputMask=MSG_ROUTE_FILE;
}
static void sinkTests(){
 const std::string code=expectedCode(),shown=displayForm(code);
 // Serial: one Serial call, the whole line in one write after the drain.
 {sinkReset();const std::string reply=phonePairNewCommand("");
  assert(reply==kSerialReply && storedSecret==code && pairState==PhonePairWindow::Open);
  assert((callOrder==std::vector<std::string>{"drain","write"}));
  assert(serialOut.size()==1 && serialOut[0]=="\nPairing code (shown once): "+shown+"\n");
  assert(nowhereElse(reply) && modeRequests.empty() && windowCodeWiped());
  // The window's attempts (a miss, then its end) never print or log it either.
  world.on=true;world.advertise=false;tickFlush(clockMs+=10);serveWorker();
  pairCloseWanted=pairId;tickFlush(clockMs+=10);pairNewIdle();
  assert(pairState==PhonePairWindow::Closed && callOrder.size()==2 && nowhereElse(reply));}
 // The serial sink's gates: nothing is written, and the command refuses
 // before any state change.
 for(int gate=0;gate<7;++gate){
  sinkReset();
  switch(gate){
   case 0:sinkCtx.origin=ORIGIN_UART;break;           // never physical presence
   case 1:sinkCtx.origin=ORIGIN_AUTOMATION;break;
   case 2:sinkCtx.outputMask=MSG_ROUTE_FILE;break;      // the command's own serial route off
   case 3:gOutputFlags=MSG_ROUTE_FILE;break;            // outSerial off
   case 4:sinkCtx.validateOnly=true;break;
   case 5:sinkCtx.transportSessionEpoch=6;break;        // not the live serial session
   case 6:haveCtx=false;break;
  }
  assert(!serialCommandSecretSinkReady() && !serialWriteCommandSecret("x",1));
  assert(!std::strcmp(phonePairNewCommand(""),"Error: phone pair new prints its code once on this console; turn serial output on"));
  assert(callOrder.empty() && serialOut.empty() && !randomCalls && replaceCalls.empty() && pairState==PhonePairWindow::None);
 }
 // The session ends while earlier lines drain: the write is refused, the
 // window ends, the command says so; still nothing printed.
 {sinkReset();onDrain=[]{epochLive=false;};const std::string reply=phonePairNewCommand("");
  assert(reply=="Error: the pairing code could not be shown; run phone pair new again");
  assert((callOrder==std::vector<std::string>{"drain"}) && serialOut.empty() && pairState==PhonePairWindow::Closed);
  assert(nowhereElse(reply) && windowCodeWiped());}
 // A short write (the host stopped reading): the window ends.
 {sinkReset();serialShortBy=4;const std::string reply=phonePairNewCommand("");
  assert(reply=="Error: the pairing code could not be shown; run phone pair new again");
  assert((callOrder==std::vector<std::string>{"drain","write"}) && pairState==PhonePairWindow::Closed && nowhereElse(reply));}
 // The OLED: no Serial call, one request for the code screen, and the code
 // only in the window the screen reads.
 {sinkReset();displayCommand();const std::string reply=phonePairNewCommand("");
  assert(reply==kDisplayReply && callOrder.empty() && serialOut.empty() && nowhereElse(reply));
  assert(modeRequests.size()==1 && modeRequests[0].first==OLED_BLUETOOTH_PHONE_CODE && modeRequests[0].second=="phone.pair.new");
  char seen[20];const PhonePairView v=phonePipePairView(seen);assert(v.onDisplay && shown==seen);
  phonePipePairClose(v.id);tickFlush(clockMs+=10);pairNewIdle();
  assert(pairState==PhonePairWindow::Closed && windowCodeWiped() && callOrder.empty() && nowhereElse(reply));}
 // The code screen's gates: a display origin and session, a running display.
 for(int gate=0;gate<5;++gate){
  sinkReset();displayCommand();
  switch(gate){
   case 0:sinkCtx.origin=ORIGIN_WEB;break;
   case 1:sinkCtx.auth.transport=SOURCE_SERIAL;break;
   case 2:sinkCtx.transportSessionEpoch=6;break;
   case 3:gOledRunning=false;break;
   case 4:sinkCtx.validateOnly=true;break;
  }
  assert(!oledPhonePairCodeSinkReady());
  assert(!std::strcmp(phonePairNewCommand(""),"Error: the OLED cannot show a pairing code now"));
  assert(modeRequests.empty() && callOrder.empty() && !randomCalls && pairState==PhonePairWindow::None);
 }
 // The display stopped after the check: the screen cannot be shown, the window ends.
 {sinkReset();displayCommand();oledConnected=false;assert(!oledShowPhonePairCode() && modeRequests.empty());}
 // The session ends after the entry checks, here while the link restarts (a
 // logout, then another login): the sink is checked again under the lock that
 // opens the window. No window, no code-screen request, no Serial call, nothing
 // for the code screen to draw; the secret did change (never shown).
 for(int display=0;display<2;++display){
  sinkReset();if(display)displayCommand();
  onPipeStart=[]()->bool{active=true;stopWanted=false;pskValid=true;epochLive=false;return true;};
  const std::string reply=phonePairNewCommand("");
  assert(reply=="Error: the session ended; run phone pair new again" && storedSecret==code);
  assert(pairState==PhonePairWindow::None && modeRequests.empty() && callOrder.empty() && serialOut.empty());
  char seen[20];std::memset(seen,'x',sizeof(seen));const PhonePairView v=phonePipePairView(seen);
  assert(v.state==PhonePairWindow::None && !v.onDisplay && !v.epoch);
  for(char ch:seen)assert(!ch);
  assert(nowhereElse(reply) && windowCodeWiped());
 }
 puts("phone pair new sink: serial is one write of the whole line after the drain and nowhere else (result, "
      "broadcasts, debug queue, OLED console, events, logs), gated like deliverCommandResult (origin, routes, "
      "validate-only, live session, the drain); a short write ends the window; the OLED gets one code-screen "
      "request and no Serial call, gated on a live display session; a session that ends after entry gets no "
      "window, request or write (the sink checked again under the window's lock)");
}
'''


def build(component: Path) -> str:
    unit = generated(component)
    hw1 = (component / 'HardwareOne.cpp').read_text()
    oled = (component / 'OLED_Utils.cpp').read_text()
    sinks = '\n'.join([extract_block(hw1, 'bool serialCommandSecretSinkReady()'),
                       extract_block(hw1, 'bool serialWriteCommandSecret('),
                       extract_block(oled, 'bool oledPhonePairCodeSinkReady()'),
                       extract_block(oled, 'bool oledShowPhonePairCode()')])
    for marker, text in (('// INSERT_SERIAL_SINK', FAKES + sinks), ('// INSERT_SINK_TESTS', TESTS)):
        assert unit.count(marker + '\n') == 1, marker
        unit = unit.replace(marker + '\n', text + '\n')
    return unit


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--component', type=Path, default=COMPONENT)
    args = parser.parse_args()
    component = args.component.resolve()
    with tempfile.TemporaryDirectory(prefix='hw1-phone-pair-sink-') as tmp:
        unit = Path(tmp) / 'sink.cpp'
        binary = Path(tmp) / 'sink'
        unit.write_text(build(component))
        command = [os.environ.get('CXX', 'clang++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                   '-Wno-unused-function', '-Wno-unused-variable', '-Wno-unused-parameter', '-DHW1_PAIR_SINK=1',
                   '-DCONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=1', '-I', str(component), str(unit), '-o', str(binary)]
        if args.sanitize:
            command[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g']
        subprocess.run(command, check=True)
        out = subprocess.run([str(binary)], check=True, timeout=100, capture_output=True, text=True).stdout
        print(out.strip().splitlines()[-1])


if __name__ == '__main__':
    main()
