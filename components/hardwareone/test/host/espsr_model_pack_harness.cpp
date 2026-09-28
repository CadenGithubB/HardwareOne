#include "System_ESPSRModelPack.h"
#include <assert.h>
#include <fstream>
#include <iterator>
#include <vector>
#include <iostream>

using Bytes = std::vector<uint8_t>;
static void put(Bytes& b, size_t at, uint32_t n) {
  for (int i = 0; i < 4; ++i) b.at(at + i) = uint8_t(n >> (8 * i));
}
static Bytes fixture() {
  Bytes b(128, 0);
  put(b, 0, 1); memcpy(b.data()+4, "wn9_test", 9); put(b, 36, 2);
  memcpy(b.data()+40, "_MODEL_INFO_", 13); put(b,72,120); put(b,76,4);
  memcpy(b.data()+80, "wn9_data", 9); put(b,112,124); put(b,116,4);
  memcpy(b.data()+120, "testdata", 8);
  return b;
}
static void valid(const Bytes& b) { const char* e="stale"; assert(ESPSRModelPack::validate(b.data(), b.size(), &e)); assert(!e); }
static void invalid(const Bytes& b) { const char* e=nullptr; assert(!ESPSRModelPack::validate(b.data(), b.size(), &e)); assert(e); }
int main(int argc, char** argv) {
  Bytes b=fixture(); valid(b);
  for(size_t n=0;n<b.size();++n) invalid(Bytes(b.begin(), b.begin()+n));
  assert(!ESPSRModelPack::validate(nullptr,128));
  assert(!ESPSRModelPack::validate(b.data(),ESPSRModelPack::kMaxFileBytes+1));
  for(auto v: {0u, 17u, 0xffffffffu}) { auto t=b;put(t,0,v);invalid(t); }
  for(auto v: {0u, 33u, 0xffffffffu}) { auto t=b;put(t,36,v);invalid(t); }
  for(auto at: {4u,40u,80u}) {
    auto t=b;memset(t.data()+at,'x',32);invalid(t);
    t=b;t[at]=0;invalid(t);
  }
  for(auto at: {72u,112u}) for(auto v: {0u,119u,121u,127u,128u,0xffffffffu}) {
    auto t=b;put(t,at,v);invalid(t);
  }
  for(auto at: {76u,116u}) for(auto v: {0u,3u,5u,0xffffffffu}) {
    auto t=b;put(t,at,v);invalid(t);
  }
  auto t=b;memcpy(t.data()+80,t.data()+40,32);invalid(t);
  t=b;t[40]='X';invalid(t);
  t=b;t.push_back(0);invalid(t);
  // One-bit mutations exercise bounds and string handling under sanitizers;
  // changes in the coefficient payload are deliberately not rejected.
  for(size_t n=0;n<b.size();++n) for(int bit=0;bit<8;++bit) {
    t=b;t[n]^=uint8_t(1u<<bit);ESPSRModelPack::validate(t.data(),t.size());
  }
  for(int i=1;i<argc;++i) {
    std::ifstream f(argv[i],std::ios::binary);assert(f);
    Bytes real((std::istreambuf_iterator<char>(f)),{});valid(real);
    std::cout << "Validated official bundle: " << real.size() << " bytes\n";
  }
  std::cout << "Packed model validation passed\n";
}
