// X3 passphrase fixtures: the same text entered in the Android app and typed
// into the P4's `phone secret` serial command.
//
// Production code: Arduino String (trim, substring, equalsIgnoreCase), the
// command-name split in System_CommandLookupCore.h, phoneSecretCommand and
// blePassphrasePolicyError (Bluetooth.cpp), internal secret storage
// (System_Settings.cpp) and PBKDF2 (System_BleSecureCrypto.h). Two transport
// steps are modelled, with their sources:
//   - the serial reader drops every CR and ends the line at LF
//     (HardwareOne.cpp, loop() serial drain);
//   - the line reaches executeCommand() as a C string, so it ends at the first
//     NUL (System_Utils.cpp, submitAndExecuteSync -> executeCommand(c_str)).
// The "android" column is the agreed entry rule (DECISIONS Q6/A-7/A-9), stated
// here as the contract the P4 result is checked against, not Android code.
// Compile with -funsigned-char: the P4 is RISC-V, where plain char is unsigned.
#include "System_InternalSecretString.h"
#include "BLE_PhonePairCode.h"  // phoneSecretCommand stores a pairing code canonical (never an X3 text)
#include "System_CommandLookupCore.h"
#include "BLE_PhoneSecureChannel.h"
#include <cassert>
#include <cctype>
#include <cstdio>
#include <set>
#include <string>
#ifdef NDEBUG
#error "This generator requires assertions"
#endif
void* heap_caps_malloc(size_t n,unsigned flags){assert(flags==(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));return malloc(n);}
struct SettingEntry{};
static void finishSettingChange(void*,const SettingEntry*){}
bool setInternalSecretSetting(String&,const String&,const SettingEntry* =nullptr);
static struct{String phonePipeSecret,bleSecureChannelSecret;bool phonePipeSecretGenerated=false;}gSettings;
enum{SOURCE_SERIAL,SOURCE_BLUETOOTH,SOURCE_G2_GLASSES};
static struct{int transport=SOURCE_SERIAL;} auth;
static auto currentAuthContext(){return auth;}
// The BLE role phonePipeReplaceSecret holds from the stop through the store
// (Bluetooth.cpp; test_phone_secret checks it): always granted here.
enum class BleRoleTransition:uint8_t{PHONE_STOP};
class BleRoleTransitionClaim{public:explicit BleRoleTransitionClaim(BleRoleTransition){}explicit operator bool()const{return true;}};
static bool phonePipeStop(){return true;}
static bool phonePipeInvalidateSecret(){return true;}
static bool blePeerClearPhonePipeEnrollment(){return true;}
static bool requestSettingsPersist(){return true;}
static bool gCLIValidateOnly=false;
#define RETURN_VALID_IF_VALIDATE_CSTR() do{if(::gCLIValidateOnly)return "VALID";}while(0)
// PRODUCTION

static std::string hex(const std::string& s) {
  static const char digits[]="0123456789abcdef";std::string out;
  for(unsigned char c:s){out+=digits[c>>4];out+=digits[c&15];}
  return out;
}
static std::string escape(const std::string& s) {
  std::string out;
  for(unsigned char c:s) {
    if(c=='"'||c=='\\'){out+='\\';out+=char(c);}
    else if(c<0x20){char b[8];snprintf(b,sizeof(b),"\\u%04x",c);out+=b;}
    else out+=char(c);
  }
  return out;
}

// Agreed Android entry rule: no NUL/CR/LF, strict UTF-8, trim outer ASCII
// 09-0D/20 only, 10-128 bytes, ASCII upper, lower, digit and punctuation.
struct Entry {bool accepts;std::string reason,secret;};
static bool asciiPunct(unsigned char c){return (c>=0x21&&c<=0x2f)||(c>=0x3a&&c<=0x40)||(c>=0x5b&&c<=0x60)||(c>=0x7b&&c<=0x7e);}
static Entry androidRule(const std::string& text,bool validUtf8) {
  if(!validUtf8)return {false,"not valid UTF-8 text",""};
  for(char c:text)if(c==0||c=='\r'||c=='\n')return {false,"contains NUL, CR or LF",""};
  size_t begin=0,end=text.size();
  auto outer=[](unsigned char c){return c==0x20||(c>=0x09&&c<=0x0d);};
  while(begin<end&&outer(text[begin]))++begin;
  while(end>begin&&outer(text[end-1]))--end;
  const std::string s=text.substr(begin,end-begin);
  if(s.size()<10)return {false,"fewer than 10 bytes after trimming",""};
  if(s.size()>128)return {false,"more than 128 bytes after trimming",""};
  bool up=false,lo=false,di=false,pu=false;
  for(unsigned char c:s){up|=c>='A'&&c<='Z';lo|=c>='a'&&c<='z';di|=c>='0'&&c<='9';pu|=asciiPunct(c);}
  if(!up)return {false,"no ASCII uppercase letter",""};
  if(!lo)return {false,"no ASCII lowercase letter",""};
  if(!di)return {false,"no ASCII digit",""};
  if(!pu)return {false,"no ASCII punctuation",""};
  return {true,"",s};
}

// Host C-locale ctype must classify exactly as the target's newlib C locale:
// ASCII only, nothing at 0x80-0xFF. Fail generation rather than emit vectors
// that depend on a desktop locale.
static void checkCtype() {
  for(int c=0;c<256;++c) {
    const bool space=c==0x20||(c>=0x09&&c<=0x0d);
    assert(!!isspace(c)==space && !!isspace(char(c))==space);
    assert(!!isupper(c)==(c>='A'&&c<='Z') && !!islower(c)==(c>='a'&&c<='z'));
    assert(!!isdigit(c)==(c>='0'&&c<='9') && !!ispunct(c)==asciiPunct(c));
  }
}

struct Vector {const char* id;std::string text;bool validUtf8;const char* description;};
static std::string repeat(const std::string& s,unsigned n){std::string r;while(n--)r+=s;return r;}

int main() {
  static_assert(char(0xff)>0,"compile with -funsigned-char to match the RISC-V target");
  checkCtype();
  const std::string base="SyntheticPipe!123";
  const std::string nfcE="\xc3\xa9",nfdE="e\xcc\x81";
  const std::vector<Vector> vectors={
    {"plain",base,true,"ASCII passphrase used by the session fixtures"},
    {"outer_spaces","  "+base+"  ",true,"outer ASCII spaces are trimmed"},
    {"outer_tabs","\t"+base+"\t",true,"outer HT is trimmed"},
    {"outer_vt_ff","\x0b"+base+"\x0c",true,"outer VT and FF are trimmed"},
    {"interior_space","Synthetic Pipe!123",true,"interior space is kept"},
    {"interior_tab","Synthetic\tPipe!123",true,"interior HT is kept"},
    {"outer_nbsp","\xc2\xa0"+base+"\xc2\xa0",true,"U+00A0 is not ASCII whitespace: kept on both sides"},
    {"outer_nel","\xc2\x85"+base,true,"U+0085 is not ASCII whitespace: kept"},
    {"outer_ideographic_space","\xe3\x80\x80"+base,true,"U+3000 is kept"},
    {"multibyte_letters","P\xc3\xa4ssw\xc3\xb6rt!123A",true,"non-ASCII bytes count toward length but not toward categories"},
    {"emoji","Pipe\xf0\x9f\x98\x80Secret!12",true,"4-byte UTF-8 sequence kept as is"},
    {"nfc_e_acute","Caf"+nfcE+"!Pipe123A",true,"precomposed U+00E9"},
    {"nfd_e_acute","Caf"+nfdE+"!Pipe123A",true,"e + U+0301: different bytes and PSK from the NFC form; no normalisation"},
    {"non_ascii_uppercase_only","\xc3\x84\xc3\x96\xc3\x9c!1234abcd",true,"Ä Ö Ü do not satisfy the uppercase rule"},
    {"unicode_punctuation_only","Synthetic\xc2\xa1Pipe123",true,"U+00A1 does not satisfy the punctuation rule"},
    {"min_10_bytes","Aa1!aaaaaa",true,"exactly 10 bytes"},
    {"short_9_bytes","Aa1!aaaaa",true,"9 bytes"},
    {"trimmed_to_10","   Aa1!aaaaaa   ",true,"10 bytes after trimming"},
    {"max_128_ascii","Aa1!"+repeat("x",124),true,"exactly 128 bytes"},
    {"over_129_ascii","Aa1!"+repeat("x",125),true,"129 bytes"},
    {"max_128_multibyte","Aa1!"+repeat(nfcE,62),true,"66 characters, 128 bytes"},
    {"over_129_multibyte","Aa1!"+repeat(nfcE,62)+"x",true,"67 characters, 129 bytes"},
    {"missing_digit","SyntheticPipe!abc",true,"no digit"},
    {"missing_punctuation","SyntheticPipe0123",true,"no punctuation"},
    {"interior_cr","Synthetic\rPipe!123",true,"Android rejects CR; the P4 serial reader drops it, giving different bytes"},
    {"trailing_cr",base+"\r",true,"Android rejects CR; the P4 serial reader drops it"},
    {"interior_nul",std::string("Synthetic\0Pipe!123",18),true,"Android rejects NUL; the P4 line ends at it"},
    {"clear_word","clear",true,"reserved: the P4 clears its secret"},
    {"clear_upper","CLEAR",true,"reserved word, any case"},
    {"empty","",true,"the P4 reports status"},
    {"only_spaces","     ",true,"empty after trimming: the P4 reports status"},
    {"invalid_utf8_byte","SyntheticPipe!12\xff",false,"not text: Android cannot produce it; the P4 stores raw bytes"},
  };
  const char* names[]={"phone","phone secret"};
  std::set<std::string> ids;
  for(const auto& v:vectors) {
    assert(ids.insert(v.id).second);
    const Entry android=androidRule(v.text,v.validUtf8);
    // Serial transport: the user types the command, the terminal sends CR LF.
    const std::string typed="phone secret "+v.text+"\r\n";
    std::string line;
    for(char c:typed){if(c=='\r')continue;if(c=='\n')break;line+=c;}
    // executeCommand(const char*): the command String stops at the first NUL.
    String command(line.c_str());
    command.trim();
    const auto match=hw1_command_lookup::resolve(command.c_str(),command.length(),2,
        [&](size_t i)->const char*{return names[i];});
    assert(match.found() && match.registryOrdinal==1 && match.lineOffset==0);
    String args=command.substring(match.matchedLength);args.trim();
    gSettings.phonePipeSecret=String("Previous!Secret1");
    const std::string message=phoneSecretCommand(args);
    std::string result="rejected";
    if(message.rfind("Pipe secret set",0)==0)result="set";
    else if(message.rfind("Pipe secret cleared",0)==0)result="cleared";
    else if(message.rfind("Phone pipe secret is",0)==0)result="status";
    const std::string stored(gSettings.phonePipeSecret.c_str(),gSettings.phonePipeSecret.length());
    std::string psk;
    if(result=="set") {
      uint8_t key[32];
      assert(hw1sc::scPbkdf2(reinterpret_cast<const uint8_t*>(stored.data()),stored.size(),
          reinterpret_cast<const uint8_t*>(hw1phone::kPipeLabel),strlen(hw1phone::kPipeLabel),100000,key));
      psk=hex(std::string(reinterpret_cast<char*>(key),32));
    }
    std::string parity="android_rejects";
    if(android.accepts) {
      // Interoperability requirement: what Android accepts, the P4 stores as
      // the same bytes, so both derive the same PSK.
      assert(result=="set" && stored==android.secret);
      parity="same_secret";
    }
    printf("{\"id\":\"%s\",\"description\":\"%s\",\"text_hex\":\"%s\",\"text_bytes\":%zu,\"valid_utf8\":%s,"
           "\"android_accepts\":%s,\"android_reason\":\"%s\",\"android_secret_hex\":\"%s\","
           "\"serial_line_hex\":\"%s\",\"p4_result\":\"%s\",\"p4_message\":\"%s\",\"p4_stored_hex\":\"%s\","
           "\"p4_stored_bytes\":%zu,\"psk_hex\":\"%s\",\"parity\":\"%s\"}\n",
           v.id,v.description,hex(v.text).c_str(),v.text.size(),v.validUtf8?"true":"false",
           android.accepts?"true":"false",android.reason.c_str(),hex(android.secret).c_str(),hex(typed).c_str(),
           result.c_str(),escape(message).c_str(),result=="set"?hex(stored).c_str():"",
           result=="set"?stored.size():size_t(0),psk.c_str(),parity.c_str());
  }
  return 0;
}
