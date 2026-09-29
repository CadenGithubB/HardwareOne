#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <set>
#include <string>
#define ENABLE_BLUETOOTH 1
#define ENABLE_G2_GLASSES 1
using TransportSessionEpoch=uint32_t;
enum CommandSource { SOURCE_INTERNAL, SOURCE_SERIAL, SOURCE_LOCAL_DISPLAY, SOURCE_G2_GLASSES, SOURCE_WEB };
struct String:std::string {
  using std::string::string;
  String()=default;
  String(const std::string& s):std::string(s){}
};
struct AuthContext { CommandSource transport=SOURCE_INTERNAL; String path,ip,user,sid;void* opaque=nullptr;String scope; };
static AuthContext auth;
static uint32_t epoch=7, uid=2;
static bool live=true, authed=true;
static String localUser="alice", peerUser="alice";
struct BlePeerOwnerSession {String user;uint32_t generation=1,transportEpoch=7;bool live()const{return generation&&transportEpoch&&!user.empty();}};
constexpr int BLE_PEER_G2_GLASSES=0;
static uint32_t peerEpoch=7;
bool blePeerOwnerSessionSnapshot(int,BlePeerOwnerSession& out){out.user=peerUser;out.transportEpoch=peerEpoch;return true;}
const AuthContext& currentAuthContext(){return auth;}
uint32_t captureTransportSessionEpoch(const AuthContext&){return epoch;}
bool transportSessionEpochIsLive(CommandSource,uint32_t e){return live&&e==epoch;}
uint32_t localDisplayTransportSessionSnapshot(String& user,bool& a){user=localUser;a=authed;return epoch;}
void secureClearString(String& user){user.assign(user.size(),'\0');user.clear();}
struct Settings {bool sttSaveTranscripts=false;} gSettings;
namespace Clock {static bool synced=true;time_t epochSeconds(){return 1790640000;}bool isSynced(){return synced;}}
static bool lockAvailable=true;
static unsigned locks=0, filesOpen=0, writes=0, opens=0, userLookups=0;
static unsigned shortWrite=0;
static bool writeError=false, wrongSize=false, denyOpen=false;
static bool idRevokes=false;
#define pdMS_TO_TICKS(n) (n)
struct FsLockGuard {bool held;FsLockGuard(const char*,unsigned):held(lockAvailable){if(held)++locks;}~FsLockGuard(){if(held)--locks;}};
bool isFsLockedByCurrentTask(){return locks!=0;}
bool getUserIdByUsername(const String& user,uint32_t& out){assert(locks);++userLookups;out=uid;if(idRevokes)live=false;return user=="alice"&&uid;}
namespace VFS {
 enum StorageType {INTERNAL, SDCARD};
 static std::map<std::string,std::string> files;
 static std::set<std::string> folders;
 static uint64_t available=1000000;
 static bool sd=false,sdPresent=true,statsOk=true,denyMkdir=false;
 static unsigned sdFailures=0,notedBytes=0;
 bool isSDWritable(){assert(locks);return sd&&sdPresent;}
 bool exists(const String& p){assert(locks);return files.count(p)||folders.count(p);}
 bool existsGuarded(const String& p,const AuthContext&){return exists(p);}
 bool mkdirGuarded(const String& p,const AuthContext& ctx){assert(locks);if(denyMkdir)return false;assert(ctx.user=="alice"||ctx.user=="system");folders.insert(p);return true;}
 AuthContext systemAuth(const char* scope,const char*){AuthContext c;c.user="system";c.scope=scope;return c;}
 bool getStats(StorageType type,uint64_t& total,uint64_t& used,uint64_t& free){assert(locks);total=1000000;used=0;free=available;return statsOk&&(type!=SDCARD||sdPresent);}
 void noteSDWriteFailure(const char*){++sdFailures;}
 void noteLittleFsBytesWritten(size_t n){notedBytes+=n;}
}
struct File {
 std::string path;
 bool valid=false,closed=true;
 File()=default;
 explicit File(const std::string& p):path(p),valid(true),closed(false){++filesOpen;}
 File(const File&)=delete;
 File(File&& other):path(other.path),valid(other.valid),closed(other.closed){other.closed=true;}
 ~File(){close();}
 explicit operator bool()const{return valid;}
 bool isDirectory()const{return false;}
 size_t size()const{return VFS::files.at(path).size()+(wrongSize?1:0);}
 size_t write(const uint8_t* bytes,size_t n){assert(locks&&!closed);++writes;size_t used=shortWrite==writes?n/2:n;VFS::files[path].append(reinterpret_cast<const char*>(bytes),used);return used;}
 void flush(){assert(locks&&!closed);}
 int getWriteError()const{return writeError?1:0;}
 void close(){if(!closed){assert(locks);closed=true;--filesOpen;}}
};
namespace VFS {
 File openGuarded(const String& p,const char* mode,const AuthContext& ctx,bool create){
  assert(locks&&ctx.user=="alice"&&create);++opens;
  if(denyOpen)return File();
  if(mode[0]=='w')files[p].clear();else if(!files.count(p))files[p]="";
  assert(p.compare(0,ctx.scope.size(),ctx.scope)==0);return File(p);
 }
}
// INSERT_INTERFACE
// INSERT_RUNTIME
static void reset(){
 assert(!locks&&!filesOpen);auth={};auth.transport=SOURCE_SERIAL;auth.user="alice";
 epoch=7;uid=2;live=authed=true;localUser=peerUser="alice";peerEpoch=7;
 gSettings.sttSaveTranscripts=true;Clock::synced=true;lockAvailable=true;
 writes=opens=userLookups=shortWrite=0;writeError=wrongSize=denyOpen=idRevokes=false;
 VFS::files.clear();VFS::folders.clear();VFS::available=1000000;
 VFS::sd=false;VFS::sdPresent=VFS::statsOk=true;VFS::denyMkdir=false;
 VFS::sdFailures=VFS::notedBytes=0;
}
static TranscriptOptions options(){return transcriptCaptureOptions(SOURCE_SERIAL,7);}
static std::string text(const TranscriptSession& s){return VFS::files.at(s.snapshot().path);}
static void complete(){
 reset();auto opt=options();gSettings.sttSaveTranscripts=false;
 TranscriptSession session;session.begin(opt,"local",0x123456789abcdef0ULL);
 assert(session.snapshot().enabled&&VFS::files.empty()&&!userLookups);
 assert(session.append(1,"",0)&&VFS::files.empty());
 assert(session.append(2,"first words",11));auto path=std::string(session.snapshot().path);
 assert(path.find("/stt/u2/")==0&&path.size()<128);
 assert(session.snapshot().chunks==1&&session.snapshot().saved&&!session.snapshot().complete);
 const auto before=text(session);unsigned count=writes;
 assert(session.append(2,"first words",11)&&session.append(1,"",0));
 assert(text(session)==before&&writes==count);
 assert(session.append(3,"second phrase",13));session.finish("done");
 assert(session.snapshot().complete&&!session.snapshot().error[0]&&session.snapshot().chunks==2);
 assert(text(session).find("first words\nsecond phrase\n\n[End: done]\n")!=std::string::npos);
 assert(session.snapshot().bytes==text(session).size()&&!filesOpen&&!locks);
 count=writes;session.finish("done");assert(writes==count&&!session.append(4,"late",4));
 TranscriptSession collision;collision.begin(opt,"local",0x123456789abcdef0ULL);
 const auto original=text(session);assert(!collision.append(1,"overwrite",9));assert(text(session)==original);
}
static void disabled_and_identity(){
 reset();gSettings.sttSaveTranscripts=false;auto opt=options();gSettings.sttSaveTranscripts=true;
 TranscriptSession off;off.begin(opt,"pi",1);assert(off.append(1,"words",5));off.finish("done");assert(VFS::files.empty()&&!opens&&!userLookups);
 auto oled=transcriptCaptureOptions(SOURCE_LOCAL_DISPLAY,7);assert(!strcmp(oled.user,"alice"));
 authed=false;assert(!transcriptCaptureOptions(SOURCE_LOCAL_DISPLAY,7).user[0]);authed=true;
 auto g2=transcriptCaptureOptions(SOURCE_G2_GLASSES,7);assert(!strcmp(g2.user,"alice"));
 peerEpoch=8;assert(!transcriptCaptureOptions(SOURCE_G2_GLASSES,7).user[0]);
 assert(!transcriptCaptureOptions(SOURCE_WEB,7).user[0]);
 assert(!transcriptCaptureOptions(SOURCE_SERIAL,8).user[0]);
 TranscriptSession invalid;auto missing=options();missing.user[0]=0;invalid.begin(missing,"pi",2);assert(!invalid.append(1,"word",4)&&invalid.snapshot().error[0]);
 for(bool revokeDuringLookup:{false,true}){reset();auto x=options();TranscriptSession expired;expired.begin(x,"pi",3);live=revokeDuringLookup;idRevokes=revokeDuringLookup;assert(!expired.append(1,"old",3)&&VFS::files.empty());}
 reset();TranscriptSession renamed;renamed.begin(options(),"pi",4);assert(renamed.append(1,"original",8));const auto prior=text(renamed);uid=3;assert(!renamed.append(2,"replacement",11)&&text(renamed)==prior);
}
static void faults(){
 for(int fault=0;fault<10;++fault){
  reset();TranscriptSession s;s.begin(options(),"local",10+fault);
  switch(fault){case 0:lockAvailable=false;break;case 1:VFS::available=100*1024;break;case 2:VFS::statsOk=false;break;case 3:VFS::denyMkdir=true;break;case 4:denyOpen=true;break;case 5:shortWrite=1;break;case 6:shortWrite=2;break;case 7:writeError=true;break;case 8:wrongSize=true;break;case 9:uid=0;break;}
  assert(!s.append(1,"must not be silently lost",24));assert(s.snapshot().error[0]&&!s.snapshot().complete&&!filesOpen&&!locks);
  unsigned old=writes;s.finish("done");assert(!s.append(2,"later",5)&&writes==old);
 }
 reset();TranscriptSession gap;gap.begin(options(),"local",24);assert(!gap.append(2,"gap",3)&&VFS::files.empty());
 reset();TranscriptSession removed;removed.begin(options(),"pi",25);assert(removed.append(1,"one",3));VFS::files.erase(removed.snapshot().path);assert(!removed.append(2,"two",3)&&VFS::files.empty());
 reset();TranscriptSession changed;changed.begin(options(),"pi",26);assert(changed.append(1,"one",3));VFS::files[changed.snapshot().path]+="edited";auto edited=text(changed);assert(!changed.append(2,"two",3)&&text(changed)==edited);
 reset();TranscriptSession sd;VFS::sd=true;sd.begin(options(),"pi",27);assert(sd.append(1,"card",4));assert(std::string(sd.snapshot().path).find("/sd/stt/u2/")==0);VFS::sdPresent=false;assert(!sd.append(2,"removed",7)&&VFS::files.size()==1);
 reset();TranscriptSession internal;internal.begin(options(),"local",28);assert(internal.append(1,"internal",8));VFS::sd=true;assert(internal.append(2,"same tier",9)&&std::string(internal.snapshot().path).find("/stt/")==0);
 reset();VFS::sd=true;shortWrite=2;TranscriptSession shortSd;shortSd.begin(options(),"pi",29);assert(!shortSd.append(1,"partial",7)&&VFS::sdFailures==1);
 reset();TranscriptSession noFooter;noFooter.begin(options(),"pi",30);assert(noFooter.append(1,"saved",5));VFS::available=0;noFooter.finish("done");assert(!noFooter.snapshot().complete&&text(noFooter).find("[End:")==std::string::npos);
}
static void terminal_and_bounds(){
 for(const char* end:{"cancelled","failed","done"}){reset();Clock::synced=false;TranscriptSession s;s.begin(options(),"pi",40);assert(s.append(1,"retained",8));s.finish(end);assert(s.snapshot().complete&&text(s).find(end)!=std::string::npos&&std::string(s.snapshot().path).find("boot-pi-")!=std::string::npos);}
 reset();TranscriptSession silence;silence.begin(options(),"local",41);assert(silence.append(1,"",0));silence.finish("cancelled");assert(VFS::files.empty());
 reset();TranscriptSession max;max.begin(options(),"local",42);std::string full(512,'x');assert(max.append(1,full.c_str(),full.size()));max.finish("done");assert(max.snapshot().chunks==1);
 reset();TranscriptSession span;span.begin(options(),"pi",44);char exact[3]={'a','b','c'};assert(span.append(1,exact,sizeof(exact)));span.finish("done");
 reset();TranscriptSession embedded;embedded.begin(options(),"pi",45);char bad[3]={'a','\0','b'};assert(!embedded.append(1,bad,sizeof(bad))&&VFS::files.empty());
 reset();TranscriptSession oversized;oversized.begin(options(),"local",43);std::string huge(513,'x');assert(!oversized.append(1,huge.c_str(),huge.size())&&VFS::files.empty());
 assert(!locks&&!filesOpen);
}
int main(){complete();disabled_and_identity();faults();terminal_and_bounds();puts("Shared transcript writer: identity, latch, replay, storage and cleanup checks passed");}
