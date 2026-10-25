#!/usr/bin/env python3
"""Dedicated secret command and internal allocation with production Arduino String.

Phone link v2: a pairing code (BLE_PhonePairCode.h) in any spelling the code
accepts is stored canonical and clears the device-made mark, which the
`phone secret` steps (phonePipeReplaceSecret, shared with `phone pair new`)
persist together with the secret. Every other text keeps the unchanged policy
and messages (X3, test_phone_passphrase.py). The steps hold the BLE role
(PHONE_STOP, Bluetooth.cpp's per-task re-entrant token) from the pipe's stop
through the store and give it back before they return, so no link start can
take the old secret meanwhile; with the role busy they refuse before stopping.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
from test_web_batch_handlers import extract_block
from arduino_string_host import arduino_string
HERE=Path(__file__).resolve().parent;COMPONENT=HERE.parents[1]
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--sanitize',action='store_true');args=p.parse_args()
bt=(COMPONENT/'Bluetooth.cpp').read_text();settings=(COMPONENT/'System_Settings.cpp').read_text();utils=(COMPONENT/'System_Utils.cpp').read_text()
header=(COMPONENT/'Bluetooth.h').read_text()
# The role token: the shipped enum and claim over recording doubles of its begin and end.
role='\n'.join([extract_block(header,'enum class BleRoleTransition : uint8_t ')+';',
 'static bool roleBusy=false;static unsigned roleDepth=0,roleClaims=0,outsideRole=0;static int roleKind=-1;',
 'static bool bleRoleTransitionBegin(BleRoleTransition kind){if(roleBusy)return false;if(!roleDepth++)roleKind=int(kind);++roleClaims;return true;}',
 'static void bleRoleTransitionEnd(){assert(roleDepth);--roleDepth;}',
 extract_block(bt,'class BleRoleTransitionClaim')+';'])
functions='\n'.join([extract_block(utils,'void secureClearString('),extract_block(settings,'bool setInternalSecretSetting('),extract_block(bt,'static const char* blePassphrasePolicyError('),extract_block(bt,'const char* phonePipeReplaceSecret('),extract_block(bt,'const char* phoneSecretCommand(')])
unit=r'''
#include "System_InternalSecretString.h"
#include "BLE_PhonePairCode.h"
#include <cassert>
#include <cstdio>
#include <set>
// ROLE
static bool allocateOk=true;static int failAllocation=0;static std::set<const void*> internal;
void* heap_caps_malloc(size_t n,unsigned flags){
  assert(flags==(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));if(!allocateOk || (failAllocation && !--failAllocation))return nullptr;
  void* p=malloc(n);internal.insert(p);return p;}
struct SettingEntry{};
static unsigned saves=0;static bool savedGenerated=false;
static struct{String phonePipeSecret,bleSecureChannelSecret="Console!Secret123";bool phonePipeSecretGenerated=false;}gSettings;
// The secret's own persist: what the mark read when the secret went to disk.
static void finishSettingChange(void*,const SettingEntry*){++saves;savedGenerated=gSettings.phonePipeSecretGenerated;if(!roleDepth)++outsideRole;}
bool setInternalSecretSetting(String&,const String&,const SettingEntry* =nullptr);
enum{SOURCE_SERIAL,SOURCE_BLUETOOTH,SOURCE_G2_GLASSES};
static struct{int transport=SOURCE_SERIAL;} auth;
static auto currentAuthContext(){return auth;}
static bool stopOk=true,keyOk=true,enrollOk=true,persistOk=true;
static unsigned stops=0,invalidations=0,clears=0;
static bool phonePipeStop(){++stops;if(!roleDepth)++outsideRole;return stopOk;}
static bool phonePipeInvalidateSecret(){++invalidations;if(!roleDepth)++outsideRole;return keyOk;}
static bool blePeerClearPhonePipeEnrollment(){++clears;return enrollOk;}
static unsigned persists=0;
static bool requestSettingsPersist(){++persists;return persistOk;}
static bool gCLIValidateOnly=false;
#define RETURN_VALID_IF_VALIDATE_CSTR() do{if(::gCLIValidateOnly)return "VALID";}while(0)
// PRODUCTION
int main(){
  const String secret("SyntheticPipe!123LongEnoughToRequireAnAllocation");
  // The validate pass (automation checks) never stops the pipe or stores the secret.
  gCLIValidateOnly=true;assert(!strcmp(phoneSecretCommand(secret),"VALID")&&!stops&&!saves&&!gSettings.phonePipeSecret.length());gCLIValidateOnly=false;
  for(int source:{SOURCE_BLUETOOTH,SOURCE_G2_GLASSES}){auth.transport=source;assert(strstr(phoneSecretCommand(secret),"Error:"));assert(!stops&&!saves);}
  auth.transport=SOURCE_SERIAL;assert(!strcmp(phoneSecretCommand("weak"),"Error: pipe passphrase must be at least 10 characters"));assert(!stops);
  assert(!strncmp(phoneSecretCommand("synthetic!pipe123"),"Error: ",7));assert(!stops);
  stopOk=false;assert(strstr(phoneSecretCommand(secret),"unchanged")&&!invalidations&&!saves);stopOk=true;
  keyOk=false;assert(strstr(phoneSecretCommand(secret),"invalidation")&&!saves);keyOk=true;
  allocateOk=false;assert(strstr(phoneSecretCommand(secret),"storage")&&!saves);allocateOk=true;
  assert(strstr(phoneSecretCommand(secret),"Pipe secret set"));assert(gSettings.phonePipeSecret==secret&&internal.count(gSettings.phonePipeSecret.c_str()));
  assert(gSettings.bleSecureChannelSecret=="Console!Secret123"&&clears==1&&saves==1);
  assert(strstr(phoneSecretCommand(""),"SET")&&!strstr(phoneSecretCommand(""),secret.c_str()));
  const unsigned before=saves;assert(strstr(phoneSecretCommand(secret),"Pipe secret set")&&saves==before);
  enrollOk=false;assert(strstr(phoneSecretCommand("AnotherPipe!Secret123"),"cleanup busy"));enrollOk=true;
  persistOk=false;assert(strstr(phoneSecretCommand("AnotherPipe!Secret123"),"persistence failed"));persistOk=true;
  assert(strstr(phoneSecretCommand("clear"),"cleared")&&!gSettings.phonePipeSecret.length());
  assert(gSettings.bleSecureChannelSecret=="Console!Secret123");
  String large;for(int i=0;i<129;++i)large+='x';assert(strstr(phoneSecretCommand(large),"128"));
  // Phone link v2: a pairing code is stored canonical (any accepted spelling)
  // and is never device-made here: the mark is cleared, in the same persist.
  static const char* kCodeMsg="Pairing code set as the pipe secret (relay only: the console needs a code from phone pair new); enter the same code on Android, then openphone and phone pair";
  for(const char* spelling:{"0123-4567-89ab-cde4","  0123 4567 89AB CDE4\t","o123-4567-89ab-cde4","0I23-4567-89ab-cde4","0l23456789abcde4"}){
    gSettings.phonePipeSecretGenerated=true;gSettings.phonePipeSecret=String("Previous!Secret1");
    const unsigned stopsBefore=stops,clearsBefore=clears,persistsBefore=persists,savesBefore=saves;
    assert(!strcmp(phoneSecretCommand(spelling),kCodeMsg));
    assert(gSettings.phonePipeSecret=="0123456789ABCDE4"&&internal.count(gSettings.phonePipeSecret.c_str()));
    assert(!gSettings.phonePipeSecretGenerated&&saves==savesBefore+1&&!savedGenerated);
    assert(stops==stopsBefore+1&&clears==clearsBefore+1&&persists==persistsBefore+1);
  }
  // The query never shows it, and the result never repeats it.
  assert(!strstr(phoneSecretCommand(""),"0123")&&!strstr(kCodeMsg,"0123"));
  // A passphrase and a clear clear the mark as well.
  gSettings.phonePipeSecretGenerated=true;assert(strstr(phoneSecretCommand(secret),"Pipe secret set")&&!gSettings.phonePipeSecretGenerated&&!savedGenerated);
  gSettings.phonePipeSecretGenerated=true;assert(strstr(phoneSecretCommand("clear"),"cleared")&&!gSettings.phonePipeSecretGenerated);
  // A code-shaped text with a wrong check symbol is not a code: the unchanged
  // policy answers with its unchanged message, nothing stops.
  {const unsigned before=stops;
   assert(!strcmp(phoneSecretCommand("0123-4567-89AB-CDE5"),"Error: pipe passphrase must include a lowercase letter")&&stops==before);
   assert(!strcmp(phoneSecretCommand("0123-4567-89ab-cde5"),"Error: pipe passphrase must include an uppercase letter")&&stops==before);
   // Over 128 bytes is refused before the code parse, whatever it spells.
   String spaced("0123");for(int i=0;i<130;++i)spaced+=' ';spaced+="456789abcde4";
   assert(!strcmp(phoneSecretCommand(spaced),"Error: pipe passphrase must be at most 128 bytes")&&stops==before);}
  // The shared steps (`phone pair new` uses them): marked device-made in the
  // secret's own persist; a storage failure leaves secret and mark as they were.
  {gSettings.phonePipeSecretGenerated=false;bool replaced=false;
   assert(!phonePipeReplaceSecret("0123456789ABCDE4",16,true,&replaced)&&replaced);
   assert(gSettings.phonePipeSecret=="0123456789ABCDE4"&&gSettings.phonePipeSecretGenerated&&savedGenerated);
   allocateOk=false;replaced=true;
   assert(!strcmp(phonePipeReplaceSecret("ZZZZZZZZZZZZZZZZ",16,false,&replaced),"Error: internal secret storage unavailable")&&!replaced);
   assert(gSettings.phonePipeSecret=="0123456789ABCDE4"&&gSettings.phonePipeSecretGenerated);allocateOk=true;
   // The store itself fails (its own internal copy): secret and mark stay as they were.
   failAllocation=2;const unsigned savesBefore=saves;
   assert(!strcmp(phonePipeReplaceSecret("ZZZZZZZZZZZZZZZZ",16,false,&replaced),"Error: internal secret storage unavailable")&&!replaced);
   assert(!failAllocation&&saves==savesBefore&&gSettings.phonePipeSecret=="0123456789ABCDE4"&&gSettings.phonePipeSecretGenerated);
   stopOk=false;assert(!strcmp(phonePipeReplaceSecret("ZZZZZZZZZZZZZZZZ",16,false,&replaced),"Error: phone pipe cleanup incomplete; secret unchanged")&&!replaced);stopOk=true;
   enrollOk=false;assert(!strcmp(phonePipeReplaceSecret("ZZZZZZZZZZZZZZZZ",16,true,&replaced),"Error: secret changed; enrollment cleanup busy, retry phone secret")&&replaced);enrollOk=true;
   assert(!phonePipeReplaceSecret("",0,false,&replaced)&&replaced&&!gSettings.phonePipeSecret.length()&&!gSettings.phonePipeSecretGenerated);}
  // The BLE role (PHONE_STOP) is held at every stop, key drop and store above
  // and given back before each call returned; another task's transition
  // refuses the steps before anything stops.
  assert(roleClaims&&!outsideRole&&!roleDepth&&roleKind==int(BleRoleTransition::PHONE_STOP));
  {bool replaced=true;const unsigned stopsBefore=stops,savesBefore=saves;roleBusy=true;
   assert(!strcmp(phonePipeReplaceSecret("0123456789ABCDE4",16,true,&replaced),"Error: BLE role transition busy")&&!replaced);
   assert(!strcmp(phoneSecretCommand("AnotherPipe!Secret123"),"Error: BLE role transition busy"));
   assert(stops==stopsBefore&&saves==savesBefore&&!gSettings.phonePipeSecret.length()&&!roleDepth);roleBusy=false;}
  puts("Pipe secret: validate pass, real String internal storage/moves, allocation failure, source and strength gates, stop/invalidation failures, enrollment revocation, persistence errors, redacted query and console preservation passed; pairing codes stored canonical with the device-made mark cleared in the same persist, the shared steps mark phone pair new's code; the BLE role held from the stop through the store, refused while busy");
}
'''.replace('// ROLE',role).replace('// PRODUCTION',functions)
# Registry/redactor integration is data; execute the command above for behavior
# (test_llm_http_settings runs the audit redactor on `phone secret` lines).
phone=(COMPONENT/'BLE_PhonePipe.cpp').read_text()
assert '{ "phone secret",' in phone and '"Usage: phone secret <passphrase|clear>", /*requiresSuperAdmin=*/true }' in phone
assert '{ "phone secret ",     MASK_AFTER_TOKEN_POS, 2,' in utils and '"phone secret", false, true },' in bt
with tempfile.TemporaryDirectory(prefix='hw1-pipe-secret-') as tmp:
 d=Path(tmp);(d/'test.cpp').write_text(unit)
 sanitize=['-fsanitize=address,undefined','-fno-omit-frame-pointer','-g'] if args.sanitize else []
 includes,sources=arduino_string(d,sanitize)
 subprocess.run([os.environ.get('CXX','clang++'),'-std=c++17','-Wall','-Wextra','-Werror','-Wno-sign-compare',*sanitize,*includes,'-I',str(COMPONENT),str(d/'test.cpp'),*sources,'-o',str(d/'test')],check=True)
 subprocess.run([str(d/'test')],check=True)
