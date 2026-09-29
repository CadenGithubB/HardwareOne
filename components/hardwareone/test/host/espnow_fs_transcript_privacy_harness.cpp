#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
    void toLowerCase(){for(char& c:text_)c=char(std::tolower(static_cast<unsigned char>(c)));}
    void trim(){const auto a=text_.find_first_not_of(" \t\r\n"),b=text_.find_last_not_of(" \t\r\n");text_=a==std::string::npos?"":text_.substr(a,b-a+1);}
    String& operator+=(char c){text_+=c;return *this;}
    String& operator+=(const String& s){text_+=s.text_;return *this;}
    friend String operator+(const String& a,const String& b){return a.text_+b.text_;}
    friend bool operator==(const String& a,const String& b){return a.text_==b.text_;}
    friend bool operator!=(const String& a,const String& b){return !(a==b);}
};
constexpr unsigned ESPNOW_V4_MAX_PLAINTEXT=202;
constexpr uint8_t ESPNOW_V4_TYPE_FS_LIST_REPLY=1,ESPNOW_V4_TYPE_FS_STAT_REPLY=2,ESPNOW_V4_TYPE_FS_GET_ACK=3;
// INSERT_WIRE
struct AuthContext {};
static AuthContext auth;
static unsigned scopes=0,scopeDepth=0,fsCalls=0,transfers=0,statsCalls=0;
static bool filesystemReady=true,sdAvailable=true,statsOkay=true,allocFails=false,openFails=false;
static std::vector<uint8_t> response;
static uint8_t responseType;
static String transferPath;
struct IdentityScope {explicit IdentityScope(const char*){++scopes;++scopeDepth;}~IdentityScope(){--scopeDepth;}};
#define SYSTEM_IDENTITY_SCOPE(label) IdentityScope identity(label)
const AuthContext& currentAuthContext(){assert(scopeDepth);return auth;}
struct FsLockGuard{explicit FsLockGuard(const char*){}};
template<class... T>void logMessage(const char*,T...){ }
#define DEBUG_ESPNOWF(...) logMessage(__VA_ARGS__)
#define ERROR_MEMORYF(...) logMessage(__VA_ARGS__)
enum class AllocPref{PreferPSRAM};
void* ps_alloc(size_t n,AllocPref,const char*){return allocFails?nullptr:std::malloc(n);}
size_t strlcpy(char* dst,const char* src,size_t cap){size_t n=std::strlen(src);if(cap){size_t copy=std::min(n,cap-1);std::memcpy(dst,src,copy);dst[copy]=0;}return n;}
uint32_t generateMessageId(){return 1;}
namespace espnowtx{
bool sendAeadSync(const uint8_t*,uint8_t type,uint16_t,uint32_t,const uint8_t* data,uint16_t n,uint8_t,uint32_t){response.assign(data,data+n);responseType=type;return true;}
}
struct Node{bool directory;size_t size;std::vector<std::string> children;};
static std::map<std::string,Node> nodes={
    {"/",{true,0,{"/stt","/hello.txt"}}}, {"/sd",{true,0,{"/stt"}}},
    {"/stt",{true,0,{"/stt/u2"}}}, {"/stt/u2",{true,0,{"/stt/u2/secret.txt"}}},
    {"/stt/u2/secret.txt",{false,900,{}}}, {"/hello.txt",{false,42,{}}},
    {"/ordinary",{true,0,{"/ordinary/note.txt"}}}, {"/ordinary/note.txt",{false,19,{}}},
    {"/sd/public.txt",{false,75,{}}}, {"/stt2/a.txt",{false,6,{}}}, {"/STT/a.txt",{false,7,{}}}
};
class File{
    std::string path_;Node* node_=nullptr;size_t cursor_=0;
public:
    File()=default;explicit File(const String& path):path_(path.c_str()){auto it=nodes.find(path_);if(it!=nodes.end())node_=&it->second;}
    explicit operator bool()const{return node_;}
    bool isDirectory()const{return node_&&node_->directory;}
    size_t size()const{return node_?node_->size:0;}
    const char* name()const{return path_.c_str();}
    void close(){node_=nullptr;}
    File openNextFile(){assert(scopeDepth);++fsCalls;if(!node_||cursor_>=node_->children.size())return {};return File(node_->children[cursor_++]);}
};
bool normalizeFsPath(const String&,String&);
namespace VFS{
enum StorageType{INTERNAL,SDCARD};
struct VirtualEntry{const char* name;bool isFolder;};
String normalize(const String& raw){String out;normalizeFsPath(raw,out);return out;}
bool existsGuarded(const String& path,const AuthContext&){assert(scopeDepth);++fsCalls;return nodes.count(path.c_str());}
File openGuarded(const String& path,const char*,const AuthContext&){assert(scopeDepth);++fsCalls;return openFails?File():File(path);}
String stripSdPrefix(const String& path){if(path=="/sd")return "/";return path.startsWith("/sd/")?path.substring(3):path;}
size_t listVirtualEntries(const String&,VirtualEntry*,size_t){assert(scopeDepth);++fsCalls;return 0;}
bool isSDAvailable(){assert(scopeDepth);++fsCalls;return sdAvailable;}
bool getStats(StorageType,uint64_t& total,uint64_t& used,uint64_t& free){assert(scopeDepth);++fsCalls;++statsCalls;total=1000;used=400;free=600;return statsOkay;}
}
uint8_t getPermissions(const String&,const AuthContext&){assert(scopeDepth);++fsCalls;return 0x3f;}
String formatPath(const char* dir,const char* name){return String(dir)+(String(dir)=="/"?"":"/")+name;}
bool sendFileToMac(const uint8_t*,const String& path){assert(scopeDepth);++transfers;transferPath=path;return true;}
// INSERT_PRODUCTION
static void reset(){assert(scopeDepth==0);scopes=fsCalls=transfers=statsCalls=0;response.clear();transferPath="";filesystemReady=sdAvailable=statsOkay=true;allocFails=openFails=false;}
static const uint8_t mac[6]={1,2,3,4,5,6};
template<class Req>Req request(const char* path){Req r={};r.reqId=0x87654321;strlcpy(r.path,path,sizeof(r.path));return r;}
template<class Reply>Reply decoded(uint8_t type){assert(responseType==type&&response.size()>=sizeof(Reply));Reply r;std::memcpy(&r,response.data(),sizeof(r));return r;}
static unsigned checks=0;
static void denied(const char* path,uint8_t status=FS_LIST_STATUS_PERM_DENIED){
    for(unsigned op=0;op<3;++op){reset();filesystemReady=status!=FS_LIST_STATUS_NOT_READY;
        if(op==0){auto q=request<V4PayloadFsListReq>(path);q.maxEntries=32;processListDeferred(mac,q);auto r=decoded<V4PayloadFsListReplyHeader>(ESPNOW_V4_TYPE_FS_LIST_REPLY);assert(r.reqId==q.reqId&&!std::strcmp(r.path,q.path));assert(r.status==status&&r.entryCount==0&&r.totalEntries==0&&r.hasMore==0&&r.nextStartIndex==0);assert(response.size()==sizeof(r));}
        if(op==1){auto q=request<V4PayloadFsStatReq>(path);processStatDeferred(mac,q);auto r=decoded<V4PayloadFsStatReply>(ESPNOW_V4_TYPE_FS_STAT_REPLY);assert(r.reqId==q.reqId&&!std::strcmp(r.path,q.path));assert(r.status==status&&r.totalBytes==0&&r.usedBytes==0&&r.freeBytes==0&&r.percentUsedX10==0);}
        if(op==2){auto q=request<V4PayloadFsGetReq>(path);processGetDeferred(mac,q);auto r=decoded<V4PayloadFsGetAck>(ESPNOW_V4_TYPE_FS_GET_ACK);assert(r.reqId==q.reqId&&!std::strcmp(r.path,q.path));assert(r.status==status&&r.fileSize==0);}
        assert(scopes==0&&fsCalls==0&&transfers==0);++checks;
    }
}
int main(){
    for(const char* path:{"/stt","/stt/","/stt/u2","/stt/u2/secret.txt","/stt/u3/secret.txt","/stt/u02/a","/stt/U2/a","/stt/u4294967295/a","/stt/u4294967296/a","stt/u2/a"," //./stt//u2/a ","/sd/stt","/sd/./stt/u2/a","//sd//stt/u2/a","/sd/STT/u2/a","/sd/stt./u2/a","/sd/stt /u2/a","/sd/stt/u2~1/a","/sd/stt/U2/a","/sd/\\stt/u2/a","/sd/.\\stt/u2/a","/sd/stt\\u2\\a","/sd/stt/u2\\a","/ordinary/../stt/u2/a","/sd/stt/u2/../u3/a","/sd/stt/u2/a\n"})denied(path);
    denied("/stt/u2/secret.txt",FS_LIST_STATUS_NOT_READY);denied("/hello.txt",FS_LIST_STATUS_NOT_READY);
    // Root LIST may reveal the protected root's name, never its descendants.
    for(const char* path:{"/","/sd","/ordinary"}){reset();auto q=request<V4PayloadFsListReq>(path);q.maxEntries=32;processListDeferred(mac,q);auto r=decoded<V4PayloadFsListReplyHeader>(ESPNOW_V4_TYPE_FS_LIST_REPLY);assert(r.status==FS_LIST_STATUS_OK&&r.entryCount>0&&scopes==1&&fsCalls>0&&transfers==0);V4PayloadFsEntry first;std::memcpy(&first,response.data()+sizeof(r),sizeof(first));assert(!std::strcmp(first.name,path==String("/ordinary")?"note.txt":"stt"));++checks;}
    for(const char* path:{"/hello.txt","/ordinary/note.txt","/sd/public.txt","/stt2/a.txt","/STT/a.txt"}){reset();auto q=request<V4PayloadFsGetReq>(path);processGetDeferred(mac,q);auto r=decoded<V4PayloadFsGetAck>(ESPNOW_V4_TYPE_FS_GET_ACK);assert(r.status==FS_LIST_STATUS_OK&&r.fileSize==nodes[path].size&&transfers==1&&transferPath==path&&scopes==2);++checks;}
    for(const char* path:{"/","/sd","/ordinary","/stt2","/STT"}){reset();auto q=request<V4PayloadFsStatReq>(path);processStatDeferred(mac,q);auto r=decoded<V4PayloadFsStatReply>(ESPNOW_V4_TYPE_FS_STAT_REPLY);assert(r.status==FS_LIST_STATUS_OK&&r.totalBytes==1000&&r.usedBytes==400&&r.freeBytes==600&&r.percentUsedX10==400&&statsCalls==1&&transfers==0);++checks;}
    reset();auto get=request<V4PayloadFsGetReq>("/missing");processGetDeferred(mac,get);assert(decoded<V4PayloadFsGetAck>(3).status==FS_LIST_STATUS_NOT_FOUND&&transfers==0);
    reset();get=request<V4PayloadFsGetReq>("/ordinary");processGetDeferred(mac,get);assert(decoded<V4PayloadFsGetAck>(3).status==FS_LIST_STATUS_NOT_A_DIR&&transfers==0);
    reset();openFails=true;get=request<V4PayloadFsGetReq>("/hello.txt");processGetDeferred(mac,get);assert(decoded<V4PayloadFsGetAck>(3).status==FS_LIST_STATUS_PERM_DENIED&&transfers==0);
    reset();auto list=request<V4PayloadFsListReq>("/hello.txt");processListDeferred(mac,list);assert(decoded<V4PayloadFsListReplyHeader>(1).status==FS_LIST_STATUS_NOT_A_DIR&&transfers==0);
    reset();allocFails=true;list=request<V4PayloadFsListReq>("/stt");processListDeferred(mac,list);assert(decoded<V4PayloadFsListReplyHeader>(1).status==FS_LIST_STATUS_NOT_READY&&scopes==0&&fsCalls==0);
    reset();sdAvailable=false;auto stat=request<V4PayloadFsStatReq>("/sd");processStatDeferred(mac,stat);assert(decoded<V4PayloadFsStatReply>(2).status==FS_LIST_STATUS_NOT_FOUND&&statsCalls==0);
    reset();statsOkay=false;stat=request<V4PayloadFsStatReq>("/");processStatDeferred(mac,stat);assert(decoded<V4PayloadFsStatReply>(2).status==FS_LIST_STATUS_IO_ERROR);
    assert(scopeDepth==0);std::printf("FS-RPC transcript privacy: %u path/operation checks plus ordinary/error behavior passed\n",checks);
}
