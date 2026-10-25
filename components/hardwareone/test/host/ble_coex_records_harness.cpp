#include "BLE_CoexRecords.h"
#include "System_CommandLimits.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#ifdef NDEBUG
#error "This harness requires assertions"
#endif
using namespace hw1coex;

static WifiCoexInputs up() {
  WifiCoexInputs in;in.installed=in.transportUp=in.staUp=true;return in;
}
static WifiSpan span(WifiCoex a,WifiCoex b){
  WifiCoexTag t;t.state=a;t.flags=WIFI_COEX_STA_UP|WIFI_COEX_HAS_IP;t.lastReason=201;t.lastDiscAgeS=12;
  WifiSpan w{};w.begin(t);t.state=b;t.flags=WIFI_COEX_STA_UP|WIFI_COEX_RECONNECT_ARMED;t.scans=1;t.disconnects=2;w.finish(t);return w;
}
static ScanRecord scan(Peer peer,ScanOutcome o,uint32_t endedAt){
  ScanRecord r{};r.endedAt=endedAt;r.durationMs=1500;r.controllerTimed=true;r.reports=12;r.matches=o==ScanOutcome::Found?3:0;
  r.firstMatchMs=o==ScanOutcome::Found?210:Tally::kNone;
  r.rssi=-71;r.peer=peer;r.purpose=Purpose::Saved;r.outcome=o;r.wifi=span(WifiCoex::Associated,WifiCoex::Hunting);
  r.interval=96;r.window=96;r.active=false;r.duplicateFilter=false;r.seconds=4;return r;
}
static ConnectRecord connect(Peer peer,ConnectOutcome o,uint16_t reason,uint32_t endedAt){
  ConnectRecord r{};r.endedAt=endedAt;r.elapsedMs=30012;r.peer=peer;r.purpose=Purpose::Saved;r.outcome=o;r.stage=ConnectStage::Open;
  r.gattStatus=o==ConnectOutcome::Connected?0:0x85;r.reason=reason;r.wifi=span(WifiCoex::Connecting,WifiCoex::Associated);
  r.waitMs=ConnectRecord::kNoWait;
  return r;
}
static unsigned countLines(const char* s){unsigned n=0;for(;*s;++s)n+=*s=='\n';return n;}

int main(){
  // ---- Wi-Fi state: every branch, in priority order -------------------------
  {WifiCoexInputs in=up();in.associated=true;in.installed=false;assert(wifiCoexClassify(in).state==WifiCoex::Unknown);}
  {WifiCoexInputs in=up();in.associated=true;in.forcedOff=true;auto t=wifiCoexClassify(in);
   assert(t.state==WifiCoex::Off && (t.flags&WIFI_COEX_FORCED_OFF));}
  {WifiCoexInputs in=up();in.associated=true;in.transportUp=false;auto t=wifiCoexClassify(in);
   assert(t.state==WifiCoex::Off && (t.flags&WIFI_COEX_COMPANION_DOWN));}
  {WifiCoexInputs in=up();in.staUp=false;assert(wifiCoexClassify(in).state==WifiCoex::Off);}
  // A scan while associated runs the scan scheme.
  {WifiCoexInputs in=up();in.associated=true;in.scanOnAir=true;assert(wifiCoexClassify(in).state==WifiCoex::Scanning);}
  // Association wins over a connect scope (NTP runs after the join, inside it).
  {WifiCoexInputs in=up();in.associated=true;in.connecting=true;in.hasIp=true;auto t=wifiCoexClassify(in);
   assert(t.state==WifiCoex::Associated && (t.flags&WIFI_COEX_HAS_IP) && (t.flags&WIFI_COEX_STA_UP));}
  {WifiCoexInputs in=up();in.connecting=true;assert(wifiCoexClassify(in).state==WifiCoex::Connecting);}
  {WifiCoexInputs in=up();in.bootHunt=true;auto t=wifiCoexClassify(in);
   assert(t.state==WifiCoex::Connecting && (t.flags&WIFI_COEX_BOOT_HUNT));}
  // Hunting: auto-reconnect armed, a recent reconnectable disconnect.
  {WifiCoexInputs in=up();in.reconnectArmed=true;in.lastReason=201;in.lastDiscAgeMs=5900;auto t=wifiCoexClassify(in);
   assert(t.state==WifiCoex::Hunting && t.lastReason==201 && t.lastDiscAgeS==5 && (t.flags&WIFI_COEX_RECONNECT_ARMED));}
  {WifiCoexInputs in=up();in.reconnectArmed=true;in.lastReason=kWifiReasonAssocLeave;in.lastDiscAgeMs=100;
   assert(wifiCoexClassify(in).state==WifiCoex::Idle);}  // a voluntary leave is never retried
  {WifiCoexInputs in=up();in.reconnectArmed=true;in.lastReason=201;in.lastDiscAgeMs=kWifiCoexHuntWindowMs;
   assert(wifiCoexClassify(in).state==WifiCoex::Idle);}
  {WifiCoexInputs in=up();in.lastReason=201;in.lastDiscAgeMs=100;assert(wifiCoexClassify(in).state==WifiCoex::Idle);}
  // ESP-NOW parking: AP only, idle station.
  {WifiCoexInputs in=up();in.staUp=false;in.apUp=true;in.espnow=true;auto t=wifiCoexClassify(in);
   assert(t.state==WifiCoex::Idle && (t.flags&WIFI_COEX_ESPNOW) && (t.flags&WIFI_COEX_AP_UP) && !(t.flags&WIFI_COEX_STA_UP));}
  // Disconnect bookkeeping: none, capped, and no stale reason without one.
  {WifiCoexInputs in=up();in.lastReason=7;auto t=wifiCoexClassify(in);assert(t.lastDiscAgeS==255 && t.lastReason==0);}
  {WifiCoexInputs in=up();in.lastDiscAgeMs=900000;assert(wifiCoexClassify(in).lastDiscAgeS==254);}
  // Activity counters pass through whatever the state.
  {WifiCoexInputs in=up();in.installed=false;in.scans=3;in.connects=4;in.disconnects=5;auto t=wifiCoexClassify(in);
   assert(t.scans==3 && t.connects==4 && t.disconnects==5);}

  // ---- Wi-Fi around a record: start/end samples and activity during it -------
  {WifiCoexTag a;a.state=WifiCoex::Associated;a.flags=WIFI_COEX_STA_UP;a.lastReason=8;a.lastDiscAgeS=255;
   a.scans=65535;a.connects=7;a.disconnects=100;
   WifiSpan w{};w.begin(a);
   assert(w.start==WifiCoex::Associated && w.end==WifiCoex::Associated && w.ageS==255 && !w.scans && !w.disconnects);
   WifiCoexTag b=a;b.state=WifiCoex::Hunting;b.flags=WIFI_COEX_STA_UP|WIFI_COEX_RECONNECT_ARMED;b.scans=1;b.connects=7;
   b.disconnects=uint16_t(100+400);b.lastReason=201;b.lastDiscAgeS=0;w.finish(b);
   // The scan counter wrapped; the disconnects saturate; the start's reason stays.
   assert(w.end==WifiCoex::Hunting && w.flagsStart==WIFI_COEX_STA_UP && (w.flagsEnd&WIFI_COEX_RECONNECT_ARMED));
   assert(w.scans==2 && w.connects==0 && w.disconnects==255 && w.reason==8 && w.ageS==255);
   char line[160];formatWifi(line,sizeof(line),w);
   assert(!std::strcmp(line," wifi=associated>hunting wflags=0x04>0x44 wlast=- wscans=2 wconns=0 wdiscs=255"));
   a.lastDiscAgeS=254;a.lastReason=15;w.begin(a);formatWifi(line,sizeof(line),w);assert(std::strstr(line," wlast=15@>254s "));
   a.lastDiscAgeS=3;w.begin(a);formatWifi(line,sizeof(line),w);assert(std::strstr(line," wlast=15@3s "));}
  for(WifiCoex s:{WifiCoex::NoWifi,WifiCoex::Unknown,WifiCoex::Off,WifiCoex::Idle,WifiCoex::Connecting,WifiCoex::Hunting,
                  WifiCoex::Scanning,WifiCoex::Associated})assert(std::strlen(wifiCoexName(s))>2);
  assert(!std::strcmp(wifiCoexName(WifiCoex::Hunting),"hunting") && !std::strcmp(wifiCoexName(WifiCoex::NoWifi),"none"));

  // ---- Tally: every report counts; only the first usable match queues --------
  Tally t;t.reset();
  assert(t.firstMatchMs()==Tally::kNone);
  t.start(1000);assert(t.firstMatchMs()==Tally::kNone);t.report();t.report();
  assert(!t.match(-70,false,1040));   // an unusable address type never queues, but is heard
  assert(t.match(-80,true,1100));     // the first usable match queues
  assert(!t.match(-50,true,1200));    // later matches only count
  assert(t.reports==2 && t.matches==3 && t.bestRssi==-50 && t.queued && t.firstMatchMs()==40);
  t.reset();assert(!t.reports && !t.matches && t.bestRssi==-128 && !t.queued && t.firstMatchMs()==Tally::kNone);
  t.match(-60,true,995);assert(t.firstMatchMs()==Tally::kNone);  // heard, but the start was never confirmed
  t.start(1000);assert(t.firstMatchMs()==0);                       // a match before the recorded start
  t.reset();t.start(0xfffffff0u);t.match(-60,true,0x10);assert(t.firstMatchMs()==0x20);  // across the wrap
  t.reset();
  std::atomic<int> queuedCount{0};
  auto flood=[&](int base){for(int i=0;i<20000;++i){t.report();if(i%4==0 && t.match(base-(i%50),true,uint32_t(i)))++queuedCount;}};
  std::thread a(flood,-20),b(flood,-30);a.join();b.join();
  assert(t.reports==40000 && t.matches==10000 && t.bestRssi==-20 && queuedCount==1);

  // ---- Scan end: tied to this scan's start; the first end wins ---------------
  t.reset();t.end(500);assert(!t.endSpan);  // a late stop of the previous scan, before this start
  t.start(1000);t.end(4000);t.end(5000);assert(t.endSpan==3001);
  // Duration and outcome from what the callbacks saw.
  {auto c=concludeScan(t,ScanOutcome::Missed,900,6000,3);
   assert(c.controller && c.durationMs==3000 && c.outcome==ScanOutcome::Missed);}       // ran its course
  assert(concludeScan(t,ScanOutcome::Missed,900,6000,4).outcome==ScanOutcome::Aborted);  // 3 s of 4: cut short
  {Tally u;u.start(1000);auto c=concludeScan(u,ScanOutcome::Missed,900,5000,4);           // no end seen: task span
   assert(!c.controller && c.durationMs==4000 && c.outcome==ScanOutcome::Missed);}
  {Tally u;u.start(1000);auto c=concludeScan(u,ScanOutcome::Missed,900,3400,4);           // cut short, no end seen
   assert(!c.controller && c.durationMs==2400 && c.outcome==ScanOutcome::Aborted);}
  {Tally u;u.start(1000);u.end(3000);auto c=concludeScan(u,ScanOutcome::Missed,900,3100,4); // cut short, end seen
   assert(c.controller && c.durationMs==2000 && c.outcome==ScanOutcome::Aborted);}
  // The 500 ms margin.
  {Tally u;u.start(1000);u.end(4500);assert(concludeScan(u,ScanOutcome::Missed,900,4600,4).outcome==ScanOutcome::Missed);}
  {Tally u;u.start(1000);u.end(4499);assert(concludeScan(u,ScanOutcome::Missed,900,4600,4).outcome==ScanOutcome::Aborted);}
  {Tally u;u.start(1000);u.end(1000);auto c=concludeScan(u,ScanOutcome::Missed,900,4600,4);  // an end at the start
   assert(c.controller && c.durationMs==0 && c.outcome==ScanOutcome::Aborted);}
  {Tally u;auto c=concludeScan(u,ScanOutcome::Missed,900,1300,4);                         // never confirmed
   assert(!c.controller && c.durationMs==400 && c.outcome==ScanOutcome::StartFailed);}
  {Tally u;u.start(1000);u.match(-60,true,1100);
   assert(concludeScan(u,ScanOutcome::Found,900,1200,4).outcome==ScanOutcome::Found);         // the caller's verdict stands
   assert(concludeScan(u,ScanOutcome::Aborted,900,1200,4).outcome==ScanOutcome::Aborted);}
  {Tally u;u.start(1000);assert(concludeScan(u,ScanOutcome::Missed,900,1100,0).outcome==ScanOutcome::Missed);}
  {Tally u;u.start(0xfffff000u);u.end(0x800);auto c=concludeScan(u,ScanOutcome::Missed,0,0x900,4);  // across the wrap
   assert(c.controller && c.durationMs==0x1800);}

  // ---- Disconnect reason: the first nonzero one for the armed target ---------
  {ReasonLatch l;assert(!l.offer(0) && l.offer(0x3e) && !l.offer(0x08) && l.take()==0x3e && l.take()==0);
   assert(l.offer(0x100) && l.take()==0x100);}

  // ---- The shared log ---------------------------------------------------------
  static Log log{};  // zero storage is an empty log (as in PSRAM .bss)
  char out[4000];
  assert(log.scanLines(out,sizeof(out),0)==0 && log.connectLines(out,sizeof(out),0)==0);
  log.scanCounters(out,sizeof(out),Peer::Phone);
  assert(!std::strcmp(out,"scans total=0 found=0 partial=0 missed=0 aborted=0 start_failed=0"));
  log.addScan(scan(Peer::Phone,ScanOutcome::Missed,10000));
  log.addScan(scan(Peer::Glasses,ScanOutcome::Partial,20000));
  log.addScan(scan(Peer::Phone,ScanOutcome::Found,40000));
  log.scanCounters(out,sizeof(out),Peer::Phone);
  assert(!std::strcmp(out,"scans total=2 found=1 partial=0 missed=1 aborted=0 start_failed=0"));
  log.scanCounters(out,sizeof(out),Peer::Glasses);
  assert(!std::strcmp(out,"scans total=1 found=0 partial=1 missed=0 aborted=0 start_failed=0"));
  // The exact line, newest first, filtered to one peer.
  log.scanLines(out,sizeof(out),45000,Peer::Phone);
  assert(!std::strcmp(out,
      "\n#3 -5s phone saved found dur_ms=1500(ctl) first_ms=210 reports=12 matches=3 rssi=-71"
      " scan=60.00/60.00 passive dupfilter=off up_to_s=4"
      " wifi=associated>hunting wflags=0x0c>0x44 wlast=201@12s wscans=1 wconns=0 wdiscs=2"
      "\n#1 -35s phone saved missed dur_ms=1500(ctl) first_ms=- reports=12 matches=0 rssi=-"
      " scan=60.00/60.00 passive dupfilter=off up_to_s=4"
      " wifi=associated>hunting wflags=0x0c>0x44 wlast=201@12s wscans=1 wconns=0 wdiscs=2"));
  {ScanRecord r=scan(Peer::Ring,ScanOutcome::Aborted,0);r.controllerTimed=false;char line[400];
   Log::scanBody(line,sizeof(line),r);assert(!std::strncmp(line,"ring saved aborted dur_ms=1500(task) ",37));}
  log.scanLines(out,sizeof(out),45000,Peer::None,1);
  assert(countLines(out)==1 && !std::strncmp(out,"\n#3 ",4));
  // Sixteen kept; totals cover the boot.
  for(uint32_t s=0;s<20;++s)log.addScan(scan(s%2?Peer::Ring:Peer::Glasses,s%3?ScanOutcome::Missed:ScanOutcome::Aborted,50000+s));
  assert(log.scanTotal==23);
  const size_t used=log.scanLines(out,sizeof(out),60000);
  assert(used==std::strlen(out) && countLines(out)==16 && !std::strncmp(out,"\n#23 ",5) && std::strstr(out,"\n#8 ") && !std::strstr(out,"\n#7 "));
  log.scanLines(out,sizeof(out),60000,Peer::Phone);assert(out[0]==0);  // phone records aged out of the ring
  // Connects.
  log.addConnect(connect(Peer::Ring,ConnectOutcome::Failed,0x0100,70000));
  log.addConnect(connect(Peer::GlassesLeft,ConnectOutcome::Connected,0,71000));
  log.connectLines(out,sizeof(out),75000);
  assert(!std::strcmp(out,
      "\n#2 -4s g2-left saved connected ms=30012 stage=open gatt=0x00 reason=0x00(-) cause=- wait_ms=-"
      " wifi=connecting>associated wflags=0x0c>0x44 wlast=201@12s wscans=1 wconns=0 wdiscs=2"
      "\n#1 -5s ring saved failed ms=30012 stage=open gatt=0x85 reason=0x100(conn-cancelled) cause=- wait_ms=-"
      " wifi=connecting>associated wflags=0x0c>0x44 wlast=201@12s wscans=1 wconns=0 wdiscs=2"));
  assert(log.connects[unsigned(Peer::Ring)][unsigned(ConnectOutcome::Failed)]==1);
  log.connectCounters(out,sizeof(out),Peer::Ring);
  assert(!std::strcmp(out,"connects total=1 connected=0 failed=1 cancelled=0"));
  log.connectCounters(out,sizeof(out),Peer::Phone);
  assert(!std::strcmp(out,"connects total=0 connected=0 failed=0 cancelled=0"));
  {ConnectRecord r=connect(Peer::Phone,ConnectOutcome::Cancelled,0x3e,0);r.gattStatus=-1;r.stage=ConnectStage::None;
   r.purpose=Purpose::Reconnect;r.cause=14;
   char line[400];Log::formatConnect(line,sizeof(line),r,0);
   assert(std::strstr(line," phone reconnect cancelled ") &&
          std::strstr(line,"stage=- gatt=- reason=0x3e(failed-to-establish) cause=14 wait_ms=- wifi="));}
  // The phone's attempt line: its stage, the cancel source and the gate wait.
  {ConnectRecord r=connect(Peer::Phone,ConnectOutcome::Cancelled,0,0);r.purpose=Purpose::Reconnect;r.gattStatus=-1;
   r.stage=ConnectStage::Discovery;r.cause=14;r.detail=CauseDetail::Expired;r.waitMs=1234;r.elapsedMs=4000;
   char line[400];Log::connectBody(line,sizeof(line),r);
   assert(!std::strncmp(line,"phone reconnect cancelled ms=4000 stage=discovery gatt=- reason=0x00(-) cause=14(expired)"
                             " wait_ms=1234 wifi=connecting>associated ",112));
   // A wait at or past the cap prints as a lower bound.
   r.waitMs=ConnectRecord::capWait(65533);Log::connectBody(line,sizeof(line),r);
   assert(std::strstr(line," wait_ms=65533 "));
   r.waitMs=ConnectRecord::capWait(70000);Log::connectBody(line,sizeof(line),r);
   assert(r.waitMs==ConnectRecord::kWaitCap && std::strstr(line," wait_ms=65534+ "));
   r.detail=CauseDetail::None;r.cause=7;r.waitMs=ConnectRecord::kNoWait;Log::connectBody(line,sizeof(line),r);
   assert(std::strstr(line," cause=7 wait_ms=- wifi="));
   r.detail=CauseDetail::ServiceChanged;r.cause=1;Log::connectBody(line,sizeof(line),r);
   assert(std::strstr(line," cause=1(service-changed) "));}
  // The phone's steps follow Arduino's numbering; every one has a name.
  static_assert(unsigned(ConnectStage::Complete)==8 && unsigned(ConnectStage::Discovery)==9 &&
                unsigned(ConnectStage::Ready)==16, "stage numbering");
  for(unsigned v=unsigned(ConnectStage::Precheck);v<=unsigned(ConnectStage::Ready);++v)
    assert(std::strcmp(name(ConnectStage(v)),"-"));
  for(unsigned v=1;v<=unsigned(CauseDetail::Expired);++v)assert(std::strlen(name(CauseDetail(v)))>4);
  assert(!std::strcmp(name(ConnectStage::Cccd),"cccd") && !std::strcmp(name(ConnectStage::Ready),"ready"));
  // Names used in records.
  assert(!std::strcmp(reasonName(0x08),"supervision-timeout") && !std::strcmp(reasonName(0x13),"peer-ended") &&
         !std::strcmp(reasonName(0x77),"other") && !std::strcmp(name(ConnectStage::OpenTimeout),"open-timeout"));
  assert(!std::strcmp(name(Peer::GlassesRight),"g2-right") && !std::strcmp(name(Purpose::Repair),"repair") &&
         !std::strcmp(name(ScanOutcome::StartFailed),"start-failed"));
  // Worst-case records: a body fits the 320-byte log line, and 16 scan lines
  // plus the header and counters fit the command's 6144-byte reply.
  {static Log big{};WifiSpan w{};{WifiCoexTag a;a.state=WifiCoex::Associated;a.flags=0xff;a.lastReason=255;a.lastDiscAgeS=254;
     w.begin(a);a.scans=a.connects=a.disconnects=1000;w.finish(a);}
   for(int i=0;i<16;++i){ScanRecord r=scan(Peer::GlassesRight,ScanOutcome::StartFailed,0);
     r.durationMs=r.reports=r.matches=4294967295u;r.firstMatchMs=4294967294u;r.rssi=-128;r.interval=r.window=16384;r.seconds=255;
     r.purpose=Purpose::Discovery;r.controllerTimed=false;r.wifi=w;r.seq=4294967295u;big.addScan(r);}
   char body[400];assert(Log::scanBody(body,sizeof(body),big.scan[0])<320-20);
   ConnectRecord c=connect(Peer::GlassesRight,ConnectOutcome::Cancelled,0x100,0);c.purpose=Purpose::Discovery;
   c.elapsedMs=4294967295u;c.stage=ConnectStage::RegisterTimeout;c.gattStatus=255;c.cause=255;c.wifi=w;
   c.detail=CauseDetail::ServiceChanged;c.waitMs=65534;
   assert(Log::connectBody(body,sizeof(body),c)<320-20);
   static Log connects{};for(int i=0;i<16;++i){c.seq=0;connects.addConnect(c);}
   connects.connectTotal=4294967295u-16;for(int i=0;i<16;++i)connects.addConnect(c);
   // In the executor's 4096-byte reply, worst-case lines stay whole: as many as
   // fit, then the note, never a cut line.
   const auto whole=[](const char* text,size_t used,size_t lines){
     assert(used==std::strlen(text) && used<CMD_RESULT_MAX);
     const char* note=std::strstr(text,"\n(older records not shown)");
     const char* end=note?note:text+used;
     assert((!note || note[26]==0) && size_t(std::count(text,end,'\n'))==lines);
     for(const char* p=text;(p=std::strchr(p,'\n'))!=nullptr && p<end;++p)assert(!std::strncmp(p,"\n#",2));};
   static char wideConnects[CMD_RESULT_MAX];
   const size_t c16=connects.connectLines(wideConnects,sizeof(wideConnects),4294967295u);
   const size_t cl=std::count(wideConnects,wideConnects+c16,'#');
   assert(cl>=14);whole(wideConnects,c16,cl);
   static char wide[CMD_RESULT_MAX];
   const size_t s16=big.scanLines(wide,sizeof(wide),4294967295u);
   const size_t sl=std::count(wide,wide+s16,'#');
   assert(sl>=10 && sl<16);whole(wide,s16,sl);
   // Every worst-case line ends with its whole Wi-Fi span.
   assert(std::strstr(wide,"wdiscs=255\n#") && std::strstr(wide,"wdiscs=255\n(older"));}
  // Short buffers hold no cut line: only the note, or nothing, terminated.
  char small[64];std::memset(small,'x',sizeof(small));
  assert(log.scanLines(small,sizeof(small),60000)==26 && !std::strcmp(small,"\n(older records not shown)"));
  char smaller[20];std::memset(smaller,'x',sizeof(smaller));
  assert(log.scanLines(smaller,sizeof(smaller),60000)==0 && smaller[0]==0);
  assert(log.connectLines(smaller,0,60000)==0);
  char tiny[8];assert(log.scanCounters(tiny,sizeof(tiny),Peer::Ring)==7 && tiny[7]==0);
  puts("BLE coexistence records: Wi-Fi state priorities, racing callback tallies, per-peer counters, 16 newest-first records, exact lines");
}
