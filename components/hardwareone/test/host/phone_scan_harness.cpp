#include "BLE_PhonePipeScan.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#ifdef NDEBUG
#error "This harness requires assertions"
#endif
using namespace hw1scan;
static std::string show(const Profile& p){char b[96];format(b,sizeof(b),p);return b;}
int main(){
  // Profiles: the default scans continuously without the duplicate filter for
  // up to 4 s; legacy is the pre-firmware-007 profile.
  assert(kDefault.window==kDefault.interval && !kDefault.duplicates && kDefault.seconds==4);
  assert(kLegacy.interval==160 && kLegacy.window==48 && kLegacy.duplicates && kLegacy.seconds==2);
  assert(show(kDefault)=="interval_ms=60.00 window_ms=60.00 duplicates=off up_to_s=4");
  assert(show(kLegacy)=="interval_ms=100.00 window_ms=30.00 duplicates=on up_to_s=2");
  Profile p=kLegacy;
  assert(!parse("default",p) && same(p,kDefault));
  assert(!parse(" legacy",p) && same(p,kLegacy));
  assert(!parse("90 30 nodup",p) && p.interval==144 && p.window==48 && !p.duplicates && p.seconds==4);
  assert(!parse("100 100 dup",p) && p.interval==160 && p.window==160 && p.duplicates);
  assert(!parse("100 30 nodup 2",p) && p.interval==160 && p.window==48 && !p.duplicates && p.seconds==2);
  assert(!parse("60 60 dup  8",p) && p.duplicates && p.seconds==8);
  assert(!parse("61 3 nodup",p) && p.interval==98 && p.window==5 && show(p)=="interval_ms=61.25 window_ms=3.12 duplicates=off up_to_s=4");
  const Profile before=p;
  for(const char* bad:{"","dup","90","90 30","90 30 maybe","90 30 dup extra","2 2 dup","1001 30 dup",
                       "30 31 dup","90 2 dup","-5 3 dup","x 3 dup","90 x dup","90 30 dupx","90 30 dup 0",
                       "90 30 dup 9","90 30 nodup 3s","90 30 nodup 3 4"}) {
    assert(parse(bad,p)!=nullptr);assert(same(p,before));
  }
  assert(unitsFromMs(3)==5 && unitsFromMs(1000)==1600);

  puts("Phone scan: default and legacy profiles and strict parsing (records: ble_coex_records)");
}
