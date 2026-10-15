#include "BLE_PhoneIdentityCore.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>
int main() {
  uint8_t address[6] = {}, irk[16] = {1}; char text[18];
  assert(hw1phone::parseIdentity("c2:00:00:00:00:01", 1, address));
  assert(hw1phone::formatIdentity(address, 1, text));
  assert(!std::strcmp(text, "c2:00:00:00:00:01"));
  assert(hw1phone::parseIdentity("C2:00:00:00:00:01", 1, address));
  assert(hw1phone::parseIdentity("02:00:00:00:00:01", 0, address));
  for (const char* invalid : {"", "c2:00:00:00:00", "c2:00:00:00:00:01x",
       "xx:00:00:00:00:01", "c2-00:00:00:00:01", "00:00:00:00:00:00",
       "ff:ff:ff:ff:ff:ff", "c0:00:00:00:00:00"}) {
    uint8_t before[6]; std::memcpy(before, address, 6);
    assert(!hw1phone::parseIdentity(invalid, 1, address));
    assert(!std::memcmp(before, address, 6));
  }
  assert(!hw1phone::parseIdentity("42:00:00:00:00:01", 1, address)); // RPA
  assert(!hw1phone::parseIdentity("02:00:00:00:00:01", 1, address)); // NRPA
  assert(!hw1phone::parseIdentity("c2:00:00:00:00:01", 2, address));
  assert(hw1phone::strongBond(true,true,true,16,true,irk));
  for (unsigned bits=0; bits<16; ++bits)
    assert(hw1phone::strongBond(bits&1,bits&2,bits&4,16,bits&8,irk) == (bits==15));
  for (unsigned size=0; size<256; ++size)
    assert(hw1phone::strongBond(true,true,true,uint8_t(size),true,irk) == (size==16));
  std::memset(irk, 0, sizeof(irk));
  assert(!hw1phone::strongBond(true,true,true,16,true,irk));
  assert(!hw1phone::strongBond(true,true,true,16,true,nullptr));
  puts("Phone identity: public/static normalization, private-address rejection, strong-bond gates passed");
}
