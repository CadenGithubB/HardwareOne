#define ARDUINOJSON_ENABLE_ARDUINO_STRING 1
#define ARDUINOJSON_ENABLE_ARDUINO_STREAM 0
#define ARDUINOJSON_ENABLE_ARDUINO_PRINT 0
#define ARDUINOJSON_ENABLE_PROGMEM 0
#include <Arduino.h>
#include <ArduinoJson.h>
#include "Transcription_UI_Policy.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
#define ENABLE_LOCAL_STT 1
#define DICTATION_MAX_TEXT 256
#define EXT_RAM_BSS_ATTR
#define RETURN_VALID_IF_VALIDATE_CSTR() do {} while(0)
#define pdMS_TO_TICKS(ms) (ms)
using TransportSessionEpoch=uint32_t;
constexpr TransportSessionEpoch kNoTransportSessionEpoch=0;
enum CommandSource {SOURCE_WEB,SOURCE_SERIAL,SOURCE_INTERNAL,SOURCE_LOCAL_DISPLAY,SOURCE_G2_GLASSES};
// INSERT_TYPES
// INSERT_PARSER
// INSERT_NORMALIZER
void webBatchTestStringAppend(){}
struct AuthContext {CommandSource transport=SOURCE_WEB;String user="alice";uint32_t epoch=7;};
static AuthContext auth;
static bool epochLive=true,guest=false,accountExists=true,storageBusy=false,sdAvailable=true;
static unsigned fsDepth=0,readCalls=0,beginCalls=0,stopCalls=0,cancelCalls=0,commitCalls=0;
static bool shortRead=false,seekFails=false,allowBegin=true;
static std::function<void()> readHook,peekHook,snapshotHook;
static std::vector<std::string> openedPaths;
static struct {bool sttSaveTranscripts=true; String micSource="auto";} gSettings;
enum AudioSource {AUDIO_SRC_NONE, AUDIO_SRC_LOCAL_PDM, AUDIO_SRC_G2_LEFT};
static bool micPdmAvailable=true, micG2Available=false;
static bool audioSourceAvailable(AudioSource source){return source==AUDIO_SRC_LOCAL_PDM?micPdmAvailable:source==AUDIO_SRC_G2_LEFT&&micG2Available;}
static const AuthContext& currentAuthContext(){return auth;}
static uint32_t captureTransportSessionEpoch(const AuthContext& c){return c.epoch;}
static bool transportSessionEpochIsLive(CommandSource s,uint32_t e){return epochLive&&s==auth.transport&&e==auth.epoch&&e!=0;}
static bool getUserAuthorizationRole(const String& user,String& role){assert(user==auth.user);role=guest?"guest":"user";return accountExists;}
static bool getUserIdByUsername(const char* user,uint32_t& id){assert(fsDepth);assert(std::string(user)==auth.user.c_str());id=accountExists?7:0;return accountExists;}
struct FsLockGuard {bool held=false;FsLockGuard(const char*,uint32_t){if(!storageBusy&&!fsDepth){held=true;++fsDepth;}}~FsLockGuard(){if(held)--fsDepth;}};
static bool isFsLockedByCurrentTask(){return fsDepth!=0;}
struct Node {std::string path,data;bool directory=false,denied=false;std::vector<std::shared_ptr<Node>> children;};
static std::map<std::string,std::shared_ptr<Node>> nodes;
static bool canRead(const char* path,const AuthContext&){assert(fsDepth);auto i=nodes.find(path);return i!=nodes.end()&&!i->second->denied;}
class File {
 std::shared_ptr<Node> node_;size_t pos_=0,next_=0;
public:
 File()=default;explicit File(std::shared_ptr<Node> n):node_(std::move(n)){}
 explicit operator bool() const{return bool(node_);}
 bool isDirectory()const{return node_&&node_->directory;}
 const char* name()const{return node_?node_->path.c_str():"";}
 size_t size()const{return node_?node_->data.size():0;}
 File openNextFile(){assert(fsDepth);return node_&&next_<node_->children.size()?File(node_->children[next_++]):File();}
 bool seek(size_t offset){assert(fsDepth);if(seekFails||!node_||offset>size())return false;pos_=offset;return true;}
 size_t read(uint8_t* out,size_t wanted){assert(fsDepth&&node_);++readCalls;auto got=std::min(wanted,size()-pos_);if(shortRead&&got)--got;memcpy(out,node_->data.data()+pos_,got);pos_+=got;if(readHook){auto hook=readHook;readHook=nullptr;hook();}return got;}
 void close(){node_.reset();}
};
namespace VFS {
static bool isSDAvailable(){return sdAvailable;}
static bool existsGuarded(const char* path,const AuthContext& ctx){return canRead(path,ctx);}
static File openGuarded(const String& path,const char* mode,const AuthContext& ctx){assert(fsDepth&&std::string(mode)=="r");openedPaths.emplace_back(path.c_str());return canRead(path.c_str(),ctx)?File(nodes.at(path.c_str())):File();}
}
static DictationAppLease current{SOURCE_WEB,7,0x1122334455667788ULL};
static bool haveLease=true,active=true,haveText=true;
static DictationTextReceipt committed{};
static STTSpeakerSnapshot speakerSnapshot;
static bool owns(const DictationAppLease& l){return haveLease&&l.source==current.source&&l.epoch==current.epoch&&l.exchange==current.exchange&&l.purpose==current.purpose&&transportSessionEpochIsLive(l.source,l.epoch);}
static unsigned currentLookups=0;
static bool dictationAppCurrent(CommandSource source,uint32_t epoch,DictationAppLease* out,DictationAppPurpose purpose=DictationAppPurpose::Transcription){++currentLookups;assert(purpose==DictationAppPurpose::Transcription);*out={};if(!haveLease||source!=current.source||epoch!=current.epoch||purpose!=current.purpose||!epochLive)return false;*out=current;return true;}
static bool dictationAvailable(const char** reason){*reason=nullptr;return true;}
static bool dictationAppSnapshot(const DictationAppLease& lease,DictationAppSnapshot* out){*out={};out->busy=active;if(!owns(lease))return false;out->valid=true;out->active=active;out->done=!active;out->status.continuous=true;out->status.captureActive=active;out->status.transcript.enabled=true;snprintf(out->status.transcript.path,sizeof(out->status.transcript.path),"/stt/u7/private.txt");if(snapshotHook){auto hook=snapshotHook;snapshotHook=nullptr;hook();}return true;}
static bool dictationAppBegin(CommandSource source,uint32_t epoch,DictationAppLease* out){++beginCalls;if(!allowBegin)return false;current={source,epoch,0x1122334455667788ULL};*out=current;haveLease=true;return true;}
static bool dictationAppRequestStop(const DictationAppLease& lease){++stopCalls;return owns(lease);}
static bool dictationAppCancel(const DictationAppLease& lease){++cancelCalls;return owns(lease);}
static bool dictationAppPeekText(const DictationAppLease& lease,char* text,size_t size,DictationTextReceipt* receipt){if(!owns(lease)||!haveText)return false;snprintf(text,size,"private words");*receipt={lease.exchange,9,0,13};if(peekHook){auto hook=peekHook;peekHook=nullptr;hook();}return true;}
static std::string draftText; static uint32_t draftVersion=0;
static bool dictationAppPeekDraft(const DictationAppLease&,char* out,size_t size,uint32_t* version){if(out&&size)out[0]=0;if(version)*version=0;if(draftText.empty())return false;snprintf(out,size,"%s",draftText.c_str());if(version)*version=draftVersion;return true;}
// System_STTVocabulary fake: the display copy uppercases a configured word; a raw copy is never changed.
static bool formatBusy=false; static bool lastParagraphStart=false;
static bool sttFormatDisplay(const char* raw,bool paragraphStart,char* out,size_t cap){if(out&&cap)out[0]=0;if(formatBusy||!raw||!out||!cap)return false;lastParagraphStart=paragraphStart;std::string s(raw);size_t at=s.find("private");if(at!=std::string::npos)s.replace(at,7,"Private");if(paragraphStart&&!s.empty())s[0]=char(toupper(s[0]));if(s.size()>=cap)return false;snprintf(out,cap,"%s",s.c_str());return true;}
static bool dictationAppReadSpeakers(const DictationAppLease& lease,STTSpeakerSnapshot* out){*out={};if(!owns(lease))return false;*out=speakerSnapshot;return true;}
static bool dictationAppCommitText(const DictationAppLease& lease,const DictationTextReceipt& receipt,size_t accepted){++commitCalls;committed=receipt;return owns(lease)&&receipt.sequence==9&&receipt.offset==0&&accepted==13;}
// INSERT_ADAPTER
static std::shared_ptr<Node> add(const std::string& path,const std::string& data,bool dir=false){auto node=std::make_shared<Node>();node->path=path;node->data=data;node->directory=dir;nodes[path]=node;return node;}
static void reset(){speakerSnapshot={};auth={};epochLive=accountExists=sdAvailable=allowBegin=true;guest=storageBusy=shortRead=seekFails=false;readCalls=beginCalls=stopCalls=cancelCalls=commitCalls=currentLookups=0;readHook=peekHook=snapshotHook=nullptr;openedPaths.clear();nodes.clear();haveLease=active=haveText=true;current={SOURCE_WEB,7,0x1122334455667788ULL};assert(!fsDepth);gSettings.sttSaveTranscripts=true;}
static JsonDocument parsed(const char* result){JsonDocument doc;assert(!deserializeJson(doc,result));assert(!doc["transcriptPath"].isNull());return doc;}
static JsonDocument files(const String& args){auto doc=parsed(commandFiles(args));assert(!fsDepth);return doc;}
static JsonDocument session(const String& args){return parsed(commandSession(args));}
static void deniedFile(const char* args){auto d=files(args);assert(d["success"]==false&&!d.containsKey("sttText"));assert(openedPaths.empty());}
static void testFiles(){
 reset();auto dir=add("/stt/u7","",true);for(int i=0;i<11;++i){char p[64];snprintf(p,sizeof(p),"/stt/u7/session-%02d.txt",i);dir->children.push_back(add(p,"saved"));}
 dir->children.insert(dir->children.begin(),add("/stt/u7/hidden.bin","private"));dir->children.insert(dir->children.begin(),add("/stt/u7/subdir.txt","",true));
 // A QuickNote folder beside the transcripts (readable, owner's own) is not a transcript.
 auto notes=add("/stt/u7/notes","",true);notes->children.push_back(add("/stt/u7/notes/boot-0000000000000001.txt","note"));dir->children.insert(dir->children.begin(),notes);
 auto denied=add("/stt/u7/denied.txt","private");denied->denied=true;dir->children.insert(dir->children.begin(),denied);
 dir->children.insert(dir->children.begin(),add("/stt/u7/a\"b.txt","private"));dir->children.insert(dir->children.begin(),add("/stt/u7/"+std::string(70,'x')+".txt","private"));
 auto d=files("list internal 0");assert(d["success"]==true&&d["entries"].size()==8&&d["more"]==true&&d["nextOffset"]==8);
 assert(d["entries"][0]["name"]=="session-00.txt"&&d["entries"][7]["name"]=="session-07.txt");
 for(JsonObject row:d["entries"].as<JsonArray>())assert(row["name"].as<std::string>().find("notes")==std::string::npos&&row["name"].as<std::string>().find("boot-")==std::string::npos);
 d=files("list internal 8");assert(d["entries"].size()==3&&d["more"]==false&&d["nextOffset"]==11);
 d=files("list internal 999");assert(d["entries"].size()==0&&d["more"]==false);
 reset();d=files("list internal 0");assert(d["success"]==true&&d["entries"].size()==0);
 reset();sdAvailable=false;d=files("list sd 0");assert(d["success"]==false);
 for(const auto* path:{"read /stt/u7/private.txt 0","read \"/stt/u8/private.txt\" 0","read \"/stt/u70/private.txt\" 0","read \"/stt/u7/../u8/private.txt\" 0","read \"/users/alice.txt\" 0","read \"/stt/u7/private.txt 0"}){reset();deniedFile(path);}
 reset();add("/stt/u7/private.txt","private text");d=files("read \" //stt/./u7//private.txt \" 0");assert(d["sttText"]=="private text"&&d["eof"]==true&&d["nextOffset"]==12);assert(openedPaths[0]=="/stt/u7/private.txt");
 reset();add("/sd/stt/u7/private.txt","sd text");d=files("read \"/sd/stt/u7/private.txt\" 0");assert(d["sttText"]=="sd text");
 for(int width=2;width<=4;++width){reset();std::string unicode=width==2?"\xc3\xa9":width==3?"\xe2\x82\xac":"\xf0\x9f\x8e\x99";std::string text=std::string(511,'a')+unicode+"end";add("/stt/u7/private.txt",text);d=files("read \"/stt/u7/private.txt\" 0");assert(d["nextOffset"]==511&&!d["eof"].as<bool>());auto next=files("read \"/stt/u7/private.txt\" 511");assert(next["sttText"].as<std::string>()==unicode+"end"&&next["eof"]==true);assert(d["sttText"].as<std::string>()+next["sttText"].as<std::string>()==text);}
 reset();add("/stt/u7/private.txt","abc");d=files("read \"/stt/u7/private.txt\" 3");assert(d["eof"]==true&&d["sttText"]=="");d=files("read \"/stt/u7/private.txt\" 4");assert(d["success"]==false);
 reset();add("/stt/u7/private.txt","abc");shortRead=true;d=files("read \"/stt/u7/private.txt\" 0");assert(d["success"]==false&&!d.containsKey("sttText"));
 reset();add("/stt/u7/private.txt","abc");readHook=[] {epochLive=false;};d=files("read \"/stt/u7/private.txt\" 0");assert(d["success"]==false&&!d.containsKey("sttText")&&d["transcriptPath"]==true);
 for(int n=0;n<4;++n){reset();if(n==0)auth.user="";if(n==1)auth.epoch=0;if(n==2)epochLive=false;if(n==3)accountExists=false;deniedFile("read \"/stt/u7/private.txt\" 0");}
 reset();storageBusy=true;deniedFile("list internal 0");
}
static void testSession(){
 reset();auto d=session("status");assert(d["exchange"]=="1122334455667788"&&d["active"]==true&&d["transcriptPath"]=="/stt/u7/private.txt");
 reset();haveLease=false;d=session("status");assert(d["exchange"]==""&&d["busy"]==true&&d["transcriptPath"]=="");
 reset();d=session("start");assert(d["success"]==true&&beginCalls==1);
 reset();guest=true;d=session("start");assert(d["success"]==false&&beginCalls==0);
 reset();d=session("next 1122334455667788");assert(d["success"]==true&&d["sttText"]=="private words"&&d["sequence"]==9&&d["length"]==13);
 // The display copy rides beside the raw text; the receipt still names the raw bytes. Sequence 9 is not the session's first phrase.
 assert(d["display"]=="Private words"&&!lastParagraphStart&&d["length"]==13);
 draftText="private draft";draftVersion=4;d=session("next 1122334455667788");assert(d["draft"]=="Private draft"&&d["draftVersion"]==4);
 formatBusy=true;d=session("next 1122334455667788");assert(!d.containsKey("display")&&d["sttText"]=="private words"&&d["draft"]=="private draft");formatBusy=false;draftText.clear();
 reset();haveText=false;speakerSnapshot.available=true;speakerSnapshot.version=8;speakerSnapshot.count=2;
 speakerSnapshot.entries[0]={9,1,true};speakerSnapshot.entries[1]={10,-1,false};
 d=session("next 1122334455667788");assert(d["available"]==false&&d["speakerVersion"]==8&&d["speakersAvailable"]==true);
 assert(d["speakers"].size()==2&&d["speakers"][0]["sequence"]==9&&d["speakers"][0]["speaker"]==1&&d["speakers"][0]["ready"]==true);
 assert(d["speakers"][1]["speaker"]==-1&&d["speakers"][1]["ready"]==false);
 // The maximum bounded history must fit the private response.
 speakerSnapshot.count=STT_SPEAKER_HISTORY;for(auto& entry:speakerSnapshot.entries)entry={UINT32_MAX,127,true};
 d=session("next 1122334455667788");assert(d["success"]==true&&d["speakers"].size()==STT_SPEAKER_HISTORY);
 reset();peekHook=[] {epochLive=false;};d=session("next 1122334455667788");assert(d["success"]==false&&!d.containsKey("sttText"));
 reset();snapshotHook=[] {epochLive=false;};d=session("status");assert(d["success"]==false&&!d.containsKey("sttText")&&d["transcriptPath"]==true);
 reset();d=session("ack 1122334455667788 9 0 13");assert(d["success"]==true&&commitCalls==1&&committed.sequence==9&&committed.length==13);
 for(const auto* args:{"ack 1122334455667788 9 0 0","ack 1122334455667788 9 0 257","ack 1122334455667788 9 65536 13","ack 1122334455667788 4294967296 0 13","ack 1122334455667788 9 -1 13","next 9999999999999999","next 1","next 0000000000000000","next \"1122334455667788"}){reset();d=session(args);assert(d["success"]==false&&!d.containsKey("sttText")&&commitCalls==0);}
 reset();d=session("stop 1122334455667788");assert(d["success"]==true&&stopCalls==1);
 reset();d=session("cancel 1122334455667788");assert(d["success"]==true&&cancelCalls==1);
 // A QuickNote held by this same session is not a transcription: status with
 // no id does not adopt it, and its id cannot page, acknowledge or end it.
 reset();current.purpose=DictationAppPurpose::QuickNote;d=session("status");
 assert(currentLookups==1&&d["exchange"]==""&&d["busy"]==true&&d["active"]==false&&d["transcriptPath"]=="");
 for(const auto* args:{"next 1122334455667788","ack 1122334455667788 9 0 13","stop 1122334455667788","cancel 1122334455667788","status 1122334455667788"}){
  reset();current.purpose=DictationAppPurpose::QuickNote;d=session(args);
  assert(d["success"]==false&&!d.containsKey("sttText")&&commitCalls==0&&stopCalls==0&&cancelCalls==0);
 }
}
int main(){testFiles();testSession();puts("Transcription command adapter: real parser/normalizer/JSON; owner file paging, UTF-8 bounds, epoch fences and App receipts passed");}
