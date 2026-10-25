// Offline synthetic transcript. Only entropy is replaced; responder handshake,
// key schedule, AEAD and pipe HELLO framing are the production implementation.
// Writes phone_secure_channel_v1_pipe_v2_vectors.json: the outer channel is
// still HW1-PIPE-v1 (every handshake and key field equals the frozen v1
// golden); the first DATA each way is the inner protocol v2 HELLO/HELLO_ACK.
#include <sodium.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
static unsigned keyCalls=0,nonceCalls=0;
static int fixedKeypair(unsigned char* pub,unsigned char* secret) {
  ++keyCalls;for(unsigned i=0;i<32;++i)secret[i]=uint8_t(0x20+i);
  return crypto_scalarmult_base(pub,secret);
}
static void fixedNonce(void* out,size_t size) {
  assert(size==16);++nonceCalls;
  for(unsigned i=0;i<size;++i)static_cast<unsigned char*>(out)[i]=uint8_t(0x10+i);
}
#define crypto_kx_keypair fixedKeypair
#define randombytes_buf fixedNonce
#include "BLE_PhoneSecureChannel.h"
#undef crypto_kx_keypair
#undef randombytes_buf
#include "phone_secure_peer.h"
#include "BLE_PhonePipeCore.h"
using Bytes=PhoneSecurePeer::Bytes;
static void field(const char* name,const uint8_t* p,size_t size) {
  printf("  \"%s\": \"",name);for(size_t i=0;i<size;++i)printf("%02x",p[i]);printf("\",\n");
}
static void field(const char* name,const Bytes& b){field(name,b.data(),b.size());}
int main() {
  assert(sodium_init()>=0);
  const char* passphrase="SyntheticPipe!123";uint8_t psk[32];
  assert(hw1sc::scPbkdf2(reinterpret_cast<const uint8_t*>(passphrase),strlen(passphrase),
    reinterpret_cast<const uint8_t*>(hw1phone::kPipeLabel),strlen(hw1phone::kPipeLabel),100000,psk));
  PhoneSecurePeer phone;
  for(unsigned i=0;i<32;++i)phone.secret[i]=uint8_t(i);
  for(unsigned i=0;i<16;++i)phone.nonce[i]=uint8_t(i);
  assert(!crypto_scalarmult_base(phone.pub,phone.secret));
  Bytes hello(49);hello[0]=1;memcpy(hello.data()+1,phone.pub,32);memcpy(hello.data()+33,phone.nonce,16);
  hw1phone::SecureChannel device;
  auto receive=[&](const Bytes& in,hw1phone::SecureChannel::Result expected){
    Bytes out(512);size_t size=0;
    assert(device.receive(in.data(),in.size(),psk,out.data(),out.size(),size)==expected);out.resize(size);return out;
  };
  const auto ack=receive(hello,hw1phone::SecureChannel::Result::Reply);assert(device.replySubmitted());
  const auto confirm=phone.confirm(ack,psk);
  const auto confirmAck=receive(confirm,hw1phone::SecureChannel::Result::Reply);
  assert(phone.confirmAck(confirmAck));assert(device.replySubmitted()&&device.ready());
  assert(keyCalls==1&&nonceCalls==1);
  // MTU247 -> physical244, inner219. Nonces are fixed nonzero little-endian u64s.
  // Inner protocol v2: the P4 offers the console; the phone has relay on and
  // wants the console (the flags X1 v2 uses, so its first DATA equals these).
  hw1pipe::Session pipe;const uint8_t pipeNonce[8]={1,2,3,4,5,6,7,8};
  pipe.setLocalFlags(hw1pipe::kP4ConsoleOffered);
  assert(pipe.start(247,pipeNonce,0,hw1phone::kSecureOverhead));
  Bytes pipeHello(512);pipeHello.resize(pipe.peek(pipeHello.data()));assert(pipe.accept(0));
  assert(pipeHello.size()==24 && pipeHello[0]==hw1pipe::kVersion && pipeHello[20]==hw1pipe::kMaxStreams &&
         pipeHello[21]==hw1pipe::kP4ConsoleOffered);
  Bytes deviceData(512);deviceData.resize(device.encrypt(pipeHello.data(),pipeHello.size(),deviceData.data(),deviceData.size()));
  assert(phone.open(deviceData)==pipeHello);
  Bytes pipeAck(32);pipeAck[0]=hw1pipe::kVersion;pipeAck[1]=uint8_t(hw1pipe::Op::HelloAck);
  hw1pipe::put16(pipeAck.data()+8,20);pipeAck[10]=3;
  memcpy(pipeAck.data()+12,pipeNonce,8);for(unsigned i=0;i<8;++i)pipeAck[20+i]=uint8_t(0x11+i);
  pipeAck[28]=hw1pipe::kMaxStreams;pipeAck[29]=hw1pipe::kPhoneRelayOn|hw1pipe::kPhoneConsoleWanted;
  hw1pipe::put16(pipeAck.data()+30,219);
  const auto phoneData=phone.seal(pipeAck);
  const auto decoded=receive(phoneData,hw1phone::SecureChannel::Result::Data);assert(decoded==pipeAck);
  assert(pipe.receive(decoded.data(),decoded.size(),1)&&pipe.state==hw1pipe::State::Ready);
  assert(pipe.peerFlags()==(hw1pipe::kPhoneRelayOn|hw1pipe::kPhoneConsoleWanted));
  uint8_t deviceSecret[32],shared[32];for(unsigned i=0;i<32;++i)deviceSecret[i]=uint8_t(0x20+i);
  assert(!crypto_scalarmult(shared,phone.secret,ack.data()+1));
  puts("{");
  printf("  \"passphrase_utf8\": \"%s\",\n",passphrase);
  printf("  \"label_ascii\": \"%s\",\n",hw1phone::kPipeLabel);
  puts("  \"pbkdf2_iterations\": 100000,\n  \"att_mtu\": 247,");
  field("phone_private_key_hex",phone.secret,32);field("phone_public_key_hex",phone.pub,32);
  field("device_private_key_hex",deviceSecret,32);field("device_public_key_hex",ack.data()+1,32);
  field("phone_nonce_hex",phone.nonce,16);field("device_nonce_hex",ack.data()+33,16);
  field("psk_hex",psk,32);field("shared_secret_hex",shared,32);
  field("phone_to_p4_key_hex",phone.txKey,32);field("p4_to_phone_key_hex",phone.rxKey,32);
  field("hello_hex",hello);field("hello_ack_hex",ack);field("confirm_hex",confirm);field("confirm_ack_hex",confirmAck);
  field("p4_to_phone_plaintext_hello_hex",pipeHello);field("p4_to_phone_data_hex",deviceData);
  field("phone_to_p4_plaintext_hello_ack_hex",pipeAck);field("phone_to_p4_data_hex",phoneData);
  puts("  \"data_counter_each_direction\": 1\n}");
}
