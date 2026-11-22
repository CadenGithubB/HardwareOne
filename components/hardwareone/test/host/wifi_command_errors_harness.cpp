#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>
class String {
 std::string text;
 public:
 String(const char* s=""):text(s?s:""){} String(std::string s):text(s){}
 const char* c_str()const{return text.c_str();} size_t length()const{return text.size();}
 int toInt()const{return std::atoi(text.c_str());}
 bool operator==(const char* s)const{return text==s;}
};
struct CommandArgs {
 std::vector<String> args;
 explicit CommandArgs(const String& text){std::istringstream in(text.c_str());std::string s;while(in>>s)args.emplace_back(s);}
 String arg(size_t n)const{return n<args.size()?args[n]:String();}
 size_t count()const{return args.size();} int argInt(size_t n,int fallback)const{return n<args.size()?arg(n).toInt():fallback;}
};
#define RETURN_VALID_IF_VALIDATE_CSTR() do {}while(0)
struct {bool wifiAutoReconnect=true;} gSettings;
bool initOk=true,connectOk=false,snapshotOk=true,supersedeOk=true;
int savedCount=0,initCalls=0,connectCalls=0;
const char* companionReason=nullptr;
char debugBuffer[1024];
bool ensureDebugBuffer(){return true;}
char* getDebugBuffer(){return debugBuffer;}
bool radioCompanionBlocksRadio(const char** reason){*reason=companionReason;return companionReason!=nullptr;}
bool ensureWifiInitialized(){++initCalls;return initOk;}
void clearRadioRestoreSnapshot(){}
std::vector<std::string> messages;
void broadcastOutput(const char* m){messages.emplace_back(m);}
bool sawMessage(const char* m){for(const auto& x:messages)if(x==m)return true;return false;}
bool staConnected=false;
struct {void setAutoReconnect(bool){} bool isConnected(){return staConnected;} String SSID(){return String("prev");}} WiFi;
enum class WifiConnectOrigin{COMMAND,BOOT,BACKGROUND};
bool consoleCancels=false,cancelDuringConnect=false;
int rollbackCalls=0;
volatile bool gWifiUserCancelled=false;
bool wifiConsoleCancelApplies(WifiConnectOrigin origin){return origin==WifiConnectOrigin::COMMAND&&consoleCancels;}
struct WifiConnectionTarget{};
struct WifiNetworkSnapshot{int count=0;};
uint32_t parseSavedNetworkId(const String& value){return static_cast<uint32_t>(value.toInt());}
bool wifiNetworkConnectionById(uint32_t,WifiConnectionTarget&){return savedCount>0;}
int wifiNetworkCount(){return savedCount;}
bool wifiNetworkSnapshot(WifiNetworkSnapshot& value){value.count=savedCount;return snapshotOk;}
bool wifiSupersedeBackgroundConnect(){return supersedeOk;}
static bool attempt(){if(cancelDuringConnect){gWifiUserCancelled=true;return false;}return connectOk;}
bool connectToBestWifiNetwork(){++connectCalls;return attempt();}
bool connectWifiTarget(const WifiConnectionTarget&,int,bool){return attempt();}
bool connectWifiIndex(int,int){return attempt();}
bool connectWifiSsid(const String&,int){++rollbackCalls;return false;}
// INSERT_COMMANDS
int main(){
 // No saved setting refuses a connect: a failed start names the companion,
 // else the start failure itself.
 initOk=false;companionReason="C6 restarting; retry shortly";
 assert(std::strstr(cmd_wificonnect(""),"C6 restarting"));
 companionReason=nullptr;
 assert(std::strstr(cmd_wificonnect(""),"radio could not start"));
 initOk=true;initCalls=connectCalls=0;
 assert(std::strstr(cmd_wificonnect(""),"No saved Wi-Fi networks. Add one in Wi-Fi settings."));
 assert(initCalls==1 && connectCalls==1); // Preserve STA re-arming even for an empty saved list.
 snapshotOk=false;assert(std::strstr(cmd_wificonnect(""),"list unavailable"));
 snapshotOk=true;savedCount=1;
 assert(std::strstr(cmd_wificonnect(""),"Check signal and password"));
 connectOk=true;assert(std::strstr(cmd_wificonnect(""),"Connected successfully"));
 supersedeOk=false;assert(std::strstr(cmd_wificonnect(""),"still stopping"));
 assert(std::strstr(cmd_wificonnect("--index 0"),"Usage:"));
 // Console cancel: the serial request gets the hint; a cancel is reported as a
 // cancel (not as a signal/password failure) and skips the rollback join.
 supersedeOk=true;connectOk=false;messages.clear();
 assert(std::strstr(cmd_wificonnect(""),"Check signal and password"));
 assert(!sawMessage("(Ctrl-C or Esc cancels)")); // a web request cannot cancel from the console
 consoleCancels=true;messages.clear();
 assert(std::strstr(cmd_wificonnect(""),"Check signal and password"));
 assert(sawMessage("(Ctrl-C or Esc cancels)"));
 cancelDuringConnect=true;messages.clear();
 const char* r=cmd_wificonnect("");
 assert(std::strstr(r,"cancelled") && !std::strstr(r,"Check signal") && !std::strstr(r,"Error:"));
 assert(!sawMessage("Failed to connect"));
 staConnected=true;rollbackCalls=0;
 r=cmd_wificonnect("--index 1");
 assert(std::strstr(r,"cancelled") && rollbackCalls==0);
 cancelDuringConnect=false;rollbackCalls=0;
 assert(std::strstr(cmd_wificonnect("--index 1"),"Check signal and password") && rollbackCalls==1);
 assert(!gWifiUserCancelled); // a stale cancel does not carry into the next request
}
