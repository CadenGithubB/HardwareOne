// Per-role file permission matrix, decided once by the production path-rule
// table and checked through both ways in: the command table (serial, BLE, web
// CLI, the Files page's mkdir/rmdir/create/delete/rename) and the direct web
// file endpoints. test_filesystem_listing_permissions_contract.py inserts the
// real System_Filesystem.cpp permission code and the requiresAdmin column of
// every file command row.
#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
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
static int taskObject;
static TaskHandle_t task=&taskObject,fsOwner=nullptr;
TaskHandle_t xTaskGetCurrentTaskHandle(){return task;}
bool isFsLockedByCurrentTask(){return task && fsOwner==task;}
struct FsLockGuard {
    bool held=false;
    explicit FsLockGuard(const char*) {
        if(!task || isFsLockedByCurrentTask())return;
        assert(!fsOwner);fsOwner=task;held=true;
    }
    ~FsLockGuard(){if(held){assert(fsOwner==task);fsOwner=nullptr;}}
};
struct Account {uint32_t id;String role;};
static std::map<std::string,Account> accounts={
    {"alice",{2,"user"}}, {"admin",{4,"admin"}}, {"guest",{5,"guest"}}};
const String kBondAdminUser="bond-admin";
bool isSuperAdminUser(const String&){return false;}
bool getUserRoleAndSuper(const String& user,String& role,bool& super) {
    auto found=accounts.find(user.c_str());
    if(found==accounts.end())return false;
    role=found->second.role;super=false;return true;
}
bool getUserIdByUsername(const String& user,uint32_t& id) {
    auto found=accounts.find(user.c_str());id=0;
    if(found==accounts.end())return false;
    id=found->second.id;return true;
}
// INSERT_INTERNAL
// INSERT_PRODUCTION

struct CommandGate {const char* name;bool requiresAdmin;};
static const CommandGate kCommandGates[]={
// INSERT_COMMAND_GATES
};

enum class Op {Read,Write,Create,Delete};
static const char* opName(Op op) {
    switch(op){case Op::Read:return "read";case Op::Write:return "write";
               case Op::Create:return "create";default:return "delete";}
}
static bool isAdminRole(const char* who){return std::string(who)=="admin";}
static bool requiresAdmin(const char* command) {
    for(const auto& gate:kCommandGates) if(std::strcmp(gate.name,command)==0) return gate.requiresAdmin;
    std::fprintf(stderr,"missing file command row: %s\n",command);std::abort();
}
static bool tableAllows(Op op,const char* raw,const AuthContext& ctx) {
    String path;
    if(!normalizeFsPath(raw,path))return false;
    switch(op){
        case Op::Read:return canRead(path,ctx);
        case Op::Write:return canEdit(path,ctx);
        case Op::Create:return canCreate(path,ctx);
        default:return canDelete(path,ctx);
    }
}
// Every command that performs `op`. The dispatcher refuses a requiresAdmin row
// for a non-admin before the handler runs; the handler then asks the table.
static bool commandAllows(Op op,const char* raw,const char* who) {
    static const char* const kRead[]={"files","fileread","fileview"};
    static const char* const kWrite[]={"filewrite"};
    static const char* const kCreate[]={"filecreate","mkdir"};
    static const char* const kDelete[]={"filedelete","rmdir"};
    const char* const* names=kRead;size_t count=3;
    if(op==Op::Write){names=kWrite;count=1;}
    if(op==Op::Create){names=kCreate;count=2;}
    if(op==Op::Delete){names=kDelete;count=2;}
    const AuthContext ctx=[&]{AuthContext c;c.user=who;return c;}();
    bool first=true,result=false;
    for(size_t i=0;i<count;++i){
        const bool allowed=(!requiresAdmin(names[i]) || isAdminRole(who)) && tableAllows(op,raw,ctx);
        if(first){result=allowed;first=false;}
        else if(allowed!=result){
            std::fprintf(stderr,"commands for %s disagree on %s for %s\n",opName(op),raw,who);std::abort();
        }
    }
    (void)requiresAdmin("filerename");
    return result;
}
// The direct web endpoints (/api/files/list, read, view, write, upload) refuse
// an admin-only path for a non-admin, then ask the table. /api/files/create
// adds the same refusal in front of the filecreate/mkdir command; the Files
// page sends delete through /api/cli, i.e. the command alone.
static bool webAllows(Op op,const char* raw,const char* who) {
    const bool preCheck=!isAdminOnlyPath(raw) || isAdminRole(who);
    AuthContext ctx;ctx.user=who;
    switch(op){
        case Op::Read:
        case Op::Write:return preCheck && tableAllows(op,raw,ctx);
        case Op::Create:return preCheck && commandAllows(op,raw,who);
        default:return commandAllows(op,raw,who);
    }
}

static int failures=0;
static void expect(const char* who,const char* path,bool r,bool w,bool c,bool d) {
    const bool expected[]={r,w,c,d};
    const Op ops[]={Op::Read,Op::Write,Op::Create,Op::Delete};
    for(int i=0;i<4;++i){
        const bool viaCommand=commandAllows(ops[i],path,who);
        const bool viaWeb=webAllows(ops[i],path,who);
        if(viaCommand!=expected[i] || viaWeb!=expected[i]){
            std::printf("MISMATCH %s %s %s: expected=%d command=%d web=%d\n",
                        who,opName(ops[i]),path,int(expected[i]),int(viaCommand),int(viaWeb));
            ++failures;
        }
    }
    // The web pre-check is derived from the same table: it may only refuse
    // what the table already refuses a user.
    if(isAdminOnlyPath(path)){
        AuthContext ctx;ctx.user="alice";
        for(Op op:ops) if(tableAllows(op,path,ctx)){
            std::printf("MISMATCH admin-only %s grants user %s\n",path,opName(op));++failures;
        }
    }
}

int main() {
    //       who      path                                   read   write  create delete
    expect("alice", "/notes/todo.txt",                      true,  true,  true,  true);
    expect("alice", "/recordings/take.wav",                 true,  true,  true,  true);
    expect("alice", "/espnow/peer.txt",                     true,  true,  false, true);
    expect("alice", "/battery.csv",                         true,  false, false, false);
    expect("alice", "/battery.csv.1",                       true,  false, false, false);
    expect("alice", "/battery.csv.header.tmp",              true,  false, false, false);
    expect("alice", "/system/settings.json",                false, false, false, false);
    expect("alice", "/logging_captures/sensors/s.csv",      false, false, false, false);
    expect("admin", "/notes/todo.txt",                      true,  true,  true,  true);
    expect("admin", "/battery.csv",                         true,  false, false, true);
    expect("admin", "/battery.csv.2",                       true,  false, false, true);
    expect("admin", "/system/settings.json",                true,  false, false, false);
    expect("admin", "/logging_captures/sensors/s.csv",      true,  true,  true,  true);
    // The battery logger writes, stages and rotates as the system identity.
    AuthContext system;system.transport=SOURCE_INTERNAL;system.user="system";
    for(const char* p:{"/battery.csv","/battery.csv.1","/battery.csv.header.tmp"}){
        String path;assert(normalizeFsPath(p,path));
        assert(canRead(path,system) && canEdit(path,system) && canCreate(path,system) &&
               canDelete(path,system) && canRename(path,system));
    }
    // Neither the file listing of the root nor its rename path lets a user
    // move the log aside and put another file in its place.
    {
        AuthContext alice;alice.user="alice";String path;
        assert(normalizeFsPath("/battery.csv",path));
        if(canRename(path,alice)){std::puts("MISMATCH alice rename /battery.csv");++failures;}
    }
    assert(!fsOwner);
    if(failures){std::printf("filesystem role matrix: %d mismatches\n",failures);return 1;}
    std::puts("filesystem role matrix: command table and web endpoints agree with the path rules");
    return 0;
}
