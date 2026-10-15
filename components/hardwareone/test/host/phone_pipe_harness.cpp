#include "BLE_PhonePipeCore.h"
#include "System_LLMHttpTransportCore.h"
#include <cassert>
#include <vector>
#include <cstdio>
using namespace hw1pipe;
struct Peer {
 uint32_t seq=0; uint16_t value=512;
 bool send(Session& s,Op op,uint16_t id,const std::vector<uint8_t>& body,uint32_t now=1) {
   size_t offset=0; bool result=true;
   do {
     size_t n=body.size()-offset;if(n>value-kHeader)n=value-kHeader;
     uint8_t p[kValue]{};p[0]=1;p[1]=uint8_t(op);put16(p+2,id);put32(p+4,seq++);put16(p+8,uint16_t(n));
     p[10]=op==Op::Data?0:uint8_t((offset==0?kFirst:0)|(offset+n==body.size()?kLast:0));
     if(n)std::memcpy(p+kHeader,body.data()+offset,n);
     result=s.receive(p,n+kHeader,now);if(!result)return false;offset+=n;
   } while(offset<body.size());return result;
 }
};
static std::vector<uint8_t> u32(uint32_t v) {std::vector<uint8_t>b(4);put32(b.data(),v);return b;}
static void drain(Session& s) {uint8_t p[kValue];unsigned count=0;while(s.peek(p)){assert(++count<2000);assert(s.accept());}}
static void ready(Session& s,Peer& p,uint16_t mtu=517) {
 uint8_t nonce[8]={1,2,3,4,5,6,7,8};assert(s.start(mtu,nonce,0));p.value=valueLimit(mtu);p.seq=0;
 drain(s);std::vector<uint8_t> ack(20);std::memcpy(ack.data(),nonce,8);ack[8]=9;ack[16]=2;put16(ack.data()+18,p.value);
 assert(p.send(s,Op::HelloAck,0,ack));assert(s.state==State::Ready);
}
static uint16_t opened(Session& s,Peer& p) {
 uint8_t host[4]={192,0,2,1};auto id=s.open(1,host,4,443,1);assert(id);drain(s);
 assert(p.send(s,Op::OpenOk,id,u32(1024)));return id;
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
 // Byte mutations never escape bounded storage, including invalid lengths/opcodes.
 for(unsigned offset=0;offset<24;++offset)for(unsigned v=0;v<256;++v) {
  Session s;Peer p;ready(s,p);opened(s,p);uint8_t f[24]{};
  f[0]=1;f[1]=uint8_t(Op::Data);put16(f+2,1);put32(f+4,p.seq);put16(f+8,12);f[offset]=uint8_t(v);
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
  assert(frame[1]==uint8_t(Op::Close) && get32(frame+4)==sequence);assert(s.accept());}
 // A response cannot precede submission of its OPEN message.
 {Session s;Peer p;ready(s,p);uint8_t host[4]={192,0,2,1};auto a=s.open(1,host,4,443,1);
  assert(a);assert(!p.send(s,Op::OpenOk,a,u32(1024)));}
 using namespace hw1httptransport;
 for(unsigned phase=0;phase<8;++phase) {
  Attempt a;assert(a.select(Path::Wifi));if(phase>=3)a.beforeHttpWrite();
  assert(a.fallback(Failure::Network,true)==(phase<3));
  assert(!a.fallback(Failure::Trust,true));assert(!a.fallback(Failure::Cancelled,true));
  if(phase<3){assert(a.select(Path::Phone));assert(!a.fallback(Failure::Network,true));}
  else assert(!a.select(Path::Phone));
  a.finish();assert(!a.select(Path::Wifi));
 }
 Health h;h.context(1,1);assert(h.directEligible());h.networkFailed(UINT32_MAX-10000);
 assert(!h.claimProbe(10000));assert(h.claimProbe(20000));assert(!h.claimProbe(20001));
 h.endProbe(false,20000);h.context(2,1);assert(h.directEligible());
 std::puts("phone pipe protocol/flow/lifetime and no-replay policy passed");
}
