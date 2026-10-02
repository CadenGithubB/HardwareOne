// Apps → Transcription. Display/input stays on the OLED owner; commands and
// filesystem work use its existing authenticated cmd_exec bridge.
#include "OLED_Mode_Transcription.h"
#if ENABLE_OLED_DISPLAY && ENABLE_DICTATION
#include "OLED_Display.h"
#include "OLED_Utils.h"
#include "OLED_UI.h"
#include "HAL_Input.h"
#include "System_Dictation.h"
#include "System_AuthIdentity.h"
#include "Transcription_UI_Policy.h"
#include <ArduinoJson.h>
#include <new>
#include <cstdio>

namespace {
enum class Op { Status, Start, Stop, Next, Ack, Toggle, List, Read };
struct Ticket { uint32_t generation, viewGeneration; TransportSessionEpoch epoch; Op op; };
struct Entry { char name[64]; char path[128]; };
struct View {
  TransportSessionEpoch epoch;
  uint32_t generation = 1, viewGeneration = 0, pollAt = 0;
  OLEDMode viewMode = OLED_MENU;
  bool entered=false, active=false, busy=false, saveDefault=false, saveEnabled=false;
  bool continuous=false, preparing=false, captureActive=false, inferenceActive=false;
  bool nextPoll=false, more=false, eof=true, micPdm=false, micG2=false;
  char exchange[17]{}, recent[1024]{}, text[513]{}, path[128]{}, message[96]{}, micSource[8]{};
  Entry files[8]{};
  unsigned count=0, listOffset=0, listNext=0, fileOffset=0, fileNext=0;
  uint32_t history[32]{};
  unsigned historyCount=0, line=0;
  bool sd=false;
  OLEDScrollState menu{}, list{};
};
EXT_RAM_BSS_ATTR View v;
// There is at most one request in flight, including across leave/re-enter.
// The callback never touches owner-task UI state or the framebuffer.
static bool inFlight=false;
static portMUX_TYPE mailboxMux=portMUX_INITIALIZER_UNLOCKED;
EXT_RAM_BSS_ATTR char mailbox[4096]{}, received[4096]{};
static Ticket delivered{};
static bool ready=false, deliveredOk=false;
static String pending, pendingAck;
static Op pendingOp=Op::Status;
static bool family(OLEDMode mode) {
  return mode==OLED_TRANSCRIPTION || mode==OLED_TRANSCRIPTION_LIVE ||
      mode==OLED_TRANSCRIPTS || mode==OLED_TRANSCRIPT_VIEW;
}
static void copy(char* dst,size_t cap,const char* src) {
  snprintf(dst,cap,"%s",src ? src : "");
}
static void wipeString(String& text) {
  for (unsigned i=0;i<text.length();++i) text.setCharAt(i,0);
  text="";
}
static void cancelId(TransportSessionEpoch epoch,const char* id) {
  DictationAppLease lease{SOURCE_LOCAL_DISPLAY,epoch,0};
  if (TranscriptionUI::parseId(id,lease.exchange)) (void)dictationAppCancel(lease);
}
static void completed(bool ok,const char* result,void* context) {
  const auto ticket=*static_cast<Ticket*>(context);
  delete static_cast<Ticket*>(context);
  portENTER_CRITICAL(&mailboxMux);
  delivered=ticket; deliveredOk=ok;
  copy(mailbox,sizeof(mailbox),result);
  ready=true;
  portEXIT_CRITICAL(&mailboxMux);
}
static bool send(const String& command,Op op) {
  if (inFlight || !v.epoch) return false;
  auto* ticket=new (std::nothrow) Ticket{v.generation,v.viewGeneration,v.epoch,op};
  if (!ticket) return false;
  if (!submitOLEDCommandForSession(command,v.epoch,completed,ticket)) {
    delete ticket; return false;
  }
  inFlight=true;
  return true;
}
static void queue(const String& command,Op op) {
  if (pending.length()) { oledToastShow("Please wait",1000); return; }
  if (op==Op::List || op==Op::Read) ++v.viewGeneration;
  pending=command; pendingOp=op;
}
static String command(const char* op) {
  return String("transcription ")+op+" "+v.exchange;
}
static void listAt(unsigned offset) {
  queue(String("transcripts list ")+(v.sd?"sd ":"internal ")+String(offset),Op::List);
}
static void readAt(unsigned offset) {
  queue(String("transcripts read \"")+v.path+"\" "+String(offset),Op::Read);
}
static void acceptReply(const Ticket& ticket,bool ok) {
  JsonDocument doc;
  const bool parsed=!deserializeJson(doc,received);
  if (ticket.generation!=v.generation || ticket.epoch!=v.epoch || !v.entered) {
    // A slow start may finish after Back. Only its returned exact lease may be
    // cancelled; never cancel the replacement UI's current session.
    if (parsed && ticket.op==Op::Start) cancelId(ticket.epoch,doc["exchange"]|"");
    return;
  }
  if ((ticket.op==Op::List || ticket.op==Op::Read) &&
      (ticket.viewGeneration!=v.viewGeneration ||
       (ticket.op==Op::List && currentOLEDMode!=OLED_TRANSCRIPTS) ||
       (ticket.op==Op::Read && currentOLEDMode!=OLED_TRANSCRIPT_VIEW))) return;
  if (!ok || !parsed || doc["success"]==false) {
    copy(v.message,sizeof(v.message),parsed ? (doc["error"]|"Request failed") : "Request failed");
    v.pollAt=millis()+1000;
    return; // Keep an unacknowledged receipt for retry, without copying twice.
  }
  v.message[0]=0;
  if (ticket.op==Op::Status || ticket.op==Op::Start || ticket.op==Op::Stop) {
    copy(v.exchange,sizeof(v.exchange),doc["exchange"]|"");
    v.active=doc["active"]|false; v.busy=doc["busy"]|false;
    v.saveDefault=doc["saveDefault"]|false; v.saveEnabled=doc["saveEnabled"]|false;
    v.continuous=doc["continuous"]|false;
    copy(v.micSource,sizeof(v.micSource),doc["micSource"]|"auto");
    v.micPdm=doc["micPdm"]|false; v.micG2=doc["micG2"]|false;
    v.preparing=doc["preparing"]|false;
    v.captureActive=doc["captureActive"]|false;
    v.inferenceActive=doc["inferenceActive"]|false;
    const char* problem=doc["saveError"]|"";
    if (!*problem) problem=doc["failure"]|"";
    if (!*problem && !(doc["available"]|false)) problem=doc["reason"]|"";
    copy(v.message,sizeof(v.message),problem);
    if (ticket.op==Op::Start) { TranscriptionUI::clear(v.recent,sizeof(v.recent)); v.line=0; }
    v.nextPoll=v.exchange[0];
  } else if (ticket.op==Op::Next) {
    if (doc["available"]|false) {
      const char* text=doc["sttText"]|"";
      const unsigned length=doc["length"]|0U;
      if (length==strlen(text) && length) {
        if ((doc["offset"]|0U)==0 && v.recent[0])
          TranscriptionUI::appendRecent(v.recent,sizeof(v.recent),"\n",1);
        TranscriptionUI::appendRecent(v.recent,sizeof(v.recent),text,length);
        pendingAck=command("ack")+" "+String(doc["sequence"]|0U)+" "+
            String(doc["offset"]|0U)+" "+String(length);
      }
    }
    v.nextPoll=false;
  } else if (ticket.op==Op::Ack) {
    wipeString(pendingAck);
  } else if (ticket.op==Op::Toggle) {
    // The settings command returns ordinary text rather than this JSON schema.
  } else if (ticket.op==Op::List) {
    v.count=0;
    for (JsonObject entry:doc["entries"].as<JsonArray>()) {
      if (v.count>=8) break;
      copy(v.files[v.count].name,64,entry["name"]|"");
      copy(v.files[v.count].path,128,entry["path"]|""); ++v.count;
    }
    v.listOffset=doc["offset"]|0U; v.listNext=doc["nextOffset"]|0U;
    v.more=doc["more"]|false;
    oledScrollInit(&v.list,nullptr,4);
  } else if (ticket.op==Op::Read) {
    copy(v.text,sizeof(v.text),doc["sttText"]|"");
    v.fileOffset=doc["offset"]|0U; v.fileNext=doc["nextOffset"]|0U;
    v.eof=doc["eof"]|true; v.line=0;
  }
  v.pollAt=millis()+350;
}
// The built-in display font is ASCII. Decode one UTF-8 code point to a single
// fallback glyph so wrapping never splits a multi-byte character.
static size_t nextGlyph(const char* text,size_t at,char& glyph) {
  const unsigned char ch=text[at]; glyph=ch<128?char(ch):'?';
  ++at;
  while (text[at] && TranscriptionUI::continuation(static_cast<unsigned char>(text[at]))) ++at;
  return at;
}
static unsigned lines(const char* text) {
  unsigned count=1,col=0;
  for(size_t at=0;text[at];) {
    char ch; at=nextGlyph(text,at,ch);
    if(ch=='\r') continue;
    if(ch=='\n' || ++col==21) { ++count; col=0; }
  }
  return count;
}
static void drawText(const char* text) {
  unsigned row=0,col=0; int y=OLED_CONTENT_START_Y;
  oledDisplay->setTextSize(1); oledDisplay->setCursor(0,y);
  for(size_t at=0;text[at] && row<v.line+4;) {
    char ch; at=nextGlyph(text,at,ch);
    if(ch=='\r') continue;
    if(row>=v.line && ch!='\n') oledDisplay->write(ch);
    if(ch=='\n' || ++col==21) {
      ++row; col=0;
      if(row>v.line) y+=8;
      oledDisplay->setCursor(0,y);
    }
  }
}
static void populateMenu() {
  oledScrollClearKeepSelection(&v.menu);
  oledScrollAddItem(&v.menu,v.active?"Stop session":(v.busy?"Speech busy":"Start session"));
  oledScrollAddItem(&v.menu,"Recent live text");
  oledScrollAddItem(&v.menu,v.saveDefault?"Save next: ON":"Save next: OFF");
  oledScrollAddItem(&v.menu,"Saved: internal");
  oledScrollAddItem(&v.menu,"Saved: SD card");
  static char mic[22];
  snprintf(mic,sizeof(mic),"Mic: %s",TranscriptionUI::micLabel(v.micSource));
  oledScrollAddItem(&v.menu,mic);
  oledScrollClampSelection(&v.menu);
}
static void displayTranscriptionHome() {
  populateMenu();
  oledDisplay->setCursor(0,OLED_CONTENT_START_Y);
  const char* status=v.active?(v.saveEnabled?"Active; saving":"Active; not saving"):
      (v.continuous?"Local continuous":"Pi: one recording");
  char line[22];
  snprintf(line,sizeof(line),"%.21s",v.message[0]?v.message:status);
  oledDisplay->print(line);
  v.menu.visibleLines=3;
  oledScrollRenderSimple(oledDisplay,&v.menu,true,OLED_CONTENT_START_Y+10);
}
static void displayTranscriptionLive() {
  drawText(v.recent[0]?v.recent:(v.preparing?"Preparing...":v.captureActive?"Listening...":
      v.inferenceActive?"Transcribing...":v.active?"Finishing...":"No captured text"));
}
static void populateList() {
  oledScrollClearKeepSelection(&v.list);
  for(unsigned i=0;i<v.count;++i) oledScrollAddItem(&v.list,v.files[i].name);
  if(v.more) oledScrollAddItem(&v.list,"Next files >");
  if(v.listOffset) oledScrollAddItem(&v.list,"< Previous files");
  if(!v.count) oledScrollAddItem(&v.list,v.message[0]?v.message:"No saved transcripts");
  oledScrollClampSelection(&v.list);
}
static void displayTranscriptionFiles() {
  populateList(); oledScrollRenderSimple(oledDisplay,&v.list);
}
static void displayTranscriptionFile() { drawText(v.text[0]?v.text:(v.message[0]?v.message:"Loading...")); }
static bool selected(uint32_t buttons) {
  return INPUT_CHECK(buttons,INPUT_BUTTON_A) || INPUT_CHECK(buttons,INPUT_BUTTON_X);
}
static bool menuInput(int,int,uint32_t buttons) {
  populateMenu();
  if(oledScrollHandleNav(&v.menu)) return true;
  if(!selected(buttons)) return false;
  if(oledGuestBlocksMutate()) return true;
  switch(v.menu.selectedIndex) {
    case 0:
      if(v.active) queue(command("stop"),Op::Stop);
      else if(!v.busy) queue("transcription start",Op::Start);
      break;
    case 1: v.line=0; requestOLEDMode(OLED_TRANSCRIPTION_LIVE,"transcription.live"); break;
    case 2: queue(v.saveDefault?"sttsavetranscripts 0":"sttsavetranscripts 1",Op::Toggle); break;
    case 3: case 4:
      v.sd=v.menu.selectedIndex==4; v.count=0; v.more=false; v.listOffset=0;
      oledScrollInit(&v.list,nullptr,4); listAt(0);
      requestOLEDMode(OLED_TRANSCRIPTS,"transcription.files"); break;
    case 5:
      // The source is claimed at session start; a change applies next session.
      if(v.active) oledToastShow("Stop session first",1200);
      else queue(String("micsource ")+TranscriptionUI::nextMicSource(v.micSource,v.micPdm,v.micG2),Op::Toggle);
      break;
  }
  return true;
}
static bool liveInput(int,int,uint32_t buttons) {
  if(gNavEvents.up && v.line) { --v.line; return true; }
  if(gNavEvents.down && v.line+4<lines(v.recent)) { ++v.line; return true; }
  if(selected(buttons) && v.active) { queue(command("stop"),Op::Stop); return true; }
  return false;
}
static bool listInput(int,int,uint32_t buttons) {
  populateList();
  if(oledScrollHandleNav(&v.list)) return true;
  if(!selected(buttons)) return false;
  const unsigned index=v.list.selectedIndex;
  if(index<v.count) {
    if(pending.length()) return true;
    copy(v.path,sizeof(v.path),v.files[index].path);
    v.text[0]=0; v.historyCount=0; v.line=0; readAt(0);
    requestOLEDMode(OLED_TRANSCRIPT_VIEW,"transcription.file");
  } else if(v.more && index==v.count) listAt(v.listNext);
  else if(v.listOffset) listAt(v.listOffset>=8?v.listOffset-8:0);
  return true;
}
static bool fileInput(int,int,uint32_t buttons) {
  if(pending.length() || inFlight) return INPUT_CHECK(buttons,INPUT_BUTTON_B)?false:true;
  if(gNavEvents.up) {
    if(v.line) --v.line;
    else if(v.fileOffset) readAt(v.historyCount?v.history[--v.historyCount]:0);
    return true;
  }
  if(gNavEvents.down || selected(buttons)) {
    if(v.line+4<lines(v.text)) ++v.line;
    else if(!v.eof && v.fileNext>v.fileOffset) {
      if(v.historyCount==32) {
        memmove(v.history,v.history+1,31*sizeof(v.history[0])); --v.historyCount;
      }
      v.history[v.historyCount++]=v.fileOffset; readAt(v.fileNext);
    }
    return true;
  }
  return false;
}
} // namespace

void resetOLEDTranscription() {
  cancelId(v.epoch,v.exchange);
  const uint32_t generation=v.generation+1;
  TranscriptionUI::clear(reinterpret_cast<char*>(&v),sizeof(v));
  v.generation=generation?generation:1;
  wipeString(pending); wipeString(pendingAck);
}
void prepareTranscriptionData() {
  const bool here=family(currentOLEDMode);
  if(v.entered && !here) resetOLEDTranscription();
  if(here && !v.entered) {
    String user; bool authed=false;
    v.epoch=localDisplayTransportSessionSnapshot(user,authed);
    wipeString(user);
    if(!authed || !v.epoch) return;
    v.entered=true; v.pollAt=0;
    oledScrollInit(&v.menu,nullptr,3); oledScrollInit(&v.list,nullptr,4);
  }
  if (here && v.entered && v.viewMode!=currentOLEDMode) {
    v.viewMode=currentOLEDMode; ++v.viewGeneration;
    if ((pendingOp==Op::List && currentOLEDMode!=OLED_TRANSCRIPTS) ||
        (pendingOp==Op::Read && currentOLEDMode!=OLED_TRANSCRIPT_VIEW)) wipeString(pending);
  }
  Ticket ticket{}; bool got=false,ok=false;
  portENTER_CRITICAL(&mailboxMux);
  if(ready) {
    memcpy(received,mailbox,sizeof(received));
    TranscriptionUI::clear(mailbox,sizeof(mailbox));
    ticket=delivered; ok=deliveredOk; ready=false; got=true;
  }
  portEXIT_CRITICAL(&mailboxMux);
  if(got) {
    inFlight=false;
    if(ticket.op==Op::Toggle && ticket.generation==v.generation && ticket.epoch==v.epoch) {
      const bool changed=ok && strncmp(received,"Error",5)!=0 && strncmp(received,"Failed",6)!=0;
      copy(v.message,sizeof(v.message),changed?"Setting updated":"Setting change failed");
      v.nextPoll=false; v.pollAt=0;
    } else acceptReply(ticket,ok);
    TranscriptionUI::clear(received,sizeof(received)); oledMarkDirty();
  }
  if(!v.entered || inFlight) return;
  if(pendingAck.length()) {
    if(int32_t(millis()-v.pollAt)>=0) send(pendingAck,Op::Ack);
  } else if(pending.length()) {
    if(send(pending,pendingOp)) wipeString(pending);
  } else if(int32_t(millis()-v.pollAt)>=0) {
    if(v.nextPoll && v.exchange[0]) send(command("next"),Op::Next);
    else send(v.exchange[0]?command("status"):String("transcription status"),Op::Status);
    v.pollAt=millis()+600;
  }
}
static const OLEDModeEntry transcriptionModes[]={
  {OLED_TRANSCRIPTION,"Transcription","mic",displayTranscriptionHome,nullptr,menuInput,true,51,"A:Select B:Cancel"},
  {OLED_TRANSCRIPTION_LIVE,"Recent text","mic",displayTranscriptionLive,nullptr,liveInput,false,-1,"A:Stop B:Menu"},
  {OLED_TRANSCRIPTS,"Transcripts","mic",displayTranscriptionFiles,nullptr,listInput,false,-1,"A:Open B:Back"},
  {OLED_TRANSCRIPT_VIEW,"Transcript","mic",displayTranscriptionFile,nullptr,fileInput,false,-1,"A:More B:Back"}
};
REGISTER_OLED_MODE_MODULE(transcriptionModes,sizeof(transcriptionModes)/sizeof(transcriptionModes[0]),"Transcription");
#endif
