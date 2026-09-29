#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#define ENABLE_BONDED_MODE 1

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
enum CommandSource {SOURCE_INTERNAL,SOURCE_SERIAL,SOURCE_ESPNOW};
struct AuthContext {CommandSource transport=SOURCE_SERIAL;String user,path,scope;};
using TaskHandle_t=void*;
static int taskObject,otherTaskObject;
static TaskHandle_t task=&taskObject,fsOwner=nullptr;
static bool failLock=false,bondLive=false;
static unsigned idReads=0,roleReads=0;
TaskHandle_t xTaskGetCurrentTaskHandle(){return task;}
bool isFsLockedByCurrentTask(){return task && fsOwner==task;}
struct FsLockGuard {
    bool held=false;
    explicit FsLockGuard(const char*) {
        if(!task || failLock || isFsLockedByCurrentTask())return;
        assert(!fsOwner);fsOwner=task;held=true;
    }
    ~FsLockGuard(){if(held){assert(fsOwner==task);fsOwner=nullptr;}}
};
struct Account {uint32_t id;String role;bool banned=false;};
static std::map<std::string,Account> accounts={
    {"Alice",{2,"user"}}, {"alice",{3,"user"}}, {"admin",{4,"admin"}},
    {"guest",{5,"guest"}}, {"super",{1,"superadmin"}}, {"banned",{6,"user",true}},
    {"large",{UINT32_MAX,"user"}}};
const String kBondAdminUser="bond-admin";
bool isSuperAdminUser(const String& name){return name==kBondAdminUser && bondLive;}
bool getUserRoleAndSuper(const String& user,String& role,bool& super) {
    assert(isFsLockedByCurrentTask());++roleReads;
    auto found=accounts.find(user.c_str());
    if(found==accounts.end() || found->second.banned)return false;
    role=found->second.role;super=role=="superadmin";return true;
}
bool getUserIdByUsername(const String& user,uint32_t& id) {
    FsLockGuard guard("fake roster id");assert(isFsLockedByCurrentTask());++idReads;id=0;
    auto found=accounts.find(user.c_str());if(found==accounts.end())return false;
    id=found->second.id;return id!=0;
}
// INSERT_INTERNAL
// INSERT_PRODUCTION

static AuthContext actor(const char* name){AuthContext ctx;ctx.user=name;return ctx;}
static void expect(const char* raw,const AuthContext& ctx,uint8_t expected) {
    // Match Guarded VFS: normalize before each real canX boundary. Metadata
    // normalizes internally; all six operations must agree with the listing.
    String path;
    if(!normalizeFsPath(raw,path)){assert(getPermissions(raw,ctx)==0);return;}
    assert(getPermissions(raw,ctx)==expected);
    assert(canRead(path,ctx)==bool(expected&PERM_READ));
    assert(canEdit(path,ctx)==bool(expected&PERM_WRITE));
    assert(canDelete(path,ctx)==bool(expected&PERM_DELETE));
    assert(canRename(path,ctx)==bool(expected&PERM_RENAME));
    assert(canCreate(path,ctx)==bool(expected&PERM_CREATE));
    assert(canImport(path,ctx)==bool(expected&PERM_IMPORT));
    FsInternal::LockedListingPermissions view(ctx);
    assert(view.ready());assert(view.forPath(raw)==expected);
}
static void matrix() {
    auto alice=actor("Alice"),lower=actor("alice"),admin=actor("admin"),guest=actor("guest"),super=actor("super");
    AuthContext system=actor("system");system.transport=SOURCE_INTERNAL;
    for(const char* root:{"/stt","/sd/stt"}) {
        for(const auto& who:{alice,lower,admin,guest})expect(root,who,PERM_READ);
        for(const auto& who:{super,system})expect(root,who,PERM_ALL);
        const std::string own=std::string(root)+"/u2/session.txt",other=std::string(root)+"/u3/session.txt";
        expect(own.c_str(),alice,PERM_ALL);expect(other.c_str(),alice,0);
        expect(other.c_str(),lower,PERM_ALL);expect(own.c_str(),lower,0);
        expect(own.c_str(),admin,0);expect((std::string(root)+"/u4").c_str(),admin,PERM_ALL);
        expect((std::string(root)+"/u5/note.txt").c_str(),guest,PERM_READ);expect(own.c_str(),guest,0);
        for(const auto& who:{super,system}){expect(own.c_str(),who,PERM_ALL);expect(other.c_str(),who,PERM_ALL);}
        for(const char* name:{"", "missing", "banned", "AuthBypass", "system"}) {
            expect(root,actor(name),0);expect(own.c_str(),actor(name),0);
        }
        expect((std::string(root)+"/u4294967295/a.txt").c_str(),actor("large"),PERM_ALL);
        for(const char* bad:{"u0","u02","U2","u2.","u2 ","u2~1","u4294967296","u-2","u2x","other","u2\\file"})
            expect((std::string(root)+"/"+bad+"/a.txt").c_str(),alice,0);
        expect((std::string(root)+"/u2/../u3/a.txt").c_str(),alice,0);
        expect((std::string(root)+"/u2/a\"b.txt").c_str(),alice,0);
        expect((std::string(root)+"/u2/file.bin").c_str(),alice,PERM_ALL&~(PERM_READ|PERM_WRITE));
        expect((std::string(root)+"/u2/pic.jpg").c_str(),alice,PERM_ALL&~PERM_WRITE);
        AuthContext confined=system;confined.scope=String(root)+"/u2";
        expect(own.c_str(),confined,PERM_ALL);expect(other.c_str(),confined,0);
        FsInternal::LockedListingPermissions view(alice);
        assert(view.forChildOf(root)==0); // root cannot create arbitrary owners
        assert(view.forChildOf((std::string(root)+"/u2").c_str())==PERM_ALL);
    }
    // SD aliases must fail closed, including backslashes FatFs treats as path
    // separators. Non-transcript trees retain their previous permissions.
    for(const char* alias:{"/sd/STT/u2/a.txt","/sd/stt./u2/a.txt","/sd/stt /u2/a.txt",
                          "/sd/stt... /u2/a.txt","/sd/stt\\u2/a.txt","/sd/\\stt/u2/a.txt",
                          "/sd/.\\stt/u2/a.txt","/sd/stt/u2\\a.txt"})expect(alias,alice,0);
    expect(" /sd//./stt/u2/note.txt/ ",alice,PERM_ALL);
    expect("/stt2/u3/a.txt",alice,PERM_ALL);expect("/sd/stt2/u3/a.txt",alice,PERM_ALL);
    expect("/STT/u3/a.txt",alice,PERM_ALL); // distinct LittleFS spelling
    expect("/photos/x.txt",admin,PERM_ALL);expect("/system/settings.json",admin,PERM_READ);
    expect("/photos/x.txt",guest,PERM_READ);expect("/photos/x.txt",actor("AuthBypass"),PERM_ALL);
}
static void listingLifetime() {
    auto alice=actor("Alice");idReads=roleReads=0;
    {
        FsInternal::LockedListingPermissions view(alice);
        assert(roleReads==1 && idReads==0);
        assert(view.forPath("/photos/x.txt")==PERM_ALL && idReads==0);
        assert(view.forPath("/sd/STT/u2/a.txt")==0 && idReads==0);
        assert(view.forPath("/stt/u2/a.txt")==PERM_ALL && idReads==1);
        assert(view.forPath("/stt/u3/a.txt")==0 && idReads==1);
        assert(view.forPath("/sd/stt/u2/b.txt")==PERM_ALL && idReads==1);
        task=&otherTaskObject;assert(!view.ready() && view.forPath("/stt/u2/a.txt")==0);task=&taskObject;
    }
    // Recreated same username gets a new ID; old files do not transfer.
    accounts.at("Alice").id=8;expect("/stt/u2/a.txt",alice,0);expect("/stt/u8/a.txt",alice,PERM_ALL);
    accounts.at("Alice").id=2;
    failLock=true;{FsInternal::LockedListingPermissions view(alice);assert(!view.ready());assert(view.forPath("/stt/u2/a.txt")==0);}failLock=false;
    // Live bond retains existing super behavior; a revoked bond cannot reuse
    // a role cached when the listing view was created.
    AuthContext bond=actor("bond-admin");bond.transport=SOURCE_ESPNOW;
    bondLive=true;{
        FsInternal::LockedListingPermissions view(bond);assert(view.forPath("/stt/u2/a.txt")==PERM_ALL);
        bondLive=false;assert(view.forPath("/stt/u2/a.txt")==0);
    }
}
int main(){matrix();listingLifetime();assert(!fsOwner);std::puts("Transcript actual filesystem permission/listing tests passed");}
