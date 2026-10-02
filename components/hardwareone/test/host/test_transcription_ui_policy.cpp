#include "../../Transcription_UI_Policy.h"
#include <cassert>
#include <string>
#include <cstdio>
int main() {
 uint64_t id; uint32_t n;
 assert(TranscriptionUI::parseId("0123456789abcdef",id)&&id==0x0123456789abcdefULL);
 for(const char* bad:{"","0","0000000000000000","0123456789abcdeg","0123456789abcdefx","-123456789abcdef"})
  assert(!TranscriptionUI::parseId(bad,id));
 assert(TranscriptionUI::parseUnsigned("4294967295",n)&&n==UINT32_MAX);
 for(const char* bad:{"","-1","1x","4294967296"," 1","1.0"})assert(!TranscriptionUI::parseUnsigned(bad,n));
 std::string words="a\xc3\xa9\xe2\x82\xac\xf0\x9f\x8e\x99z";
 for(size_t end=1;end<=words.size();++end) {
  size_t take=TranscriptionUI::completePrefix(words.data(),end);
  assert(take<=end);
  if(take<words.size())assert(!TranscriptionUI::continuation((unsigned char)words[take]));
 }
 assert(TranscriptionUI::completePrefix("\xe2\x82",2)==0);
 char tail[10]{};TranscriptionUI::appendRecent(tail,sizeof(tail),"abc",3);
 TranscriptionUI::appendRecent(tail,sizeof(tail),"defghijkl",9);assert(std::string(tail)=="defghijkl");
 for(size_t i=0;i<100;++i) {
  TranscriptionUI::appendRecent(tail,sizeof(tail),words.data(),words.size());
  assert(strlen(tail)<sizeof(tail));assert(!TranscriptionUI::continuation((unsigned char)tail[0]));
 }
 TranscriptionUI::clear(tail,sizeof(tail));for(char c:tail)assert(c==0);
 assert(TranscriptionUI::safeFilename("My session.txt"));
 for(const char* bad:{"","../x.txt","a\".txt","a\\b.txt","a\nb.txt"})assert(!TranscriptionUI::safeFilename(bad));
 using TranscriptionUI::nextMicSource; using TranscriptionUI::micLabel;
 assert(std::string(nextMicSource("auto",true,true))=="pdm"&&std::string(nextMicSource("pdm",true,true))=="g2");
 assert(std::string(nextMicSource("g2",true,true))=="auto"&&std::string(nextMicSource("auto",false,true))=="g2");
 assert(std::string(nextMicSource("auto",false,false))=="auto"&&std::string(nextMicSource("g2",true,false))=="pdm");
 assert(std::string(micLabel("pdm"))=="Onboard"&&std::string(micLabel("g2"))=="Glasses"&&std::string(micLabel(nullptr))=="Auto");
 puts("Transcription UI policy: strict IDs/offsets, bounded UTF-8 pages/tails and filenames passed");
}
