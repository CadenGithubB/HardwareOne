#pragma once
#include "System_BleSecureCrypto.h"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace hw1phone {
inline constexpr char kPipeLabel[] = "HW1-PIPE-v1";
constexpr size_t kSecureOverhead = 25;
constexpr uint16_t kMinimumMtu = 64;

// The phone initiates; P4 responds. This is the console v1 key schedule with
// a separate domain label and no console application-fragment header.
// Allocate the owner in internal RAM; it is never copied or used by callbacks.
class SecureChannel {
 public:
  enum class Result { Error, Reply, Data };
  SecureChannel() = default;
  SecureChannel(const SecureChannel&) = delete;
  SecureChannel& operator=(const SecureChannel&) = delete;
  ~SecureChannel() { reset(); }
  void reset() {
    sodium_memzero(rxKey_,sizeof(rxKey_));sodium_memzero(txKey_,sizeof(txKey_));
    phase_=0;rxCounter_=0;txCounter_=1;
  }
  bool ready() const { return phase_==4; }
  // Called only after the reply was accepted by the local GATT submission.
  bool replySubmitted() {
    if(phase_==1){phase_=2;return true;}
    if(phase_==3){phase_=4;return true;}
    return false;
  }
  Result receive(const uint8_t* in,size_t size,const uint8_t psk[32],
                 uint8_t* out,size_t cap,size_t& length) {
    length=0;
    if(!in || !size || !out)return error();
    if(in[0]==1 && phase_==0 && size==49 && cap>=49 && psk) {
      uint8_t secret[32],pub[32],nonce[16],shared[32],ikm[64],salt[32],keys[64];
      if(sodium_init()<0 || crypto_kx_keypair(pub,secret)!=0)return error();
      randombytes_buf(nonce,sizeof(nonce));
      const int exchanged=crypto_scalarmult(shared,secret,in+1);
      sodium_memzero(secret,sizeof(secret));
      if(exchanged!=0){sodium_memzero(shared,sizeof(shared));return error();}
      memcpy(ikm,shared,32);memcpy(ikm+32,psk,32);
      memcpy(salt,in+33,16);memcpy(salt+16,nonce,16);
      hw1sc::scHkdf(ikm,sizeof(ikm),salt,sizeof(salt),kPipeLabel,strlen(kPipeLabel),keys,sizeof(keys));
      memcpy(rxKey_,keys,32);memcpy(txKey_,keys+32,32);
      sodium_memzero(shared,sizeof(shared));sodium_memzero(ikm,sizeof(ikm));sodium_memzero(keys,sizeof(keys));
      out[0]=2;memcpy(out+1,pub,32);memcpy(out+33,nonce,16);length=49;phase_=1;
      return Result::Reply;
    }
    if(in[0]==3 && phase_==2 && size==19 && cap>=19) {
      uint8_t nonce[12],plain[2];hw1sc::scNonce(nonce,0,0);
      const bool valid=crypto_aead_chacha20poly1305_ietf_decrypt_detached(
          plain,nullptr,in+1,2,in+3,nullptr,0,nonce,rxKey_)==0 && plain[0]=='o' && plain[1]=='k';
      sodium_memzero(plain,sizeof(plain));
      if(!valid)return error();
      hw1sc::scNonce(nonce,1,0);out[0]=4;
      const uint8_t ok[2]={'o','k'};unsigned long long tag=0;
      if(crypto_aead_chacha20poly1305_ietf_encrypt_detached(out+1,out+3,&tag,ok,2,nullptr,0,nullptr,nonce,txKey_)!=0)
        return error();
      length=19;phase_=3;return Result::Reply;
    }
    if(in[0]!=0x10 || !ready() || size<kSecureOverhead || size-kSecureOverhead>cap)return error();
    uint64_t counter=0;for(unsigned i=1;i<9;++i)counter=(counter<<8)|in[i];
    if(counter<=rxCounter_)return error();
    const size_t count=size-kSecureOverhead;uint8_t nonce[12];hw1sc::scNonce(nonce,0,counter);
    if(crypto_aead_chacha20poly1305_ietf_decrypt_detached(out,nullptr,in+9,count,in+9+count,nullptr,0,nonce,rxKey_)!=0)
      return error();
    rxCounter_=counter;length=count;return Result::Data;
  }
  size_t encrypt(const uint8_t* plain,size_t size,uint8_t* out,size_t cap) {
    if(!ready() || !plain || !out || cap<kSecureOverhead || size>cap-kSecureOverhead ||
       txCounter_==std::numeric_limits<uint64_t>::max()) {reset();return 0;}
    const uint64_t counter=txCounter_++;uint8_t nonce[12];hw1sc::scNonce(nonce,1,counter);
    out[0]=0x10;for(unsigned i=0;i<8;++i)out[1+i]=uint8_t(counter>>(8*(7-i)));
    unsigned long long tag=0;
    if(crypto_aead_chacha20poly1305_ietf_encrypt_detached(out+9,out+9+size,&tag,plain,size,nullptr,0,nullptr,nonce,txKey_)!=0) {
      reset();return 0;
    }
    return size+kSecureOverhead;
  }
 private:
  Result error(){reset();return Result::Error;}
  uint8_t rxKey_[32]{},txKey_[32]{};
  uint64_t rxCounter_=0,txCounter_=1;
  uint8_t phase_=0;
};
} // namespace hw1phone
