#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <string>
#include <vector>
// INSERT_POLICY
class String {
    std::string text_;
 public:
    String()=default;
    String(const char* s):text_(s?s:""){}
    String(const std::string& s):text_(s){}
    size_t length()const{return text_.size();}
    const char* c_str()const{return text_.c_str();}
    char operator[](size_t i)const{return i<length()?text_[i]:0;}
    bool startsWith(const String& s)const{return text_.rfind(s.text_,0)==0;}
    bool endsWith(const String& s)const{return length()>=s.length() && text_.compare(length()-s.length(),s.length(),s.text_)==0;}
    int indexOf(const char* s)const{auto n=text_.find(s);return n==std::string::npos?-1:int(n);}
    int indexOf(char c)const{auto n=text_.find(c);return n==std::string::npos?-1:int(n);}
    int lastIndexOf(char c)const{auto n=text_.rfind(c);return n==std::string::npos?-1:int(n);}
    String substring(size_t i)const{return text_.substr(std::min(i,length()));}
    void reserve(size_t n){text_.reserve(n);}
    void remove(size_t i){text_.erase(i);}
    void replace(const char* before,const char* after){const size_t n=std::strlen(before);for(size_t i=0;(i=text_.find(before,i))!=std::string::npos;i+=std::strlen(after))text_.replace(i,n,after);}
    void toLowerCase(){for(char& c:text_)c=char(std::tolower(static_cast<unsigned char>(c)));}
    void trim(){const auto a=text_.find_first_not_of(" \t\r\n"),b=text_.find_last_not_of(" \t\r\n");text_=a==std::string::npos?"":text_.substr(a,b-a+1);}
    String& operator+=(char c){text_+=c;return *this;}
    String& operator+=(const String& s){text_+=s.text_;return *this;}
    String& operator=(char c){text_=c;return *this;}
    friend String operator+(const String& a,const String& b){return a.text_+b.text_;}
    friend bool operator==(const String& a,const String& b){return a.text_==b.text_;}
    friend bool operator!=(const String& a,const String& b){return !(a==b);}
};
constexpr unsigned ESPNOW_V4_MAX_PLAINTEXT=202;
constexpr uint8_t ESPNOW_V4_TYPE_FS_LIST_REPLY=1,ESPNOW_V4_TYPE_FS_STAT_REPLY=2,ESPNOW_V4_TYPE_FS_GET_ACK=3;
// INSERT_WIRE
enum CommandSource { SOURCE_WEB = 0, SOURCE_INTERNAL = 2, SOURCE_ESPNOW = 3 };
struct AuthContext { CommandSource transport = SOURCE_WEB; String path, ip, user, sid; void* opaque = nullptr; String scope; };
inline constexpr const char* kBondAdminUser = "bond-admin";
#ifndef ENABLE_BONDED_MODE
#define ENABLE_BONDED_MODE 1
#endif
static AuthContext auth;
static unsigned scopes=0,scopeDepth=0,fsCalls=0,transfers=0,statsCalls=0,sdChecks=0,sends=0;
static bool filesystemReady=true,sdAvailable=true,statsOkay=true,allocFails=false,openFails=false;
// Bond state the requester gate consults: the configured bond peer and whether its session is live.
static const uint8_t bondMac[6]={1,2,3,4,5,6};
static bool bondConfigured=true,bondSession=true;
static std::vector<std::string> installed;   // "<user>@<transport>" per identity install, in order
static AuthContext lastInstalled;
static std::vector<uint8_t> response;
static uint8_t responseType;
static String transferPath;
// Real guard name, so the handlers compile whether they install SYSTEM_IDENTITY_SCOPE or the requester's identity.
struct ExecIdentityGuard {
    AuthContext saved;
    explicit ExecIdentityGuard(const AuthContext& install):saved(auth){
        auth=install;lastInstalled=install;++scopes;++scopeDepth;
        installed.push_back(std::string(install.user.c_str())+(install.transport==SOURCE_ESPNOW?"@espnow":install.transport==SOURCE_INTERNAL?"@internal":"@other"));
    }
    ~ExecIdentityGuard(){auth=saved;--scopeDepth;}
};
AuthContext systemIdentity(const char*){AuthContext c;c.transport=SOURCE_INTERNAL;c.user="system";return c;}
#define SYSTEM_IDENTITY_SCOPE(label) ExecIdentityGuard identity(systemIdentity(label))
const AuthContext& currentAuthContext(){assert(scopeDepth);return auth;}
namespace BondedPeer{bool peerMacBytes(uint8_t out[6]){if(!bondConfigured)return false;std::memcpy(out,bondMac,6);return true;}}
bool isBondSessionTokenValid(){return bondSession;}
// Mirrors the two resolveRole branches that matter: SYSTEM is unrestricted; the bond identity over
// ESP-NOW is SUPER only while the bond session is live, and nobody otherwise.
static bool identityMayRead(){return (auth.transport==SOURCE_INTERNAL&&auth.user=="system")||(auth.transport==SOURCE_ESPNOW&&auth.user==kBondAdminUser&&bondSession);}
struct FsLockGuard{explicit FsLockGuard(const char*){}};
template<class... T>void logMessage(const char*,T...){ }
#define DEBUG_ESPNOWF(...) logMessage(__VA_ARGS__)
#define WARN_ESPNOWF(...) logMessage(__VA_ARGS__)
#define ERROR_MEMORYF(...) logMessage(__VA_ARGS__)
enum class AllocPref{PreferPSRAM};
void* ps_alloc(size_t n,AllocPref,const char*){return allocFails?nullptr:std::malloc(n);}
size_t strlcpy(char* dst,const char* src,size_t cap){size_t n=std::strlen(src);if(cap){size_t copy=std::min(n,cap-1);std::memcpy(dst,src,copy);dst[copy]=0;}return n;}
uint32_t generateMessageId(){return 1;}
namespace espnowtx{
bool sendAeadSync(const uint8_t*,uint8_t type,uint16_t,uint32_t,const uint8_t* data,uint16_t n,uint8_t,uint32_t){assert(scopeDepth==0);++sends;response.assign(data,data+n);responseType=type;return true;}
}
struct Node{bool directory;size_t size;std::vector<std::string> children;};
static std::map<std::string,Node> nodes={
    {"/",{true,0,{"/stt","/hello.txt"}}}, {"/sd",{true,0,{"/stt"}}},
    {"/stt",{true,0,{"/stt/u2"}}}, {"/stt/u2",{true,0,{"/stt/u2/secret.txt"}}},
    {"/stt/u2/secret.txt",{false,900,{}}}, {"/hello.txt",{false,42,{}}},
    {"/ordinary",{true,0,{"/ordinary/note.txt"}}}, {"/ordinary/note.txt",{false,19,{}}},
    {"/sd/public.txt",{false,75,{}}}, {"/stt2/a.txt",{false,6,{}}}, {"/STT/a.txt",{false,7,{}}},
    // Device-private files: a wrongly served request would actually list or transfer these.
    {"/system",{true,0,{"/system/users","/system/certs"}}},
    {"/system/users",{true,0,{"/system/users/users.json","/system/users/user_settings"}}},
    {"/system/users/users.json",{false,512,{}}}, {"/system/users/user_settings",{true,0,{}}},
    {"/system/certs",{true,0,{"/system/certs/x.key"}}}, {"/system/certs/x.key",{false,1700,{}}}
};
class File{
    std::string path_;Node* node_=nullptr;size_t cursor_=0;
public:
    File()=default;explicit File(const String& path):path_(path.c_str()){auto it=nodes.find(path_);if(it!=nodes.end())node_=&it->second;}
    explicit operator bool()const{return node_;}
    bool isDirectory()const{return node_&&node_->directory;}
    size_t size()const{return node_?node_->size:0;}
    time_t getLastWrite()const{assert(false && "ESP-NOW listings must not request modification times");return 0;}
    const char* name()const{return path_.c_str();}
    void close(){node_=nullptr;}
    File openNextFile(){assert(scopeDepth);++fsCalls;if(!node_||cursor_>=node_->children.size())return {};return File(node_->children[cursor_++]);}
};
bool normalizeFsPath(const String&,String&);
namespace VFS{
enum StorageType{INTERNAL,SDCARD};
struct VirtualEntry{const char* name;bool isFolder;};
static std::vector<StorageType> requestedTiers;
bool existsGuarded(const String& path,const AuthContext&){assert(scopeDepth);++fsCalls;return identityMayRead()&&nodes.count(path.c_str());}
File openGuarded(const String& path,const char*,const AuthContext&){assert(scopeDepth);++fsCalls;return (openFails||!identityMayRead())?File():File(path);}
size_t listVirtualEntries(const String&,VirtualEntry*,size_t){assert(scopeDepth);++fsCalls;return 0;}
bool isSDAvailable(){assert(scopeDepth);++fsCalls;++sdChecks;return sdAvailable;}
bool getStats(StorageType tier,uint64_t& total,uint64_t& used,uint64_t& free){assert(scopeDepth);++fsCalls;++statsCalls;requestedTiers.push_back(tier);total=tier==INTERNAL?1000:2000;used=tier==INTERNAL?400:1400;free=600;return statsOkay;}
// INSERT_VFS
}
uint8_t getPermissions(const String&,const AuthContext&){assert(scopeDepth);++fsCalls;return identityMayRead()?0x3f:0;}
String formatPath(const char* dir,const char* name){return String(dir)+(String(dir)=="/"?"":"/")+name;}
// Reads under the installed identity (canRead), so a refused identity transfers nothing.
bool sendFileToMac(const uint8_t*,const String& path){assert(scopeDepth);if(!identityMayRead())return false;++transfers;transferPath=path;return true;}
// INSERT_PRODUCTION
static void reset(){assert(scopeDepth==0);scopes=fsCalls=transfers=statsCalls=sdChecks=sends=0;response.clear();transferPath="";VFS::requestedTiers.clear();filesystemReady=sdAvailable=statsOkay=true;allocFails=openFails=false;bondConfigured=bondSession=true;installed.clear();lastInstalled=AuthContext{};}
static const uint8_t mac[6]={1,2,3,4,5,6};        // the bonded peer (equals bondMac)
static const uint8_t otherMac[6]={9,8,7,6,5,4};   // a securely paired peer that is not the bonded peer
template<class Req>Req request(const char* path){Req r={};r.reqId=0x87654321;strlcpy(r.path,path,sizeof(r.path));return r;}
template<class Reply>Reply decoded(uint8_t type){assert(responseType==type&&response.size()>=sizeof(Reply));Reply r;std::memcpy(&r,response.data(),sizeof(r));return r;}
// ---- Who the handlers serve -------------------------------------------------
// A request the module must refuse: the reply is PERM_DENIED with the reqId and path echoed and every
// payload field zero, and no identity was installed, no VFS call made and nothing transferred.
static unsigned requesterChecks=0,requesterFailures=0;
static const char* opName(unsigned op){return op==0?"LIST":op==1?"STAT":"GET";}
static void refused(unsigned op,const uint8_t* who,const char* label,const char* path,bool configured,bool session){
    reset();bondConfigured=configured;bondSession=session;
    uint8_t status=0xff;bool echoed=false,zeroed=false;
    if(op==0){auto q=request<V4PayloadFsListReq>(path);q.maxEntries=32;processListDeferred(who,q);auto r=decoded<V4PayloadFsListReplyHeader>(ESPNOW_V4_TYPE_FS_LIST_REPLY);status=r.status;echoed=r.reqId==q.reqId&&!std::strcmp(r.path,q.path);zeroed=r.entryCount==0&&r.totalEntries==0&&r.hasMore==0&&r.nextStartIndex==0&&response.size()==sizeof(r);}
    else if(op==1){auto q=request<V4PayloadFsStatReq>(path);processStatDeferred(who,q);auto r=decoded<V4PayloadFsStatReply>(ESPNOW_V4_TYPE_FS_STAT_REPLY);status=r.status;echoed=r.reqId==q.reqId&&!std::strcmp(r.path,q.path);zeroed=r.totalBytes==0&&r.usedBytes==0&&r.freeBytes==0&&r.percentUsedX10==0;}
    else{auto q=request<V4PayloadFsGetReq>(path);processGetDeferred(who,q);auto r=decoded<V4PayloadFsGetAck>(ESPNOW_V4_TYPE_FS_GET_ACK);status=r.status;echoed=r.reqId==q.reqId&&!std::strcmp(r.path,q.path);zeroed=r.fileSize==0;}
    const bool okay=sends==1&&status==FS_LIST_STATUS_PERM_DENIED&&echoed&&zeroed&&scopes==0&&fsCalls==0&&transfers==0&&installed.empty();
    ++requesterChecks;
    if(!okay){++requesterFailures;std::fprintf(stderr,"FAIL %s '%s' from %s: expected perm_denied with no identity and no transfer, got status=%u scopes=%u fsCalls=%u transfers=%u\n",opName(op),path,label,unsigned(status),scopes,fsCalls,transfers);}
}
static void refusedRequester(const uint8_t* who,const char* label,bool configured,bool session){
    for(const char* path:{"/system/users/users.json","/system/certs/x.key","/system/users/user_settings","/","/hello.txt","/sd"})
        for(unsigned op=0;op<3;++op)refused(op,who,label,path,configured,session);
}
#if ENABLE_BONDED_MODE
static unsigned checks=0;
static unsigned statRoutingChecks=0,statRoutingFailures=0;
static void statRouting(const char* path, VFS::StorageType expected, bool mounted, bool readable) {
    reset();
    sdAvailable = mounted;
    statsOkay = readable;
    auto q = request<V4PayloadFsStatReq>(path);
    processStatDeferred(mac, q);
    auto r = decoded<V4PayloadFsStatReply>(ESPNOW_V4_TYPE_FS_STAT_REPLY);
    const bool missing = expected == VFS::SDCARD && !mounted;
    const uint8_t status = missing ? FS_LIST_STATUS_NOT_FOUND
        : readable ? FS_LIST_STATUS_OK : FS_LIST_STATUS_IO_ERROR;
    const unsigned queryCount = missing ? 0u : 1u;
    bool okay = sends == 1 && response.size() == sizeof(r) && r.reqId == q.reqId
        && !std::strcmp(r.path, q.path) && r.status == status && r.reserved2 == 0;
    for (uint8_t reserved : r.reserved1) okay &= reserved == 0;
    okay &= scopes == 1 && scopeDepth == 0 && transfers == 0
        && sdChecks == (expected == VFS::SDCARD ? 1u : 0u);
    okay &= statsCalls == queryCount && VFS::requestedTiers.size() == queryCount;
    if (queryCount && !VFS::requestedTiers.empty())
        okay &= VFS::requestedTiers.front() == expected;
    if (status == FS_LIST_STATUS_OK) {
        okay &= r.totalBytes == (expected == VFS::INTERNAL ? 1000u : 2000u)
            && r.usedBytes == (expected == VFS::INTERNAL ? 400u : 1400u)
            && r.freeBytes == 600 && r.percentUsedX10 == (expected == VFS::INTERNAL ? 400 : 700);
    } else {
        okay &= r.totalBytes == 0 && r.usedBytes == 0 && r.freeBytes == 0 && r.percentUsedX10 == 0;
    }
    ++statRoutingChecks;
    if (!okay) {
        ++statRoutingFailures;
        const char* queriedTier = VFS::requestedTiers.empty() ? "none"
            : VFS::requestedTiers.front() == VFS::INTERNAL ? "internal" : "sd";
        std::fprintf(stderr, "FAIL STAT path='%s' mounted=%d readable=%d: "
                     "expected tier=%s status=%u, got tier=%s status=%u total=%llu\n",
                     path, mounted, readable, expected == VFS::INTERNAL ? "internal" : "sd",
                     unsigned(status), queriedTier, unsigned(r.status),
                     static_cast<unsigned long long>(r.totalBytes));
    }
}
static void denied(const char* path,uint8_t status=FS_LIST_STATUS_PERM_DENIED){
    for(unsigned op=0;op<3;++op){reset();filesystemReady=status!=FS_LIST_STATUS_NOT_READY;
        if(op==0){auto q=request<V4PayloadFsListReq>(path);q.maxEntries=32;processListDeferred(mac,q);auto r=decoded<V4PayloadFsListReplyHeader>(ESPNOW_V4_TYPE_FS_LIST_REPLY);assert(r.reqId==q.reqId&&!std::strcmp(r.path,q.path));assert(r.status==status&&r.entryCount==0&&r.totalEntries==0&&r.hasMore==0&&r.nextStartIndex==0);assert(response.size()==sizeof(r));}
        if(op==1){auto q=request<V4PayloadFsStatReq>(path);processStatDeferred(mac,q);auto r=decoded<V4PayloadFsStatReply>(ESPNOW_V4_TYPE_FS_STAT_REPLY);assert(r.reqId==q.reqId&&!std::strcmp(r.path,q.path));assert(r.status==status&&r.totalBytes==0&&r.usedBytes==0&&r.freeBytes==0&&r.percentUsedX10==0);}
        if(op==2){auto q=request<V4PayloadFsGetReq>(path);processGetDeferred(mac,q);auto r=decoded<V4PayloadFsGetAck>(ESPNOW_V4_TYPE_FS_GET_ACK);assert(r.reqId==q.reqId&&!std::strcmp(r.path,q.path));assert(r.status==status&&r.fileSize==0);}
        assert(sends==1&&scopes==0&&fsCalls==0&&transfers==0);++checks;
    }
}
// The bonded peer with a live session is served under the bond identity (never SYSTEM), with the
// stamp the bonded command path uses; the SUPER-while-bonded rule is the boundary, so it still reads
// /system (the documented decision in System_Filesystem.cpp resolveRole), which is recorded here, not hidden.
static unsigned servedChecks=0,servedFailures=0;
static void servedFail(const char* what){++servedFailures;std::fprintf(stderr,"FAIL served identity: %s\n",what);}
static bool onlyBondIdentity(){if(installed.empty())return false;for(const auto& id:installed)if(id!="bond-admin@espnow")return false;return true;}
static std::string installedList(){std::string s;for(const auto& id:installed){if(!s.empty())s+=",";s+=id;}return s.empty()?"none":s;}
static void servedIdentity(){
    reset();auto g=request<V4PayloadFsGetReq>("/hello.txt");processGetDeferred(mac,g);auto ga=decoded<V4PayloadFsGetAck>(ESPNOW_V4_TYPE_FS_GET_ACK);
    ++servedChecks;if(!(ga.status==FS_LIST_STATUS_OK&&ga.fileSize==42&&transfers==1&&transferPath=="/hello.txt"&&scopes==2))servedFail("GET '/hello.txt' from the bonded peer: expected OK with one transfer under two identity installs");
    ++servedChecks;if(!onlyBondIdentity()){char msg[200];std::snprintf(msg,sizeof(msg),"expected bond-admin over SOURCE_ESPNOW, got %s",installedList().c_str());servedFail(msg);}
    ++servedChecks;if(!(lastInstalled.path=="/espnow/fs"&&lastInstalled.ip=="espnow:01:02:03:04:05:06"&&lastInstalled.scope==""&&lastInstalled.sid==""&&lastInstalled.opaque==nullptr))servedFail("expected the bond identity stamped path=/espnow/fs ip=espnow:<MAC>, unconfined");
    reset();auto l=request<V4PayloadFsListReq>("/");l.maxEntries=32;processListDeferred(mac,l);auto lr=decoded<V4PayloadFsListReplyHeader>(ESPNOW_V4_TYPE_FS_LIST_REPLY);
    ++servedChecks;if(!(lr.status==FS_LIST_STATUS_OK&&lr.entryCount>0&&scopes==1&&onlyBondIdentity()))servedFail("LIST '/' from the bonded peer: expected OK under the bond identity");
    reset();auto s=request<V4PayloadFsStatReq>("/");processStatDeferred(mac,s);auto sr=decoded<V4PayloadFsStatReply>(ESPNOW_V4_TYPE_FS_STAT_REPLY);
    ++servedChecks;if(!(sr.status==FS_LIST_STATUS_OK&&statsCalls==1&&scopes==1&&onlyBondIdentity()))servedFail("STAT '/' from the bonded peer: expected OK under the bond identity");
    reset();g=request<V4PayloadFsGetReq>("/system/users/users.json");processGetDeferred(mac,g);ga=decoded<V4PayloadFsGetAck>(ESPNOW_V4_TYPE_FS_GET_ACK);
    ++servedChecks;if(!(ga.status==FS_LIST_STATUS_OK&&transfers==1&&transferPath=="/system/users/users.json"&&onlyBondIdentity()))servedFail("GET '/system/users/users.json' from the bonded peer: expected OK under the bond identity (SUPER while bonded)");
}
#endif // ENABLE_BONDED_MODE
int main(){
#if ENABLE_BONDED_MODE
    for(const char* path:{"/stt","/stt/","/stt/u2","/stt/u2/secret.txt","/stt/u3/secret.txt","/stt/u02/a","/stt/U2/a","/stt/u4294967295/a","/stt/u4294967296/a","stt/u2/a"," //./stt//u2/a ","/sd/stt","/sd/./stt/u2/a","//sd//stt/u2/a","/sd/STT/u2/a","/sd/stt./u2/a","/sd/stt /u2/a","/sd/stt/u2~1/a","/sd/stt/U2/a","/sd/\\stt/u2/a","/sd/.\\stt/u2/a","/sd/stt\\u2\\a","/sd/stt/u2\\a","/ordinary/../stt/u2/a","/sd/stt/u2/../u3/a","/sd/stt/u2/a\n",
        // QuickNote files live inside the private transcript tree.
        "/stt/u2/notes","/stt/u2/notes/20261004T153012Z-6f1c9a2e00000007.txt","/sd/stt/u2/notes/boot-0000000000000001.txt","/sd/STT/u2/notes/boot-0000000000000001.txt"})denied(path);
    denied("/stt/u2/secret.txt",FS_LIST_STATUS_NOT_READY);denied("/hello.txt",FS_LIST_STATUS_NOT_READY);
    // Root LIST may reveal the protected root's name, never its descendants.
    for(const char* path:{"/","/sd","/ordinary"}){reset();auto q=request<V4PayloadFsListReq>(path);q.maxEntries=32;processListDeferred(mac,q);auto r=decoded<V4PayloadFsListReplyHeader>(ESPNOW_V4_TYPE_FS_LIST_REPLY);assert(r.status==FS_LIST_STATUS_OK&&r.entryCount>0&&scopes==1&&fsCalls>0&&transfers==0);V4PayloadFsEntry first;std::memcpy(&first,response.data()+sizeof(r),sizeof(first));assert(!std::strcmp(first.name,path==String("/ordinary")?"note.txt":"stt"));++checks;}
    for(const char* path:{"/hello.txt","/ordinary/note.txt","/sd/public.txt","/stt2/a.txt","/STT/a.txt"}){reset();auto q=request<V4PayloadFsGetReq>(path);processGetDeferred(mac,q);auto r=decoded<V4PayloadFsGetAck>(ESPNOW_V4_TYPE_FS_GET_ACK);assert(r.status==FS_LIST_STATUS_OK&&r.fileSize==nodes[path].size&&transfers==1&&transferPath==path&&scopes==2);++checks;}
    for(const char* path:{"/","/ordinary","/stt2","/STT"}){reset();auto q=request<V4PayloadFsStatReq>(path);processStatDeferred(mac,q);auto r=decoded<V4PayloadFsStatReply>(ESPNOW_V4_TYPE_FS_STAT_REPLY);assert(r.status==FS_LIST_STATUS_OK&&r.totalBytes==1000&&r.usedBytes==400&&r.freeBytes==600&&r.percentUsedX10==400&&statsCalls==1&&transfers==0);++checks;}
    reset();auto get=request<V4PayloadFsGetReq>("/missing");processGetDeferred(mac,get);assert(decoded<V4PayloadFsGetAck>(3).status==FS_LIST_STATUS_NOT_FOUND&&transfers==0);
    reset();get=request<V4PayloadFsGetReq>("/ordinary");processGetDeferred(mac,get);assert(decoded<V4PayloadFsGetAck>(3).status==FS_LIST_STATUS_NOT_A_DIR&&transfers==0);
    reset();openFails=true;get=request<V4PayloadFsGetReq>("/hello.txt");processGetDeferred(mac,get);assert(decoded<V4PayloadFsGetAck>(3).status==FS_LIST_STATUS_PERM_DENIED&&transfers==0);
    reset();auto list=request<V4PayloadFsListReq>("/hello.txt");processListDeferred(mac,list);assert(decoded<V4PayloadFsListReplyHeader>(1).status==FS_LIST_STATUS_NOT_A_DIR&&transfers==0);
    reset();allocFails=true;list=request<V4PayloadFsListReq>("/stt");processListDeferred(mac,list);assert(decoded<V4PayloadFsListReplyHeader>(1).status==FS_LIST_STATUS_NOT_READY&&scopes==0&&fsCalls==0);
    reset();sdAvailable=false;auto stat=request<V4PayloadFsStatReq>("/sd");processStatDeferred(mac,stat);assert(decoded<V4PayloadFsStatReply>(2).status==FS_LIST_STATUS_NOT_FOUND&&statsCalls==0);
    reset();statsOkay=false;stat=request<V4PayloadFsStatReq>("/");processStatDeferred(mac,stat);assert(decoded<V4PayloadFsStatReply>(2).status==FS_LIST_STATUS_IO_ERROR);
    assert(scopeDepth==0);std::printf("FS-RPC transcript privacy: %u path/operation checks plus ordinary/error behavior passed\n",checks);
    for(bool mounted:{false,true})for(bool readable:{false,true}){
        for(const char* path:{"/sd","/sd/","//sd//x","sd"," sd/x "})statRouting(path,VFS::SDCARD,mounted,readable);
        for(const char* path:{"/","/ordinary","/sdata","/sdcard","/sdcard/x","/SD"})statRouting(path,VFS::INTERNAL,mounted,readable);
    }
    std::printf("FS-RPC STAT routing: %u scenarios, %u failures\n",statRoutingChecks,statRoutingFailures);
    // Who the handlers serve: only the bonded peer, only while its bond session is live.
    refusedRequester(otherMac,"a non-bond peer",true,true);
    refusedRequester(mac,"the bonded peer without a live session",true,false);
    refusedRequester(mac,"the bond MAC with no bond configured",false,true);
    // Transcript privacy stays in front of the identity for every requester.
    for(unsigned op=0;op<3;++op)refused(op,otherMac,"a non-bond peer","/stt/u2/secret.txt",true,true);
    servedIdentity();
    std::printf("FS-RPC requester gate: %u refusals checked, %u failures; served identity: %u checks, %u failures\n",
                requesterChecks,requesterFailures,servedChecks,servedFailures);
    return (statRoutingFailures||requesterFailures||servedFailures)?1:0;
#else
    // Bond mode compiled out: the module has no requester it can serve, so every request is refused.
    refusedRequester(mac,"the bond MAC (bond mode compiled out)",true,true);
    refusedRequester(otherMac,"a non-bond peer (bond mode compiled out)",true,true);
    std::printf("FS-RPC requester gate (ENABLE_BONDED_MODE=0): %u refusals checked, %u failures\n",requesterChecks,requesterFailures);
    return requesterFailures?1:0;
#endif
}
