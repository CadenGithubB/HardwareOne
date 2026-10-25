#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

// Native QuickNote (SID 0x12), verified against Even 2.3.2/build243 and the
// 2026-10-15 create/record/save/list/read capture. Independent of Arduino.
namespace G2Notes {
constexpr uint8_t Sid = 0x12;
constexpr size_t TextLimit = 1024, WireLimit = 1536, IdSize = 16;
enum Command : uint32_t {
  List=1, Meta=2, Page=3, Control=4, Asr=5, Status=161, ListRequest=162,
  DetailRequest=163, PageRequest=164, Loaded=165, Ack=166, Draft=167, Voice=168
};
struct Reader {
  const uint8_t* p; size_t left; bool ok = true;
  uint64_t var() {
    uint64_t v=0;
    for (unsigned i=0;i<10;++i) {
      if (!left) { ok=false; return 0; }
      const uint8_t b=*p++; --left;
      if (i==9 && b>1) { ok=false; return 0; }
      v |= uint64_t(b & 127) << (i*7);
      if (!(b&128)) return v;
    }
    ok=false; return 0;
  }
  bool field(uint32_t& number, uint8_t& wire, uint64_t& value, const uint8_t*& data, size_t& len) {
    if (!ok || !left) return false;
    const uint64_t tag=var(); number=uint32_t(tag>>3); wire=tag&7; data=nullptr; len=0; value=0;
    if (!ok || !number || tag>>32) return ok=false;
    if (wire==0) { value=var(); return ok; }
    if (wire==2) { const uint64_t n=var(); if (!ok || n>left) return ok=false; len=size_t(n); }
    else if (wire==1) len=8;
    else if (wire==5) len=4;
    else return ok=false;
    if (len>left) return ok=false;
    data=p; p+=len; left-=len; return true;
  }
};
struct Writer {
  uint8_t* p; size_t cap, size=0; bool ok=true;
  void byte(uint8_t b) { if (size<cap) p[size++]=b; else ok=false; }
  void var(uint64_t v) { do { byte(uint8_t(v&127) | (v>127?128:0)); v>>=7; } while(v); }
  void integer(uint32_t n, uint32_t v) { var(uint64_t(n)<<3); var(v); }
  void bytes(uint32_t n, const void* data, size_t len) {
    var((uint64_t(n)<<3)|2); var(len);
    if (len>cap-size || (len && !data)) { ok=false; return; }
    if (len) memcpy(p+size,data,len);
    size+=len;
  }
  size_t finish() const { return ok ? size : 0; }
};
struct Event {
  uint32_t command=0, magic=0, action=0, entry=0, error=0, acked=0;
  uint32_t direction=0, request=0, page=0;
  char id[IdSize+1]={}; bool hasId=false;
};
inline bool copyId(const uint8_t* p, size_t n, char* out) {
  if (n!=IdSize) return false;
  memcpy(out,p,n); out[n]=0; return true;
}
inline bool parse(const uint8_t* data, size_t length, Event& out) {
  out={}; if (!data || !length || length>128) return false;
  Reader r{data,length}; uint32_t n; uint8_t w; uint64_t v; const uint8_t* b; size_t z;
  const uint8_t* body=nullptr; size_t bodyLen=0; uint32_t bodyNo=0; bool cmd=false, magic=false;
  while(r.field(n,w,v,b,z)) {
    if (n==1 || n==2) {
      if (w!=0 || v>UINT32_MAX || (n==1?cmd:magic)) return false;
      if(n==1) {out.command=uint32_t(v);cmd=true;} else {out.magic=uint32_t(v);magic=true;}
    } else if(n>=3 && n<=16) {
      if(w!=2 || body) return false;
      body=b; bodyLen=z; bodyNo=n;
    }
  }
  if(!r.ok || !cmd || !body) return false;
  const uint32_t expected=out.command==Status?11:out.command==ListRequest?7:
      out.command==DetailRequest?8:out.command==PageRequest?9:out.command==Loaded?10:
      out.command==Ack?12:out.command==Draft?14:out.command==Voice?15:0;
  if(!expected || expected!=bodyNo) return false;
  Reader sub{body,bodyLen}; uint32_t seen=0;
  while(sub.field(n,w,v,b,z)) {
    if(n>3) continue;
    if(seen&(1u<<n)) return false;
    seen|=1u<<n;
    const bool id=(out.command==ListRequest?n==2:
        (out.command==DetailRequest||out.command==PageRequest||out.command==Loaded)&&n==1);
    if(id) {if(w!=2 || !copyId(b,z,out.id)) return false; out.hasId=true; continue;}
    if(w!=0 || v>UINT32_MAX) return false;
    const uint32_t value=uint32_t(v);
    if(out.command==Draft || out.command==Voice || out.command==Status) {
      if(n==1) out.action=value; else if(n==2) out.entry=value; else out.error=value;
    } else if(out.command==Ack) {if(n==1) out.acked=value;else if(n==2)out.error=value;}
    else if(out.command==ListRequest) {if(n==1)out.direction=value;else if(n==3)out.request=value;}
    else if(out.command==DetailRequest) {if(n==2)out.request=value;}
    else {if(n==2)out.page=value;else if(n==3)out.request=value;}
  }
  if(!sub.ok) return false;
  if((out.command==DetailRequest||out.command==PageRequest||out.command==Loaded)&&!out.hasId) return false;
  if(out.command==Draft && (out.action<1||out.action>4)) return false;
  if(out.command==Voice && (out.action<1||out.action>3)) return false;
  if(out.command==ListRequest && out.direction>2) return false;
  return true;
}
inline size_t wrap(uint32_t cmd, uint32_t magic, uint32_t field, const Writer& body,
                   uint8_t* out, size_t cap) {
  if(!body.ok) return 0;
  Writer w{out,cap}; w.integer(1,cmd); w.integer(2,magic); w.bytes(field,body.p,body.size); return w.finish();
}
// Conservative stock 20px layout: count UTF-8 codepoints, never continuation
// bytes. The recording view is 544px wide; detail uses 510px and 24 columns.
inline uint32_t lineCount(const char* text, size_t length, unsigned columns=26) {
  if(!length)return 0;
  uint32_t lines=1;unsigned column=0;
  for(size_t i=0;i<length;++i) {
    const uint8_t c=uint8_t(text[i]);
    if(c=='\n') {++lines;column=0;}
    else if((c&0xc0)!=0x80) {if(column==columns){++lines;column=0;}++column;}
  }
  return lines;
}
// Native drafts are bounded to the stock renderer's 1024 UTF-8 byte contract.
// Save is a separate transition from recording completion.
struct DraftState {
  char text[TextLimit+1]={}; size_t length=0, segmentStart=0;
  bool open=false, recording=false, finishing=false, saveWanted=false;
  void clear() { memset(this,0,sizeof(*this)); }
  bool start() { if(!open || recording || finishing || saveWanted) return false; segmentStart=length; recording=true; return true; }
  void stop() { if(recording) {recording=false;finishing=true;} }
  bool append(const char* s,size_t n) {
    const size_t sep=length&&n?1:0;
    if(n+sep>TextLimit-length) return false;
    if(sep)text[length++]=' ';
    memcpy(text+length,s,n);length+=n;text[length]=0;return true;
  }
  void done() {recording=finishing=false;}
  bool undo() {if(recording||finishing||saveWanted)return false;length=segmentStart;text[length]=0;return true;}
  bool canSave() const {return open&&saveWanted&&!recording&&!finishing&&length;}
};
} // namespace G2Notes
