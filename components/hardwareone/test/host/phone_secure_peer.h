#pragma once
// Synthetic phone endpoint for production-channel tests; uses the pinned real
// crypto libraries. Independent PBKDF2/HKDF vectors live in the crypto harness.
#include "BLE_PhoneSecureChannel.h"
#include <cassert>
#include <vector>
struct PhoneSecurePeer {
  using Bytes=std::vector<uint8_t>;
  uint8_t secret[32]{},pub[32]{},nonce[16]{},txKey[32]{},rxKey[32]{};
  uint64_t tx=1,rx=0;
  Bytes hello() {
    assert(sodium_init()>=0);assert(!crypto_kx_keypair(pub,secret));randombytes_buf(nonce,16);
    Bytes b(49);b[0]=1;memcpy(b.data()+1,pub,32);memcpy(b.data()+33,nonce,16);return b;
  }
  Bytes confirm(const Bytes& ack,const uint8_t psk[32],const char* label=hw1phone::kPipeLabel) {
    assert(ack.size()==49&&ack[0]==2);
    uint8_t shared[32],ikm[64],salt[32],keys[64],n[12];
    assert(!crypto_scalarmult(shared,secret,ack.data()+1));
    memcpy(ikm,shared,32);memcpy(ikm+32,psk,32);memcpy(salt,nonce,16);memcpy(salt+16,ack.data()+33,16);
    hw1sc::scHkdf(ikm,64,salt,32,label,strlen(label),keys,64);
    memcpy(txKey,keys,32);memcpy(rxKey,keys+32,32);hw1sc::scNonce(n,0,0);
    Bytes result(19);result[0]=3;const uint8_t ok[2]={'o','k'};unsigned long long tag;
    assert(!crypto_aead_chacha20poly1305_ietf_encrypt_detached(result.data()+1,result.data()+3,&tag,ok,2,nullptr,0,nullptr,n,txKey));
    return result;
  }
  bool confirmAck(const Bytes& b) {
    if(b.size()!=19||b[0]!=4)return false;
    uint8_t n[12],plain[2];hw1sc::scNonce(n,1,0);
    return !crypto_aead_chacha20poly1305_ietf_decrypt_detached(plain,nullptr,b.data()+1,2,b.data()+3,nullptr,0,n,rxKey)&&plain[0]=='o'&&plain[1]=='k';
  }
  Bytes seal(const Bytes& plain) {
    uint8_t n[12];hw1sc::scNonce(n,0,tx);Bytes result(25+plain.size());result[0]=0x10;
    for(unsigned i=0;i<8;++i)result[1+i]=uint8_t(tx>>(8*(7-i)));
    ++tx;unsigned long long tag;
    assert(!crypto_aead_chacha20poly1305_ietf_encrypt_detached(result.data()+9,result.data()+9+plain.size(),&tag,
        plain.data(),plain.size(),nullptr,0,nullptr,n,txKey));return result;
  }
  // P4-direction sealer (P4-to-phone key, direction 1) for contract vectors:
  // bytes a production P4 never writes, under the counter the P4 would use.
  Bytes sealAsP4(const Bytes& plain,uint64_t counter) const {
    uint8_t n[12];hw1sc::scNonce(n,1,counter);Bytes result(25+plain.size());result[0]=0x10;
    for(unsigned i=0;i<8;++i)result[1+i]=uint8_t(counter>>(8*(7-i)));
    unsigned long long tag;
    assert(!crypto_aead_chacha20poly1305_ietf_encrypt_detached(result.data()+9,result.data()+9+plain.size(),&tag,
        plain.data(),plain.size(),nullptr,0,nullptr,n,rxKey));return result;
  }
  Bytes open(const Bytes& b) {
    assert(b.size()>=25&&b[0]==0x10);uint64_t ctr=0;for(unsigned i=1;i<9;++i)ctr=(ctr<<8)|b[i];assert(ctr>rx);
    Bytes out(b.size()-25);uint8_t n[12];hw1sc::scNonce(n,1,ctr);
    assert(!crypto_aead_chacha20poly1305_ietf_decrypt_detached(out.data(),nullptr,b.data()+9,out.size(),b.data()+9+out.size(),nullptr,0,n,rxKey));
    rx=ctr;return out;
  }
};
