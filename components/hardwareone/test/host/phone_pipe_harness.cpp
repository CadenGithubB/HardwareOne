#include "BLE_PhonePipeCore.h"
#include "System_LLMHttpTransportCore.h"
#include <algorithm>
#include <cassert>
#include <vector>
#include <cstdio>
using namespace hw1pipe;
namespace hw1pipe {
// Reads the core's private frame buffers to prove they were wiped.
struct SessionProbe {
 static bool zero(const uint8_t* p,size_t n) {for(size_t i=0;i<n;++i)if(p[i])return false;return true;}
 // Every byte outside the ring's live region reads zero.
 static bool ringWiped(const Buffer& b) {
   for(size_t i=0;i<kWindow;++i) if((i+kWindow-b.head)%kWindow>=b.size && b.bytes[i]) return false;
   return true;
 }
 static bool packetZero(const Session& s) {return zero(s.packet_,sizeof(s.packet_));}
 static bool assemblyZero(const Session& s) {return zero(s.assembly_,sizeof(s.assembly_));}
 static bool wiped(const Session& s) {
   for(const auto& st:s.streams) if(!ringWiped(st.tx) || !ringWiped(st.rx)) return false;
   return s.pending_ || packetZero(s);  // packet_ holds plaintext only while a frame is pending
 }
};
}
struct Peer {
 uint32_t seq=0; uint16_t value=512;
 bool send(Session& s,Op op,uint16_t id,const std::vector<uint8_t>& body,uint32_t now=1) {
   size_t offset=0; bool result=true;
   do {
     size_t n=body.size()-offset;if(n>value-kHeader)n=value-kHeader;
     uint8_t p[kValue]{};p[0]=kVersion;p[1]=uint8_t(op);put16(p+2,id);put32(p+4,seq++);put16(p+8,uint16_t(n));
     p[10]=op==Op::Data?0:uint8_t((offset==0?kFirst:0)|(offset+n==body.size()?kLast:0));
     if(n)std::memcpy(p+kHeader,body.data()+offset,n);
     result=s.receive(p,n+kHeader,now);if(!result)return false;offset+=n;
   } while(offset<body.size());return result;
 }
 // One raw frame (any header byte and flags), sequence-checked like the rest.
 bool raw(Session& s,uint8_t version,Op op,uint16_t id,const std::vector<uint8_t>& body,uint8_t flags,uint32_t now=1) {
   std::vector<uint8_t> p(kHeader+body.size());p[0]=version;p[1]=uint8_t(op);put16(p.data()+2,id);
   put32(p.data()+4,seq++);put16(p.data()+8,uint16_t(body.size()));p[10]=flags;
   if(!body.empty())std::memcpy(p.data()+kHeader,body.data(),body.size());
   return s.receive(p.data(),p.size(),now);
 }
};
using Frame=std::vector<uint8_t>;
static std::vector<uint8_t> u32(uint32_t v) {std::vector<uint8_t>b(4);put32(b.data(),v);return b;}
static void drain(Session& s,uint32_t now=0) {uint8_t p[kValue];unsigned count=0;while(s.peek(p)){assert(++count<2000);assert(s.accept(now));}}
// One P4 frame through the submission transaction (empty: nothing to send).
static Frame take(Session& s,uint32_t now=0) {uint8_t f[kValue];const size_t n=s.peek(f);if(!n)return {};assert(s.accept(now));return Frame(f,f+n);}
static Op opOf(const Frame& f) {return Op(f[1]);}
static uint16_t idOf(const Frame& f) {return get16(f.data()+2);}
static uint16_t reasonOf(const Frame& f) {return get16(f.data()+kHeader);}
static Frame bodyOf(const Frame& f) {return Frame(f.begin()+kHeader,f.end());}
static void ready(Session& s,Peer& p,uint16_t mtu=517,uint8_t phoneFlags=kPhoneRelayOn|kPhoneConsoleWanted,
                  uint16_t overhead=0,uint32_t at=0) {
 uint8_t nonce[8]={1,2,3,4,5,6,7,8};assert(s.start(mtu,nonce,at,overhead));p.value=uint16_t(valueLimit(mtu)-overhead);p.seq=0;
 drain(s,at);std::vector<uint8_t> ack(20);std::memcpy(ack.data(),nonce,8);ack[8]=9;ack[16]=kMaxStreams;ack[17]=phoneFlags;
 put16(ack.data()+18,p.value);assert(p.send(s,Op::HelloAck,0,ack,at+1));assert(s.state==State::Ready && s.peerFlags()==phoneFlags);
}
static uint16_t opened(Session& s,Peer& p,uint32_t at=1) {
 uint8_t host[4]={192,0,2,1};auto id=s.open(1,host,4,443,at);assert(id);drain(s,at);
 assert(p.send(s,Op::OpenOk,id,u32(1024),at));return id;
}
// The console OPEN is exactly limit 4096 | port 1 | type 4 | no host.
static uint16_t consoleOpened(Session& s,Peer& p,uint32_t now=1) {
 auto id=s.openConsole(now);assert(id && s.find(id)==&s.streams[kConsoleSlot] && s.find(id)->console);
 auto f=take(s,now);assert(opOf(f)==Op::Open && idOf(f)==id && f[0]==kVersion && f[10]==(kFirst|kLast));
 const Frame body={0x00,0x10,0x00,0x00,0x01,0x00,0x04,0x00};assert(bodyOf(f)==body);
 assert(p.send(s,Op::OpenOk,id,u32(4096),now));assert(s.find(id)->state==StreamState::Open);return id;
}
// A saturated console beside one relay upload of `total` bytes, one frame per
// 30 ms (the adapter's pacing); the phone grants consumed+4096 as frames land
// and answers PINGs. Times are from the first write.
struct Upload { uint32_t relayDone=0, firstRelay=0; unsigned relayFrames=0, consoleFrames=0; };
static Upload upload(uint16_t mtu,size_t total,bool relayPressure,bool displayBusy,uint32_t limitMs) {
 Session s;Peer p;ready(s,p,mtu,kPhoneRelayOn|kPhoneConsoleWanted,25);
 const auto a=opened(s,p);const auto c=consoleOpened(s,p);
 const std::vector<uint8_t> chunk(4096,0x55);const uint8_t head[4]={2,1,0,4};
 Upload r;size_t written=0;const uint32_t start=10;
 for(uint32_t t=start;t<start+limitMs;t+=30) {
   if(written<total) written+=s.write(a,chunk.data(),std::min(total-written,chunk.size()));
   while(s.pushRecord(c,head,4,chunk.data(),1024,4)==PushResult::Ok){}
   assert(s.tick(t,relayPressure,displayBusy));
   const Frame f=take(s,t);if(f.empty())continue;
   if(opOf(f)==Op::Ping)assert(p.send(s,Op::Pong,0,bodyOf(f),t));
   if(opOf(f)!=Op::Data)continue;
   auto* st=s.find(idOf(f));assert(p.send(s,Op::Credit,idOf(f),u32(st->sent+4096),t));
   if(idOf(f)==c){++r.consoleFrames;continue;}
   if(!r.relayFrames++)r.firstRelay=t-start;
   if(st->sent==total && !r.relayDone)r.relayDone=t-start;
 }
 return r;
}
int main() {
 for(uint16_t mtu:{23,64,247,517}) {
  Session s;Peer p;ready(s,p,mtu);auto a=opened(s,p),b=opened(s,p);
  uint8_t host[4]={192,0,2,1};assert(!s.open(1,host,4,443,1));
  std::vector<uint8_t> bytes(1024,42);assert(s.write(a,bytes.data(),bytes.size())==1024);
  assert(s.write(b,bytes.data(),bytes.size())==1024);drain(s);
  assert(s.find(a)->sent==1024 && s.find(b)->sent==1024);
  assert(s.write(a,bytes.data(),bytes.size())==1024);uint8_t frame[kValue];assert(!s.peek(frame));
  assert(p.send(s,Op::Credit,a,u32(2048)));drain(s);assert(s.find(a)->sent==2048);
  assert(p.send(s,Op::Data,b,bytes));uint8_t out[1024];assert(s.read(b,out,512)==512);
  assert(s.tick(200,true));assert(!s.peek(frame));assert(s.tick(201));drain(s);
  assert(s.find(b)->receiveLimit==1536);assert(s.read(b,out,512)==512);
  assert(p.send(s,Op::Fin,b,u32(1024)));assert(s.find(b)->eof);assert(!s.read(b,out,1));
  assert(s.close(a,202,true));drain(s);assert(p.send(s,Op::CloseAck,a,{}));s.release(a);
  auto c=opened(s,p);assert(c>b);assert(p.send(s,Op::Data,a,{1}));assert(s.find(c)->received==0);
  s.down();assert(s.find(c)->state==StreamState::Ended);
 }
 {Session s;Peer p;ready(s,p);auto a=opened(s,p);assert(!p.send(s,Op::Credit,a,u32(8192)));assert(s.error==Error::Flow);}
 {Session s;Peer p;ready(s,p);auto a=opened(s,p);std::vector<uint8_t>x(1025,1);assert(!p.send(s,Op::Data,a,x));assert(s.error==Error::Flow);}
 {Session s;Peer p;ready(s,p);p.seq++;assert(!p.send(s,Op::Ping,0,u32(1)));}
 {Session s;Peer p;ready(s,p);auto a=opened(s,p);assert(s.close(a,2));assert(!s.tick(2002));}
 {Session s;Peer p;ready(s,p);opened(s,p);assert(s.tick(5001));drain(s);assert(!s.tick(10001));}
 {Session s;Peer p;ready(s,p);auto a=opened(s,p);assert(p.send(s,Op::Fin,a,u32(0)));assert(!p.send(s,Op::Data,a,{1}));}
 // A cancelled OPEN may retire on OPEN_ERR without waiting for a second
 // CLOSE_ACK. Peer CLOSE retires only after our ACK is actually submitted.
 {Session s;Peer p;ready(s,p);uint8_t host[4]={192,0,2,1};auto id=s.open(1,host,4,443,1);drain(s);
  assert(s.close(id,2));assert(p.send(s,Op::OpenErr,id,{uint8_t(Error::Connect),0}));
  assert(!s.find(id)->live());s.release(id);assert(!s.find(id));}
 {Session s;Peer p;ready(s,p);auto id=opened(s,p);assert(p.send(s,Op::Close,id,{0,0}));
  s.release(id);assert(s.find(id)&&s.find(id)->live());drain(s);
  assert(!s.find(id)->live());s.release(id);assert(!s.find(id));}
 // Duplicate peer CLOSE cannot extend the existing retirement deadline.
 {Session s;Peer p;ready(s,p);auto id=opened(s,p);assert(p.send(s,Op::Close,id,{0,0},10));
  assert(p.send(s,Op::Close,id,{0,0},1500));assert(!s.tick(2010));}
 // Byte mutations never escape bounded storage, including invalid lengths/opcodes.
 for(unsigned offset=0;offset<24;++offset)for(unsigned v=0;v<256;++v) {
  Session s;Peer p;ready(s,p);opened(s,p);uint8_t f[24]{};
  f[0]=kVersion;f[1]=uint8_t(Op::Data);put16(f+2,1);put32(f+4,p.seq);put16(f+8,12);f[offset]=uint8_t(v);
  (void)s.receive(f,sizeof(f),3);
 }
 // Receive grants become effective only when the CREDIT value is submitted.
 {Session s;Peer p;ready(s,p);auto a=opened(s,p);
  assert(p.send(s,Op::Data,a,std::vector<uint8_t>(1024,1)));uint8_t out[1024];
  assert(s.read(a,out,1024)==1024);assert(s.tick(200));
  assert(s.find(a)->receiveLimit==1024 && s.find(a)->queuedLimit==2048);
  for(unsigned i=0;i<30;++i)assert(s.tick(201+i));
  drain(s);assert(s.find(a)->receiveLimit==2048);}
 // Cancellation of a locally prepared but unsubmitted frame consumes no sequence.
 {Session s;Peer p;ready(s,p);auto a=opened(s,p);uint8_t bytes[32]{};
  assert(s.write(a,bytes,32)==32);uint8_t frame[kValue];assert(s.peek(frame));
  auto sequence=s.txSequence;assert(s.close(a,2,true));assert(s.peek(frame));
  assert(frame[1]==uint8_t(Op::Close) && get32(frame+4)==sequence);assert(s.accept(0));}
 // A response cannot precede submission of its OPEN message.
 {Session s;Peer p;ready(s,p);uint8_t host[4]={192,0,2,1};auto a=s.open(1,host,4,443,1);
  assert(a);assert(!p.send(s,Op::OpenOk,a,u32(1024)));}
 // Combined display/mic pressure spends at most 2000 ms per stream, even
 // across quiet gaps, repeated DATA/CREDIT and uint32_t time wraparound.
 for(uint32_t start:{uint32_t(100),UINT32_MAX-1500}) {
  // Set up just before `start` (the link PING would otherwise see a P4 silent since 0).
  Session s;Peer p;ready(s,p,517,kPhoneRelayOn|kPhoneConsoleWanted,0,start-20);
  auto a=opened(s,p,start-10);auto b=opened(s,p,start-10);
  uint8_t frame[kValue],out[16],bytes[16]{};
  assert(p.send(s,Op::Data,a,std::vector<uint8_t>(16,1),start));assert(s.read(a,out,16)==16);
  assert(s.write(a,bytes,16)==16);assert(s.write(b,bytes,16)==16);
  assert(s.tick(start,true));assert(!s.peek(frame));
  assert(s.tick(start+1000,false));assert(s.find(a)->pressureMs==1000);
  // Resuming pressure must not reset the budget. A previously queued CREDIT
  // and DATA both wait; unrelated control bypasses them.
  assert(s.tick(start+1100,true));assert(!s.peek(frame));
  assert(p.send(s,Op::Ping,0,u32(123),start+1100));
  assert(s.peek(frame)&&frame[1]==uint8_t(Op::Pong));assert(s.accept(0));
  assert(s.tick(start+2099,true));assert(!s.peek(frame));
  assert(s.tick(start+2100,true));assert(s.find(a)->pressureMs==2000);
  assert(s.peek(frame)&&frame[1]==uint8_t(Op::Credit));assert(s.accept(0));
  assert(s.find(a)->receiveLimit==1040);
  assert(s.peek(frame)&&frame[1]==uint8_t(Op::Data));auto first=get16(frame+2);assert(s.accept(0));
  assert(s.peek(frame)&&frame[1]==uint8_t(Op::Data)&&get16(frame+2)!=first);assert(s.accept(0));
  assert(s.tick(start+2200,false));assert(s.tick(start+2300,true));
  assert(s.write(a,bytes,16)==16);assert(s.peek(frame)&&frame[1]==uint8_t(Op::Data));assert(s.accept(0));
  assert(s.close(b,start+2300,true));assert(s.peek(frame)&&frame[1]==uint8_t(Op::Close));assert(s.accept(0));
 }
 // A-1: the response allowance starts at local acceptance of the LAST OPEN
 // fragment; the phone's OPEN_ERR/RESET is marked as a remote end.
 {Session s;Peer p;ready(s,p,23);const char* name="relay.example.test";  // 8-byte bodies: 4 fragments
  auto id=s.open(3,reinterpret_cast<const uint8_t*>(name),18,8443,1);assert(id);
  uint8_t frame[kValue];
  for(uint32_t t:{70u,80u,90u}){assert(s.peek(frame)&&s.accept(t));assert(!s.find(id)->openSubmitted);}
  assert(s.peek(frame)&&(frame[10]&kLast)&&s.accept(110));
  assert(s.find(id)->openSubmitted&&s.find(id)->openSubmittedAt==110);
  assert(p.send(s,Op::OpenErr,id,{5,0}));assert(s.find(id)->remoteEnd&&s.find(id)->error==Error::Vpn);}
 {Session s;Peer p;ready(s,p);auto id=opened(s,p);assert(!s.find(id)->remoteEnd);
  assert(p.send(s,Op::Reset,id,{11,0}));assert(s.find(id)->remoteEnd);
  auto other=opened(s,p);s.down();assert(!s.find(other)->remoteEnd);}  // session loss is not a phone answer
 {Session s;uint8_t nonce[8]={8,7,6,5,4,3,2,1};assert(s.start(247,nonce,0));assert(!std::memcmp(s.nonce(),nonce,8));}
 // ---- Inner protocol v2 ----
 // Slot classes: relays take slots 0..1 only, the console slot 2 only; ids
 // stay shared and strictly increasing across classes.
 {Session s;Peer p;ready(s,p);uint8_t host[4]={192,0,2,1};
  auto a=opened(s,p),b=opened(s,p);assert(!s.open(1,host,4,443,1));
  assert(s.streams[kConsoleSlot].state==StreamState::Free && s.relayLive());
  auto c=consoleOpened(s,p);assert(c>b && !s.find(a)->console && !s.find(b)->console);
  assert(!s.openConsole(1));
  assert(s.close(a,2,true));drain(s);assert(p.send(s,Op::CloseAck,a,{}));s.release(a);
  assert(s.close(b,2,true));drain(s);assert(p.send(s,Op::CloseAck,b,{}));s.release(b);
  assert(!s.relayLive() && s.find(c)->live());
  auto d=s.open(1,host,4,443,3);assert(d>c && s.find(d)==&s.streams[0]);}
 {Session s;Peer p;ready(s,p);consoleOpened(s,p);uint8_t host[4]={192,0,2,1};
  assert(s.open(1,host,4,443,1) && s.open(1,host,4,443,1) && !s.open(1,host,4,443,1));}
 {Session s;assert(!s.openConsole(0));uint8_t nonce[8]={1};assert(s.start(247,nonce,0) && !s.openConsole(0));}
 {Session s;assert(s.streams[kConsoleSlot].console && !s.streams[0].console && !s.streams[1].console);
  s.streams[kConsoleSlot].reset();assert(s.streams[kConsoleSlot].console);}
 // HELLO: header 2, max_streams 3, the P4 flags (defined bits only).
 for(uint8_t flags:{uint8_t(0),uint8_t(1),uint8_t(0xff)}) {
  Session s;s.setLocalFlags(flags);uint8_t nonce[8]={1,2,3,4,5,6,7,8};assert(s.start(247,nonce,0));
  auto f=take(s);assert(f.size()==24 && f[0]==kVersion && opOf(f)==Op::Hello && f[10]==3 && !s.peerFlags());
  assert(!std::memcmp(f.data()+12,nonce,8) && f[20]==3 && f[21]==(flags&1) && get16(f.data()+22)==244);
 }
 // HELLO_ACK: max_streams must be 3 and only phone flag bits 0..1 may be set.
 for(unsigned flags=0;flags<8;++flags) for(uint8_t streams:{uint8_t(2),uint8_t(3),uint8_t(4)}) {
  Session s;Peer p;uint8_t nonce[8]={1,2,3,4,5,6,7,8};assert(s.start(517,nonce,0));drain(s);
  std::vector<uint8_t> ack(20);std::memcpy(ack.data(),nonce,8);ack[8]=9;ack[16]=streams;ack[17]=uint8_t(flags);
  put16(ack.data()+18,512);const bool ok=streams==3 && flags<4;
  assert(p.send(s,Op::HelloAck,0,ack)==ok);
  assert(ok ? s.state==State::Ready && s.peerFlags()==flags : s.state==State::Failed && s.error==Error::Protocol);
 }
 // LINK_STATE out: never in Hello (HELLO carries the flags), at once for the
 // first change after READY, then one queued per second at most; an unsent one
 // is overwritten in place; a change back to what was queued sends nothing.
 {Session s;Peer p;uint8_t nonce[8]={1,2,3,4,5,6,7,8};assert(s.start(517,nonce,0));
  s.setLocalFlags(kP4ConsoleOffered);assert(s.tick(10) && s.controlQueued()==1);drain(s);  // HELLO (flags 0) only
  std::vector<uint8_t> ack(20);std::memcpy(ack.data(),nonce,8);ack[8]=9;ack[16]=3;ack[17]=3;put16(ack.data()+18,512);
  assert(p.send(s,Op::HelloAck,0,ack,20));
  assert(s.tick(20));auto f=take(s,20);
  assert(opOf(f)==Op::LinkState && !idOf(f) && f[10]==3 && f.size()==16 && bodyOf(f)==Frame({1,0,0,0}));
  s.setLocalFlags(0);assert(s.tick(500) && !s.controlQueued());
  s.setLocalFlags(1);assert(s.tick(600) && !s.controlQueued());
  s.setLocalFlags(0);assert(s.tick(1019) && !s.controlQueued());
  assert(s.tick(1020) && s.controlQueued()==1);                     // TX gate held: stays unsent
  s.setLocalFlags(1);assert(s.tick(1100) && s.controlQueued()==1);  // overwritten in place
  f=take(s,1150);assert(opOf(f)==Op::LinkState && get32(f.data()+kHeader)==1 && !s.controlQueued());
  s.setLocalFlags(0);assert(s.tick(2019) && !s.controlQueued() && s.tick(2020) && s.controlQueued()==1);}
 // LINK_STATE in: a full snapshot of the phone's flags, no answer owed;
 // undefined bits, upper bytes, a stream id, a short body or Hello -> Protocol.
 {Session s;Peer p;ready(s,p);
  assert(p.send(s,Op::LinkState,0,u32(kPhoneConsoleWanted)) && s.peerFlags()==kPhoneConsoleWanted);
  assert(p.send(s,Op::LinkState,0,u32(0)) && !s.peerFlags() && !s.controlQueued());}
 for(uint32_t bad:{4u,0x80u,0x100u,0x01000003u}) {
  Session s;Peer p;ready(s,p);assert(!p.send(s,Op::LinkState,0,u32(bad)) && s.error==Error::Protocol);}
 {Session s;Peer p;ready(s,p);auto a=opened(s,p);assert(!p.send(s,Op::LinkState,a,u32(1)) && s.error==Error::Protocol);}
 {Session s;Peer p;ready(s,p);assert(!p.send(s,Op::LinkState,0,{1,0,0}) && s.error==Error::Protocol);}
 {Session s;Peer p;uint8_t nonce[8]={1,2,3,4,5,6,7,8};assert(s.start(517,nonce,0));drain(s);
  assert(!p.send(s,Op::LinkState,0,u32(1)) && s.error==Error::Protocol);}
 // Errors 16/17/18 end one stream (the session goes on); a stream-0 RESET 18
 // (PAUSED) ends the session as a peer reset; 19 is Protocol everywhere.
 for(uint16_t code=0;code<40;++code)assert(knownError(code)==(code>=1&&code<=18));
 for(uint16_t code:{16,17,18}) {
  Session s;Peer p;ready(s,p);uint8_t host[4]={192,0,2,1};auto id=s.open(1,host,4,443,1);drain(s);
  auto c=consoleOpened(s,p);
  assert(p.send(s,Op::OpenErr,id,{uint8_t(code),0}) && s.state==State::Ready);
  assert(s.find(id)->state==StreamState::Ended && s.find(id)->remoteEnd && s.find(id)->error==Error(code));
  assert(p.send(s,Op::Reset,c,{uint8_t(code),0}) && s.find(c)->state==StreamState::Ended && s.state==State::Ready);
 }
 {Session s;Peer p;ready(s,p);assert(!p.send(s,Op::Reset,0,{18,0}) && s.error==Error::Paused && s.peerReset);
  assert(s.helloEnd()==HelloEnd::None);}
 {Session s;Peer p;ready(s,p);uint8_t host[4]={192,0,2,1};auto id=s.open(1,host,4,443,1);drain(s);
  assert(!p.send(s,Op::OpenErr,id,{19,0}) && s.error==Error::Protocol);}
 {Session s;Peer p;ready(s,p);auto id=opened(s,p);assert(!p.send(s,Op::Reset,id,{19,0}) && s.error==Error::Protocol);}
 {Session s;Peer p;ready(s,p);assert(!p.send(s,Op::Reset,0,{19,0}) && s.error==Error::Protocol && !s.peerReset);}
 // CLOSE reasons, checked by class once the stream is found: relay 0..1,
 // console 0..5, an id no slot holds 0..5. FIN on the console is Protocol.
 for(uint16_t reason=0;reason<8;++reason) {
  {Session s;Peer p;ready(s,p);auto a=opened(s,p);const bool ok=reason<=1;
   assert(p.send(s,Op::Close,a,{uint8_t(reason),0})==ok);
   if(ok){assert(s.find(a)->peerCloseReason==reason);auto f=take(s);assert(opOf(f)==Op::CloseAck && idOf(f)==a);}
   else assert(s.error==Error::Protocol);}
  {Session s;Peer p;ready(s,p);auto c=consoleOpened(s,p);const bool ok=reason<=5;
   assert(p.send(s,Op::Close,c,{uint8_t(reason),0})==ok);
   if(ok){assert(s.find(c)->peerCloseReason==reason && s.find(c)->state==StreamState::Closing);
     auto f=take(s);assert(opOf(f)==Op::CloseAck && idOf(f)==c && s.find(c)->state==StreamState::Ended);}
   else assert(s.error==Error::Protocol);}
  for(bool released:{false,true}) {   // ended (still in its slot) or retired: 0..5
   Session s;Peer p;ready(s,p);auto a=opened(s,p);assert(s.close(a,2));drain(s);assert(p.send(s,Op::CloseAck,a,{}));
   if(released)s.release(a);
   const bool ok=reason<=5;assert(p.send(s,Op::Close,a,{uint8_t(reason),0})==ok);
   if(ok){auto f=take(s);assert(opOf(f)==Op::CloseAck && idOf(f)==a);} else assert(s.error==Error::Protocol);}
 }
 {Session s;Peer p;ready(s,p);auto c=consoleOpened(s,p);assert(!p.send(s,Op::Fin,c,u32(0)) && s.error==Error::Protocol);}
 // The P4 sends only its class's reasons: console 1..5 (close() means 1), relay 0..1.
 {Session s;Peer p;ready(s,p);auto c=consoleOpened(s,p);assert(s.close(c,2,false));auto f=take(s);
  assert(opOf(f)==Op::Close && reasonOf(f)==1 && s.find(c)->closeReason==1);}
 for(uint16_t reason:{0,1,2,3,4,5,6,99}) {
  Session s;Peer p;ready(s,p);auto c=consoleOpened(s,p);auto a=opened(s,p);
  assert(s.closeAfterDrain(c,2,reason) && s.closeAfterDrain(a,2,reason));
  auto f=take(s);assert(opOf(f)==Op::Close && idOf(f)==c && reasonOf(f)==(reason>=1&&reason<=5?reason:1));
  f=take(s);assert(opOf(f)==Op::Close && idOf(f)==a && reasonOf(f)==(reason<=1?reason:1));
 }
 // closeAfterDrain: what is already in tx goes out first and CLOSE follows the
 // last DATA accept; writes are refused meanwhile, inbound DATA is still taken
 // (and wiped by the close); the first reason stands.
 {Session s;Peer p;ready(s,p);auto c=consoleOpened(s,p);
  std::vector<uint8_t> rec(1200,7);assert(s.pushRecord(c,rec.data(),4,rec.data()+4,1196,4)==PushResult::Ok);
  assert(s.closeAfterDrain(c,10,uint16_t(ConsoleClose::ConsoleOff)));
  assert(s.find(c)->draining && s.find(c)->state==StreamState::Open && !s.controlQueued());
  assert(!s.write(c,rec.data(),1) && !s.txSpace(c) && s.pushRecord(c,rec.data(),4,nullptr,0,0)==PushResult::Gone);
  assert(s.closeAfterDrain(c,11,uint16_t(ConsoleClose::Renewed)) && s.find(c)->drainReason==3);
  assert(p.send(s,Op::Data,c,std::vector<uint8_t>(100,1)) && s.rxAvailable(c)==100);
  size_t data=0;Frame f;
  for(uint32_t t=20;;t+=30) {assert(s.tick(t));f=take(s,t);assert(!f.empty());if(opOf(f)!=Op::Data)break;data+=f.size()-kHeader;}
  assert(data==1200 && opOf(f)==Op::Close && reasonOf(f)==3 && s.find(c)->state==StreamState::Closing);
  assert(SessionProbe::wiped(s) && !s.find(c)->rx.size);}
 // The 3 s cap: display pressure holds the console's DATA, the CLOSE (control)
 // goes anyway and the unsent tail is wiped ("not confirmed").
 {Session s;Peer p;ready(s,p);auto c=consoleOpened(s,p);std::vector<uint8_t> rec(600,9);
  assert(s.pushRecord(c,rec.data(),4,rec.data()+4,596,4)==PushResult::Ok);
  assert(s.tick(100,false,true) && s.closeAfterDrain(c,100,2));
  for(uint32_t t=130;t<3100;t+=30){assert(s.tick(t,false,true) && take(s,t).empty());}
  assert(s.tick(3100,false,true));auto f=take(s,3100);assert(opOf(f)==Op::Close && reasonOf(f)==2);
  assert(!s.find(c)->sent && SessionProbe::wiped(s));}
 {Session s;Peer p;ready(s,p);auto c=s.openConsole(1);assert(s.closeAfterDrain(c,2,4));
  auto f=take(s);assert(opOf(f)==Op::Open);f=take(s);assert(opOf(f)==Op::Close && idOf(f)==c && reasonOf(f)==4);}
 {Session s;Peer p;ready(s,p);auto a=opened(s,p);assert(s.closeAfterDrain(a,2,0));auto f=take(s);
  assert(opOf(f)==Op::Close && reasonOf(f)==0 && s.closeAfterDrain(a,3,1) && !s.controlQueued());}
 // Ingress credit: slot 2 grants consumed+4096, relays consumed+1024; nothing
 // is granted until consume() (which wipes); one byte past the window is Flow.
 {Session s;Peer p;ready(s,p);auto c=consoleOpened(s,p);
  std::vector<uint8_t> in(4096);for(size_t i=0;i<in.size();++i)in[i]=uint8_t(i*7+1);
  for(size_t off=0;off<in.size();off+=500)
   assert(p.send(s,Op::Data,c,std::vector<uint8_t>(in.begin()+off,in.begin()+std::min(in.size(),off+500))));
  assert(s.rxAvailable(c)==4096 && !s.find(c)->rx.space());
  uint8_t peek[8];assert(s.rxPeek(c,peek,8,4090)==6 && peek[0]==in[4090] && peek[5]==in[4095]);
  assert(!s.rxPeek(c,peek,8,4096) && !s.rxPeek(c,nullptr,8) && !s.rxAvailable(c+1));
  for(uint32_t t=200;t<1200;t+=100)assert(s.tick(t) && !s.controlQueued());
  assert(!s.consume(c,4097) && s.consume(c,1000) && s.rxAvailable(c)==3096 && s.find(c)->consumed==1000);
  assert(SessionProbe::wiped(s) && s.rxPeek(c,peek,1)==1 && peek[0]==in[1000]);
  assert(s.tick(1300));auto f=take(s,1300);
  assert(opOf(f)==Op::Credit && idOf(f)==c && get32(f.data()+kHeader)==5096 && s.find(c)->receiveLimit==5096);
  assert(p.send(s,Op::Data,c,std::vector<uint8_t>(1000,1)));
  assert(!p.send(s,Op::Data,c,{1}) && s.error==Error::Flow);}
 {Session s;Peer p;ready(s,p);auto c=consoleOpened(s,p);
  assert(!p.send(s,Op::Data,c,std::vector<uint8_t>(4097,1)) && s.error==Error::Flow);}
 {Session s;Peer p;ready(s,p);auto a=opened(s,p);assert(p.send(s,Op::Data,a,std::vector<uint8_t>(600,1)));
  assert(s.consume(a,600) && SessionProbe::wiped(s) && s.tick(200));
  auto f=take(s,200);assert(opOf(f)==Op::Credit && get32(f.data()+kHeader)==1624);}
 {Session s;Peer p;ready(s,p);auto c=s.openConsole(1);assert(!s.consume(c,0) && !s.rxAvailable(c));}  // Opening
 // Egress: whole records or nothing, with room kept for the writer's ABORT.
 {Session s;Peer p;ready(s,p);auto c=s.openConsole(1);
  assert(!s.txSpace(c) && s.pushRecord(c,nullptr,0,nullptr,0,0)==PushResult::Gone);
  drain(s);assert(p.send(s,Op::OpenOk,c,u32(4096)));
  std::vector<uint8_t> big(4096,5);const uint8_t head[4]={2,1,0,0},abort[4]={6,0,0,0};
  assert(s.txSpace(c)==4096 && !s.txSpace(0) && !s.txSpace(999));
  assert(s.pushRecord(c,head,4,big.data(),4089,4)==PushResult::NoSpace && !s.find(c)->tx.size);
  assert(s.pushRecord(c,head,4,big.data(),4088,4)==PushResult::Ok && s.txSpace(c)==4);
  assert(s.pushRecord(c,head,4,big.data(),1,0)==PushResult::NoSpace);
  assert(s.pushRecord(c,abort,4,nullptr,0,0)==PushResult::Ok && !s.txSpace(c));
  assert(s.pushRecord(c,abort,4,nullptr,0,0)==PushResult::NoSpace);
  assert(s.pushRecord(c+1,abort,4,nullptr,0,0)==PushResult::Gone && s.pushRecord(c,nullptr,4,nullptr,0,0)==PushResult::Gone);
  s.down();assert(s.pushRecord(c,abort,4,nullptr,0,0)==PushResult::Gone && !s.txSpace(c));}
 // The control queue: the worst legal case is exactly 12 with the TX gate held
 // (per slot CLOSE(old), OPEN(new), CLOSE(new); then PING, PONG, LINK_STATE).
 // Coalescing keeps it at 12; the 13th distinct message fails with Resources.
 // Released, it drains in FIFO order: OPEN is never dropped or overtaken.
 for(bool overflow:{false,true}) {
  Session s;Peer p;ready(s,p);uint8_t host[4]={192,0,2,1};uint16_t old[3],fresh[3];
  old[0]=opened(s,p);old[1]=opened(s,p);old[2]=consoleOpened(s,p);
  for(unsigned i=0;i<3;++i) {
   assert(s.close(old[i],100,true) && p.send(s,Op::Reset,old[i],{14,0},100));s.release(old[i]);
   fresh[i]=i<2?s.open(1,host,4,443,4000):s.openConsole(4000);
   assert(fresh[i] && s.find(fresh[i])==&s.streams[i] && s.close(fresh[i],4000,true));
  }
  assert(s.controlQueued()==9);
  assert(s.tick(5100) && s.controlQueued()==10);                          // relay PING
  assert(p.send(s,Op::Ping,0,u32(7),5100) && s.controlQueued()==11);      // PONG
  s.setLocalFlags(kP4ConsoleOffered);assert(s.tick(5101) && s.controlQueued()==12);  // LINK_STATE
  assert(p.send(s,Op::Ping,0,u32(8),5102) && s.controlQueued()==12);      // PONG, newest cookie
  s.setLocalFlags(0);assert(s.tick(5103) && s.controlQueued()==12);
  s.setLocalFlags(kP4ConsoleOffered);assert(s.tick(5104) && s.controlQueued()==12);
  if(overflow) {assert(!p.send(s,Op::Close,old[0],{0,0},5105) && s.error==Error::Resources);continue;}
  const Op order[12]={Op::Close,Op::Open,Op::Close,Op::Close,Op::Open,Op::Close,Op::Close,Op::Open,Op::Close,
                      Op::Ping,Op::Pong,Op::LinkState};
  const uint16_t ids[12]={old[0],fresh[0],fresh[0],old[1],fresh[1],fresh[1],old[2],fresh[2],fresh[2],0,0,0};
  for(unsigned i=0;i<12;++i) {
   auto f=take(s,5200);assert(opOf(f)==order[i] && idOf(f)==ids[i]);
   if(order[i]==Op::Pong)assert(get32(f.data()+kHeader)==8);
   if(order[i]==Op::LinkState)assert(get32(f.data()+kHeader)==kP4ConsoleOffered);
  }
  assert(!s.controlQueued() && s.state==State::Ready);
 }
 // Twelve CLOSE_ACKs for a retired id fit; the thirteenth is Resources.
 {Session s;Peer p;ready(s,p);auto a=opened(s,p);assert(s.close(a,2));drain(s);assert(p.send(s,Op::CloseAck,a,{}));s.release(a);
  for(unsigned i=0;i<12;++i)assert(p.send(s,Op::Close,a,{0,0}));
  assert(s.controlQueued()==12 && !p.send(s,Op::Close,a,{0,0}) && s.error==Error::Resources);}
 // F-3: closing or ending a stream purges only its unstarted CREDITs, so a
 // deferred CREDIT cannot add a fourth message per slot.
 {Session s;Peer p;ready(s,p);uint8_t host[4]={192,0,2,1};auto a=opened(s,p);
  assert(p.send(s,Op::Data,a,std::vector<uint8_t>(600,1),10));uint8_t buf[600];assert(s.read(a,buf,600)==600);
  assert(s.tick(20) && s.controlQueued()==1 && s.tick(30,true) && s.find(a)->deferred());
  uint8_t frame[kValue];assert(!s.peek(frame));
  assert(s.close(a,40,true) && s.controlQueued()==1);
  assert(p.send(s,Op::Reset,a,{14,0},50));s.release(a);
  auto b=s.open(1,host,4,443,60);assert(b && s.close(b,60,true) && s.controlQueued()==3);
  for(Op op:{Op::Close,Op::Open,Op::Close}){auto f=take(s,70);assert(opOf(f)==op);}
  assert(!s.controlQueued());}
 {Session s;Peer p;ready(s,p);auto a=opened(s,p);assert(p.send(s,Op::Data,a,std::vector<uint8_t>(600,1),10));
  uint8_t buf[600];assert(s.read(a,buf,600)==600 && s.tick(20) && s.controlQueued()==1);
  assert(p.send(s,Op::Reset,a,{14,0},21) && !s.controlQueued());}             // end() purges too
 {Session s;Peer p;ready(s,p);auto a=opened(s,p);assert(p.send(s,Op::Data,a,std::vector<uint8_t>(600,1),10));
  uint8_t buf[600];assert(s.read(a,buf,600)==600 && s.tick(20));
  uint8_t frame[kValue];assert(s.peek(frame) && frame[1]==uint8_t(Op::Credit));   // started: pending submission
  assert(s.close(a,30,true) && s.controlQueued()==2 && s.accept(31));
  auto f=take(s,32);assert(opOf(f)==Op::Close && idOf(f)==a);}
 // Scheduling, counted at accept(): relays round-robin, the console every
 // fourth DATA frame while it has data (R R R C); a repeated peek is no turn.
 {Session s;Peer p;ready(s,p);auto a=opened(s,p),b=opened(s,p);auto c=consoleOpened(s,p);
  assert(p.send(s,Op::Credit,a,u32(4096)) && p.send(s,Op::Credit,b,u32(4096)));
  std::vector<uint8_t> bytes(4096,3);for(auto id:{a,b,c})assert(s.write(id,bytes.data(),bytes.size())==4096);
  std::vector<uint16_t> order;uint8_t frame[kValue];
  for(unsigned i=0;i<16;++i){assert(s.peek(frame) && s.peek(frame) && s.accept(10+i));order.push_back(get16(frame+2));}
  for(unsigned i=0;i<16;++i)assert((order[i]==c)==(i%4==3));
  for(unsigned i=0;i+2<16;++i)if(order[i]!=c){const unsigned j=order[i+1]==c?i+2:i+1;assert(order[j]!=order[i]);}}
 // Only the console has data: it takes every frame.
 {Session s;Peer p;ready(s,p);opened(s,p);auto c=consoleOpened(s,p);std::vector<uint8_t> bytes(2000,3);
  assert(s.write(c,bytes.data(),bytes.size())==2000);for(unsigned i=0;i<4;++i){auto f=take(s);assert(idOf(f)==c);}}
 // Chat during a saturated console: at MTU 244 (204 B bodies, one frame per
 // 30 ms) a 14,450 B relay upload completes within 5 s, also when relay
 // pressure first spends its whole 2 s budget.
 for(bool pressure:{false,true}) {
  const auto r=upload(244,14450,pressure,false,10000);
  assert(r.relayDone && r.relayDone<=5000 && r.relayFrames==71 && r.consoleFrames>=23);
  assert(pressure ? r.firstRelay>=2000 : r.firstRelay==0);
  std::printf("chat upload during console BLOB, relay pressure %s: %u ms (%u relay, %u console frames)\n",
              pressure?"on":"off",unsigned(r.relayDone),r.relayFrames,r.consoleFrames);
 }
 // A minute of display updates: the console sends nothing (no budget) while
 // the relay waits out its 2 s budget and then flows.
 {const auto r=upload(244,1u<<20,true,true,60000);
  assert(!r.consoleFrames && r.relayFrames>1500 && r.firstRelay>=2000 && r.firstRelay<2100);}
 // PING split: a console alone sees no PING at 5 s, one after 60 s of silence
 // and Deadline 5 s later; answered, the next waits another 60 s of P4 silence.
 {Session s;Peer p;ready(s,p);consoleOpened(s,p,1);
  assert(s.tick(5001) && !s.controlQueued() && s.tick(60000) && !s.controlQueued());
  assert(s.tick(60001) && s.controlQueued()==1);auto f=take(s,60001);
  assert(opOf(f)==Op::Ping && get32(f.data()+kHeader)==60001);
  assert(s.tick(65000) && !s.tick(65001) && s.error==Error::Deadline);}
 {Session s;Peer p;ready(s,p);consoleOpened(s,p,1);assert(s.tick(60001));auto f=take(s,60001);
  assert(p.send(s,Op::Pong,0,bodyOf(f),60100));
  assert(s.tick(70000) && !s.controlQueued() && s.tick(120000) && !s.controlQueued());
  assert(s.tick(120001) && s.controlQueued()==1);}
 // TX idle alone triggers it (the phone's watchdog runs on what it receives)...
 {Session s;Peer p;ready(s,p);
  for(uint32_t t=10000;t<60000;t+=10000)assert(p.send(s,Op::LinkState,0,u32(3),t) && s.tick(t) && !s.controlQueued());
  assert(s.tick(59999) && !s.controlQueued() && s.tick(60000) && s.controlQueued()==1);}
 // ...and so does RX idle while the P4 keeps sending.
 {Session s;Peer p;ready(s,p);auto c=consoleOpened(s,p,1);const uint8_t head[4]={2,0,1,0},x='x';
  for(uint32_t t=10000;t<=60000;t+=10000){assert(s.pushRecord(c,head,4,&x,1,4)==PushResult::Ok && s.tick(t));assert(opOf(take(s,t))==Op::Data);}
  assert(s.tick(60001) && opOf(take(s,60001))==Op::Ping);}
 // A live relay keeps the 5 s RX-silence PING.
 {Session s;Peer p;ready(s,p);opened(s,p);assert(s.tick(5000) && !s.controlQueued() && s.tick(5001) && s.controlQueued()==1);}
 // Wiping: ring bytes outside the live data, packet_ whenever no frame is
 // pending, assembly_ on down(); after consume, DATA accept, cancel, close,
 // end, down and release.
 {Session s;Peer p;ready(s,p);auto a=opened(s,p);auto b=opened(s,p);auto c=consoleOpened(s,p);
  const std::vector<uint8_t> secret(300,0xA5);
  assert(p.send(s,Op::Data,c,secret) && p.send(s,Op::Data,a,secret) && p.send(s,Op::Data,b,secret));
  assert(s.write(a,secret.data(),300)==300 && s.write(b,secret.data(),300)==300);
  assert(s.pushRecord(c,secret.data(),4,secret.data(),296,4)==PushResult::Ok && SessionProbe::wiped(s));
  assert(s.consume(c,100) && SessionProbe::wiped(s));
  auto f=take(s,2);assert(opOf(f)==Op::Data && SessionProbe::packetZero(s));
  uint8_t frame[kValue];assert(s.peek(frame) && frame[1]==uint8_t(Op::Data) && !SessionProbe::packetZero(s));
  const uint16_t pendingId=get16(frame+2);assert(s.close(pendingId,3,true));
  assert(SessionProbe::packetZero(s) && SessionProbe::wiped(s) && !s.find(pendingId)->tx.size && !s.find(pendingId)->rx.size);
  const uint16_t other=pendingId==a?b:a;assert(p.send(s,Op::Reset,other,{11,0}) && SessionProbe::wiped(s));
  assert(!s.find(other)->tx.size && !s.find(other)->rx.size);
  assert(p.send(s,Op::Ping,0,u32(0xA5A5A5A5)) && !SessionProbe::assemblyZero(s));
  assert(s.peek(frame) && !SessionProbe::packetZero(s));
  s.down();assert(SessionProbe::packetZero(s) && SessionProbe::assemblyZero(s) && SessionProbe::wiped(s));
  for(auto& st:s.streams){for(auto byte:st.tx.bytes)assert(!byte);for(auto byte:st.rx.bytes)assert(!byte);}
  s.release(c);assert(SessionProbe::wiped(s));}
 {Session s;Peer p;ready(s,p);auto a=opened(s,p);std::vector<uint8_t> bytes(500,0x5A);
  assert(p.send(s,Op::Data,a,bytes));uint8_t out[200];assert(s.read(a,out,200)==200 && SessionProbe::wiped(s));
  assert(s.tick(10,true) && s.write(a,bytes.data(),100)==100);   // relay pressure: pending DATA is dropped...
  uint8_t frame[kValue];assert(s.tick(11) && s.peek(frame) && !SessionProbe::packetZero(s));
  assert(s.tick(12,true) && !s.peek(frame) && SessionProbe::packetZero(s));}   // ...and wiped
 // Version signal: exactly a stream-0 RESET 2 as one frame (seq = rxSequence)
 // before HELLO_ACK, under a v1 or v2 header: Version, a peer reset, and the
 // VersionSignal latch with the time since our HELLO was accepted. The latch
 // survives the adapter's later down() calls and is reset only by start().
 {const Frame v1={0x01,0x25,0x00,0x00,0x00,0x00,0x00,0x00,0x02,0x00,0x03,0x00,0x02,0x00};
  for(uint8_t version:{uint8_t(1),kVersion}) {
   Session s;uint8_t nonce[8]={1,2,3,4,5,6,7,8};assert(s.start(247,nonce,0) && opOf(take(s,100))==Op::Hello);
   Frame f=v1;f[0]=version;assert(!s.receive(f.data(),f.size(),350));
   assert(s.error==Error::Version && s.peerReset && s.helloEnd()==HelloEnd::VersionSignal && s.helloEndAfterMs()==250);
   s.down(Error::Version,400);s.down();assert(s.helloEnd()==HelloEnd::VersionSignal && s.helloEndAfterMs()==250);
   assert(s.start(247,nonce,500) && s.helloEnd()==HelloEnd::None && !s.helloEndAfterMs());
  }
  // The same bytes after READY are a protocol error, not a signal.
  {Session s;Peer p;ready(s,p);Frame f=v1;put32(f.data()+4,s.rxSequence);
   assert(!s.receive(f.data(),f.size(),2) && s.error==Error::Protocol && !s.peerReset && s.helloEnd()==HelloEnd::None);}
  // Near misses in Hello: another code, sequence, flag, length or header.
  for(int miss=0;miss<6;++miss) {
   Session s;uint8_t nonce[8]={1,2,3,4,5,6,7,8};assert(s.start(247,nonce,0) && take(s,100).size());
   Frame f=v1;
   if(miss==0)f[12]=14;
   if(miss==1)f[4]=1;
   if(miss==2)f[10]=1;
   if(miss==3)f[11]=1;
   if(miss==4){f.push_back(0);f[8]=3;}
   if(miss==5)f[0]=3;
   assert(!s.receive(f.data(),f.size(),200) && s.error==Error::Protocol && !s.peerReset && s.helloEnd()==HelloEnd::None);
  }
  // A v2 stream-0 RESET with another code in Hello is an ordinary peer reset.
  {Session s;Peer p;uint8_t nonce[8]={1,2,3,4,5,6,7,8};assert(s.start(247,nonce,0) && take(s,100).size());
   assert(!p.send(s,Op::Reset,0,{14,0},200) && s.error==Error::Cancelled && s.peerReset && s.helloEnd()==HelloEnd::None);}
  // Before our HELLO was accepted there is nothing to strike.
  {Session s;uint8_t nonce[8]={1,2,3,4,5,6,7,8};assert(s.start(247,nonce,0));
   assert(!s.receive(v1.data(),v1.size(),5) && s.error==Error::Version && s.peerReset && s.helloEnd()==HelloEnd::None);}
  // A v1 HELLO_ACK is a protocol error.
  {Session s;Peer p;uint8_t nonce[8]={1,2,3,4,5,6,7,8};assert(s.start(517,nonce,0));drain(s);
   std::vector<uint8_t> ack(20);std::memcpy(ack.data(),nonce,8);ack[8]=9;ack[16]=2;put16(ack.data()+18,512);
   assert(!p.raw(s,1,Op::HelloAck,0,ack,3) && s.error==Error::Protocol && s.helloEnd()==HelloEnd::None);}
 }
 // Silent HELLO: a link loss (Connect, with the time) within 1.5 s of HELLO
 // acceptance, or the 3 s HELLO deadline, with nothing received. A local
 // deadline, an untimed down(), a begun answer or a READY session never count.
 for(uint32_t at:{101u,1599u,1600u}) {
  Session s;uint8_t nonce[8]={1,2,3,4,5,6,7,8};assert(s.start(247,nonce,0) && take(s,100).size());
  s.down(Error::Connect,at);assert(s.error==Error::Connect);
  assert(s.helloEnd()==(at<1600?HelloEnd::Silent:HelloEnd::None) && s.helloEndAfterMs()==(at<1600?at-100:0));
  s.down(Error::Connect,at+1);assert(s.helloEnd()==(at<1600?HelloEnd::Silent:HelloEnd::None));
 }
 {Session s;uint8_t nonce[8]={1,2,3,4,5,6,7,8};assert(s.start(247,nonce,0) && take(s,100).size());
  assert(s.tick(2999) && !s.tick(3000) && s.error==Error::Deadline);
  assert(s.helloEnd()==HelloEnd::Silent && s.helloEndAfterMs()==2900);}
 {Session s;uint8_t nonce[8]={1,2,3,4,5,6,7,8};assert(s.start(247,nonce,0) && take(s,100).size());
  s.down(Error::Deadline,200);assert(s.helloEnd()==HelloEnd::None);}
 {Session s;uint8_t nonce[8]={1,2,3,4,5,6,7,8};assert(s.start(247,nonce,0) && take(s,100).size());
  s.down();assert(s.helloEnd()==HelloEnd::None);}
 {Session s;uint8_t nonce[8]={1,2,3,4,5,6,7,8};assert(s.start(247,nonce,0));
  s.down(Error::Connect,50);assert(s.helloEnd()==HelloEnd::None);}               // HELLO never went out
 {Session s;Peer p;uint8_t nonce[8]={1,2,3,4,5,6,7,8};assert(s.start(23,nonce,0));p.value=20;drain(s);
  std::vector<uint8_t> ack(8,1);assert(p.raw(s,kVersion,Op::HelloAck,0,ack,kFirst,50));   // an answer has begun
  s.down(Error::Connect,60);assert(s.helloEnd()==HelloEnd::None);}
 {Session s;Peer p;ready(s,p);s.down(Error::Connect,10);assert(s.helloEnd()==HelloEnd::None);}
 std::printf("host sizeof(Session)=%zu Stream=%zu Message=%zu Buffer=%zu\n",
             sizeof(Session),sizeof(Stream),sizeof(Message),sizeof(Buffer));
 using namespace hw1httptransport;
 for(Path first:{Path::Wifi,Path::Phone})for(unsigned phase=0;phase<8;++phase) {
  const Path other=first==Path::Wifi?Path::Phone:Path::Wifi;
  Attempt a;assert(a.select(first)&&a.first==first);if(phase>=3)a.beforeHttpWrite();
  assert(a.fallback(Failure::Network,other,true)==(phase<3));
  assert(!a.fallback(Failure::Network,other,false));assert(!a.fallback(Failure::Network,first,true));
  assert(!a.fallback(Failure::Trust,other,true));assert(!a.fallback(Failure::Cancelled,other,true));
  assert(!a.fallback(Failure::Protocol,other,true));assert(!a.fallback(Failure::Resources,other,true));
  if(phase<3) {
   assert(a.select(other)&&a.first==first&&a.tried(first)&&a.tried(other));
   // Each path at most once: never back to the first path.
   assert(!a.fallback(Failure::Network,first,true));
  } else assert(!a.select(other));
  a.finish();assert(!a.select(first)&&!a.fallback(Failure::Network,other,true));
 }
 for(uint16_t code=0;code<32;++code) {
  const Failure f=openRefusal(code);
  // 16 SERVICE_OFF (relay off or no destination) is Network: Wi-Fi gets its
  // one try. 17 UNSUPPORTED and 18 PAUSED stay Protocol.
  assert((f==Failure::Network)==(code==5||code==6||code==7||code==8||code==11||code==uint16_t(Error::ServiceOff)));
  assert((f==Failure::Resources)==(code==9||code==10));assert((f==Failure::Cancelled)==(code==14));
 }
 assert(openRefusal(uint16_t(Error::Unsupported))==Failure::Protocol && openRefusal(uint16_t(Error::Paused))==Failure::Protocol);
 Health h;h.context(1,1);assert(h.eligible());h.networkFailed(UINT32_MAX-10000);
 assert(!h.claimProbe(10000));assert(h.claimProbe(20000));assert(!h.claimProbe(20001));
 h.endProbe(false,20000);h.context(2,1);assert(h.eligible());
 std::puts("phone pipe v2 protocol/flow/lifetime, slot classes, LINK_STATE, control queue bound, scheduling, PING split, wiping, version signal and no-replay policy passed");
}
