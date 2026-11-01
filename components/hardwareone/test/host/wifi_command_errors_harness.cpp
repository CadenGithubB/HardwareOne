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
struct {bool wifiEnabled=true,wifiAutoReconnect=true;} gSettings;
bool initOk=true,connectOk=false,snapshotOk=true,supersedeOk=true;
int savedCount=0,initCalls=0,connectCalls=0;
const char* companionReason=nullptr;
char debugBuffer[1024];
bool ensureDebugBuffer(){return true;}
char* getDebugBuffer(){return debugBuffer;}
bool radioCompanionBlocksRadio(const char** reason){*reason=companionReason;return companionReason!=nullptr;}
bool ensureWiFiInitialized(){++initCalls;return initOk;}
void clearRadioRestoreSnapshot(){}
void broadcastOutput(const char*){}
struct {void setAutoReconnect(bool){} bool isConnected(){return false;} String SSID(){return String();}} WiFi;
struct WifiConnectionTarget{};
struct WifiNetworkSnapshot{int count=0;};
uint32_t parseSavedNetworkId(const String& value){return static_cast<uint32_t>(value.toInt());}
bool wifiNetworkConnectionById(uint32_t,WifiConnectionTarget&){return savedCount>0;}
int wifiNetworkCount(){return savedCount;}
bool wifiNetworkSnapshot(WifiNetworkSnapshot& value){value.count=savedCount;return snapshotOk;}
bool wifiSupersedeBackgroundConnect(){return supersedeOk;}
bool connectToBestWiFiNetwork(){++connectCalls;return connectOk;}
bool connectWiFiTarget(const WifiConnectionTarget&,int,bool){return connectOk;}
bool connectWiFiIndex(int,int){return connectOk;}
bool connectWiFiSSID(const String&,int){return false;}
// INSERT_COMMANDS
int main(){
 gSettings.wifiEnabled=false;initOk=false;
 assert(std::strstr(cmd_wificonnect(""),"Wi-Fi is disabled. Enable Wi-Fi in Settings."));
 gSettings.wifiEnabled=true;companionReason="C6 restarting; retry shortly";
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
}
