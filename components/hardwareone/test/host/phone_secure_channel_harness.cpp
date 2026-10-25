#include "phone_secure_peer.h"
#include <cstdio>
using Channel=hw1phone::SecureChannel;
using Bytes=PhoneSecurePeer::Bytes;
static uint8_t psk[32]={1};
static Channel::Result receive(Channel& c,const Bytes& bytes,Bytes& out) {
  out.resize(512);size_t length=0;auto result=c.receive(bytes.data(),bytes.size(),psk,out.data(),out.size(),length);out.resize(length);return result;
}
static void established(Channel& c,PhoneSecurePeer& p) {
  Bytes out;assert(receive(c,p.hello(),out)==Channel::Result::Reply);assert(!c.ready());
  assert(c.replySubmitted());auto confirm=p.confirm(out,psk);
  assert(receive(c,confirm,out)==Channel::Result::Reply);assert(!c.ready());assert(p.confirmAck(out));
  assert(c.replySubmitted()&&c.ready());assert(!c.replySubmitted());
}
static void vector(const uint8_t* bytes,const char* hex,size_t n) {
  for(size_t i=0;i<n;++i){unsigned expected=0;assert(sscanf(hex+2*i,"%2x",&expected)==1);assert(bytes[i]==expected);}
}
int main() {
  uint8_t out[64];const char* pw="SyntheticPipe!123";
  assert(hw1sc::scPbkdf2((const uint8_t*)pw,strlen(pw),(const uint8_t*)hw1phone::kPipeLabel,strlen(hw1phone::kPipeLabel),100000,out));
  vector(out,"7cbd65005cf381ea90f6faa75bafef1b9fbf0ac203106c2dfab332b26962298d",32);
  uint8_t ikm[22];memset(ikm,0x0b,22);uint8_t salt[13],info[10];for(int i=0;i<13;++i)salt[i]=i;for(int i=0;i<10;++i)info[i]=0xf0+i;
  hw1sc::scHkdf(ikm,22,salt,13,(char*)info,10,out,42);
  vector(out,"3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865",42);
  hw1sc::scNonce(out,1,0x0102030405060708ULL);vector(out,"000000010102030405060708",12);
  assert(!hw1sc::scPbkdf2((const uint8_t*)pw,strlen(pw),(const uint8_t*)"x",1,0,out));
  for(int bad=0;bad<3;++bad) {
    Channel c;PhoneSecurePeer p;Bytes response;assert(receive(c,p.hello(),response)==Channel::Result::Reply);assert(c.replySubmitted());
    uint8_t wrong[32]={2};auto confirm=p.confirm(response,bad==0?wrong:psk,bad==1?"HW1-SC-v1":hw1phone::kPipeLabel);
    if(bad==2)confirm.back()^=1;
    assert(receive(c,confirm,response)==Channel::Result::Error&&!c.ready());
  }
  {Channel c;PhoneSecurePeer p;Bytes response;auto hello=p.hello();assert(receive(c,hello,response)==Channel::Result::Reply);
    auto confirm=p.confirm(response,psk);assert(receive(c,confirm,response)==Channel::Result::Error);}
  {Channel c;PhoneSecurePeer p;Bytes response;auto hello=p.hello();memset(hello.data()+1,0,32);assert(receive(c,hello,response)==Channel::Result::Error);}
  for(int bad=0;bad<6;++bad) {
    Channel c;PhoneSecurePeer p;established(c,p);Bytes response;Bytes plain={1,2,3,4};auto wire=p.seal(plain);
    assert(receive(c,wire,response)==Channel::Result::Data&&response==plain);
    if(bad==0){} // replay
    if(bad==1){wire=p.seal(plain);wire.back()^=1;}
    if(bad==2)wire=plain;
    if(bad==3)wire=p.hello();
    if(bad==4){wire=p.seal(plain);wire[8]=0;}
    if(bad==5){wire.resize(24);}
    assert(receive(c,wire,response)==Channel::Result::Error&&!c.ready());
  }
  // Secure counters may skip values, but cannot repeat/regress after the gap.
  {Channel c;PhoneSecurePeer p;established(c,p);Bytes response;Bytes plain={1,2,3};
    p.tx=7;auto wire=p.seal(plain);
    assert(receive(c,wire,response)==Channel::Result::Data&&response==plain);
    p.tx=9;wire=p.seal(plain);assert(receive(c,wire,response)==Channel::Result::Data);
    p.tx=8;wire=p.seal(plain);assert(receive(c,wire,response)==Channel::Result::Error);}
  for(size_t n:{size_t(0),size_t(36),size_t(216),size_t(219),size_t(487)}) {
    Channel c;PhoneSecurePeer p;established(c,p);Bytes plain(n,0x51),wire(512),response;
    auto app=p.seal(plain);assert(receive(c,app,response)==Channel::Result::Data&&response==plain);
    uint8_t dummy=0;size_t length=c.encrypt(n?plain.data():&dummy,n,wire.data(),wire.size());assert(length==n+25);wire.resize(length);assert(p.open(wire)==plain);
    c.reset();assert(!c.ready()&&!c.encrypt(&dummy,1,wire.data(),wire.size()));
  }
  puts("Phone secure channel: independent PBKDF2/RFC5869 vectors, domain separation, bidirectional AEAD, ACK gates, wrong keys, replay, malformed frames and reset passed");
}
