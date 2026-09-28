#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <deque>
#include <set>
#include <string>
using String=std::string;
using esp_err_t=int;
constexpr int ESP_OK=0, GLOBAL_VOICE_CMD_ID_START=990;
#define INFO_SRF(...) ((void)0)
#define DEBUG_SRF(...) ((void)0)
#define WARN_SYSTEMF(...) ((void)0)
struct esp_mn_phrase_t { int command_id; char* string; };
struct esp_mn_error_t { int num; };
struct Phrase {
 String storage; esp_mn_phrase_t phrase;
 Phrase(int id,const char* text):storage(text),phrase{id,storage.data()} {}
};
static std::deque<Phrase> phrases;
static unsigned duplicateAdds=0;
static bool clearFails=false,updateFails=false,ready=true,lockAvailable=true;
static String addFails;
static std::set<String> disabled;
void* findCommand(const String& name) { return disabled.count(name)?nullptr:reinterpret_cast<void*>(1); }
String normalizePhrase(const char* text) {
 String value=text?text:"";
 value.erase(0,value.find_first_not_of(" \t\r\n"));
 auto end=value.find_last_not_of(" \t\r\n");
 if(end!=String::npos) value.erase(end+1);
 std::transform(value.begin(),value.end(),value.begin(),[](unsigned char c){return std::tolower(c);});
 return value;
}
bool mnCommandsReady() { return ready; }
bool lockMN(uint32_t) { return lockAvailable; }
void unlockMN() {}
void srReportDeadVoiceRoutes() {}
int esp_mn_commands_clear() { if(clearFails)return -1; phrases.clear();return ESP_OK; }
esp_mn_phrase_t* esp_mn_commands_get_from_index(int index) {
 return index>=0 && static_cast<size_t>(index)<phrases.size()?&phrases[index].phrase:nullptr;
}
char* esp_mn_commands_get_string(int id) {
 for(auto& p:phrases) if(p.phrase.command_id==id)return p.phrase.string;
 return nullptr;
}
int esp_mn_commands_add(int id,const char* text) {
 if(addFails==text)return -1;
 // Match vendor semantics: duplicate additions silently REPLACE the command ID.
 for(auto& p:phrases)if(p.storage==text){++duplicateAdds;p.phrase.command_id=id;return ESP_OK;}
 phrases.emplace_back(id,text);return ESP_OK;
}
bool mnUpdateLocked() { return !updateFails; }
// INSERT_TABLES
// INSERT_FUNCTIONS
static void assertUniqueIdsAndPhrases() {
 std::set<int> ids;std::set<String> texts;
 for(auto& p:phrases){assert(ids.insert(p.phrase.command_id).second);assert(texts.insert(normalizePhrase(p.phrase.string)).second);}
 assert(duplicateAdds==0 && gVoiceCliMappingCount==phrases.size());
}
static void testGrammar() {
 assert(loadCategories());assert(phrases.size()==10);assertUniqueIdsAndPhrases();
 assert(findLoadedVoicePhrase("SYSTEM")->command_id==1);
 assert(!strcmp(findCliCommandForId(1),"system"));
 assert(findLoadedVoicePhrase("wifi")->command_id==3);
 assert(loadSubCategoriesForCategory("sensor"));assert(phrases.size()==14);assertUniqueIdsAndPhrases();
 assert(findLoadedVoicePhrase("motion sensor")->command_id==1);
 assert(findLoadedVoicePhrase("camera"));
 assert(loadTargetsForCategory("system"));assert(phrases.size()==6);assertUniqueIdsAndPhrases();
 assert(!strcmp(findCliCommandForId(findLoadedVoicePhrase("reboot")->command_id),"reboot"));
 assert(loadTargetsForCategorySubCategory("sensor","camera"));assert(phrases.size()==7);assertUniqueIdsAndPhrases();
 assert(!strcmp(findCliCommandForId(findLoadedVoicePhrase("take picture")->command_id),"cameracapture"));
 assert(!loadTargetsForCategory("sensor")); // Never merge ambiguous subcategory targets.
 disabled={"wifistatus","radiopower","wifiscan"};
 assert(loadCategories());assert(!findLoadedVoicePhrase("wifi"));assertUniqueIdsAndPhrases();disabled.clear();
}
static void testCanonicalPhraseCopy() {
 assert(loadCategories());
 const int voiceId=findLoadedVoicePhrase("voice")->command_id;
 char owned[128];assert(copyRecognizedVoicePhrase(voiceId,owned,sizeof(owned)));
 assert(!strcmp(owned,"voice")); // MN7 result.string may be " VuS", never used here.
 assert(loadTargetsForCategory("voice"));assert(!strcmp(owned,"voice")); // Callback can replace table.
 const int closeId=findLoadedVoicePhrase("close")->command_id;
 assert(copyRecognizedVoicePhrase(closeId,owned,sizeof(owned)) && !strcmp(owned,"close"));
 assert(!strcmp(findCliCommandForId(closeId),"closesr")); // Display phrase is distinct from CLI command.
 assert(!copyRecognizedVoicePhrase(987654,owned,sizeof(owned)));
 assert(!copyRecognizedVoicePhrase(closeId,owned,3));
 auto* target=findLoadedVoicePhrase("close");
 assert(loadedTargetMatches(target,"closesr"));assert(!loadedTargetMatches(target,"reboot"));
}
static void testFailures() {
 clearFails=true;assert(!loadCategories());assert(!loadSubCategoriesForCategory("sensor"));
 assert(!loadTargetsForCategory("system"));assert(!loadTargetsForCategorySubCategory("sensor","camera"));clearFails=false;
 for(const char* phrase:{"cancel","system"}){addFails=phrase;assert(!loadCategories());}addFails.clear();
 addFails="motion sensor";assert(!loadSubCategoriesForCategory("sensor"));
 addFails="reboot";assert(!loadTargetsForCategory("system"));
 addFails="take picture";assert(!loadTargetsForCategorySubCategory("sensor","camera"));addFails.clear();
 updateFails=true;assert(!loadCategories());assert(!loadSubCategoriesForCategory("sensor"));
 assert(!loadTargetsForCategory("system"));assert(!loadTargetsForCategorySubCategory("sensor","camera"));updateFails=false;
 ready=false;assert(!loadCategories());ready=true;lockAvailable=false;assert(!loadCategories());lockAvailable=true;
 assert(loadCategories());assertUniqueIdsAndPhrases();
}
int main(){testGrammar();testCanonicalPhraseCopy();testFailures();puts("ESP-SR grammar deduplication, canonical phrase ownership and failure tests passed");}
