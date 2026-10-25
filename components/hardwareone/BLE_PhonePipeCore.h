#pragma once
// Android pipe, inner protocol v2: allocation-free wire/state owner. Call under
// the adapter's lock. No BLE, TLS, credentials, Arduino or OS dependencies.
// Slots by class: 0..1 relay (TCP through the phone), 2 the app console.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace hw1pipe {
inline constexpr char kServiceUuid[] = "b7e10000-6f7d-4b2e-a8c4-3d9f5021c6a0";
inline constexpr char kRxUuid[] = "b7e10001-6f7d-4b2e-a8c4-3d9f5021c6a0";
inline constexpr char kTxUuid[] = "b7e10002-6f7d-4b2e-a8c4-3d9f5021c6a0";
constexpr size_t kHeader = 12, kValue = 512, kControl = 272, kWindow = 4096;
constexpr uint8_t kFirst = 1, kLast = 2;
// Header byte 0 of every frame; HELLO/HELLO_ACK carry kMaxStreams and a flag byte.
constexpr uint8_t kVersion = 2, kMaxStreams = 3;
// Worst legal queue: per slot an end message for the old id, OPEN and CLOSE
// (or CREDIT) for the new one, plus PING, PONG and LINK_STATE (3*3+3).
constexpr unsigned kRelaySlots = 2, kConsoleSlot = 2, kControlQueue = 12;
constexpr uint32_t kRelayCredit = 1024, kConsoleCredit = 4096;
// Flag bytes: the P4's in HELLO/LINK_STATE, the phone's in HELLO_ACK/LINK_STATE.
// Any other bit set is a protocol error.
constexpr uint8_t kP4ConsoleOffered = 0x01, kPhoneRelayOn = 0x01, kPhoneConsoleWanted = 0x02;
constexpr uint8_t kP4FlagMask = kP4ConsoleOffered, kPhoneFlagMask = kPhoneRelayOn | kPhoneConsoleWanted;
enum class Op : uint8_t { Hello=1, HelloAck=2, Open=0x10, OpenOk=0x11,
  OpenErr=0x12, Data=0x20, Credit=0x21, Fin=0x22, Close=0x23,
  CloseAck=0x24, Reset=0x25, Ping=0x30, Pong=0x31, LinkState=0x32 };
enum class Error : uint16_t { None=0, Protocol=1, Version=2, Unauthorized=3,
  Target=4, Vpn=5, Dns=6, Connect=7, ConnectTimeout=8, Busy=9,
  Resources=10, Tcp=11, Flow=12, Deadline=13, Cancelled=14, Display=15,
  ServiceOff=16, Unsupported=17, Paused=18 };
// Console CLOSE reasons the P4 sends (it accepts 0..5 on slot 2). Relay: 0 normal, 1 cancelled.
enum class ConsoleClose : uint16_t { Error=1, Renewed=2, ConsoleOff=3, NotWanted=4, Revoked=5 };
enum class PushResult : uint8_t { Ok, NoSpace, Gone };
// How a session that never reached READY ended (the owner's version-mismatch
// strike): the phone's version RESET, or silence after our HELLO went out.
enum class HelloEnd : uint8_t { None, VersionSignal, Silent };
enum class State : uint8_t { Down, Hello, Ready, Failed };
enum class StreamState : uint8_t { Free, Opening, Open, Closing, Ended };
inline uint16_t get16(const uint8_t* p) { return uint16_t(p[0]) | uint16_t(p[1])<<8; }
inline uint32_t get32(const uint8_t* p) { return uint32_t(get16(p)) | uint32_t(get16(p+2))<<16; }
inline void put16(uint8_t* p, uint16_t v) { p[0]=uint8_t(v); p[1]=uint8_t(v>>8); }
inline void put32(uint8_t* p, uint32_t v) { put16(p,uint16_t(v)); put16(p+2,uint16_t(v>>16)); }
inline uint16_t valueLimit(uint16_t mtu) { return mtu<23 ? 0 : uint16_t(mtu>515 ? 512 : mtu-3); }
inline bool expired(uint32_t now, uint32_t start, uint32_t duration) { return uint32_t(now-start)>=duration; }
inline bool knownError(uint16_t e) { return e>=1 && e<=18; }
static_assert(uint16_t(Error::Paused)==18 && uint16_t(Error::ServiceOff)==16 && uint16_t(Error::Unsupported)==17,
              "v2 error codes are wire values");
static_assert(kMaxStreams==kRelaySlots+1 && kConsoleSlot==kRelaySlots, "slot classes: relays, then the console");
static_assert(kConsoleCredit==kWindow && kRelayCredit<=kWindow, "credit never runs past the ring");
static_assert(kControlQueue==3*kMaxStreams+3, "the control queue holds the worst legal case exactly");
struct SessionProbe;  // host tests only: reads private buffers to prove they were wiped

// Every byte outside the live region is zero: drop and clear wipe what they
// release (console logins and relay plaintext sit here in PSRAM).
struct Buffer {
  uint8_t bytes[kWindow]{};
  size_t head=0, size=0;
  size_t space() const { return kWindow-size; }
  void clear() { drop(size); head=0; }
  size_t push(const uint8_t* p,size_t n) {
    if (n>space()) n=space();
    for (size_t i=0;i<n;++i) bytes[(head+size+i)%kWindow]=p[i];
    size+=n; return n;
  }
  size_t peek(uint8_t* p,size_t n,size_t offset=0) const {
    if (offset>=size) return 0;
    if (n>size-offset) n=size-offset;
    for(size_t i=0;i<n;++i) p[i]=bytes[(head+offset+i)%kWindow];
    return n;
  }
  void drop(size_t n) {
    if (n>size) n=size;
    for(size_t i=0;i<n;++i) bytes[(head+i)%kWindow]=0;
    head=(head+n)%kWindow; size-=n;
  }
  size_t pop(uint8_t* p,size_t n) { n=peek(p,n); drop(n); return n; }
};
struct Stream {
  uint16_t id=0;
  StreamState state=StreamState::Free;
  Error error=Error::None;
  Buffer tx,rx;
  uint32_t sent=0,received=0,consumed=0,peerLimit=0,receiveLimit=0,queuedLimit=0;
  uint32_t creditAt=0,closeAt=0,openedAt=0;
  // Relay: one cumulative pressure budget for this TLS stream, including setup
  // after OPEN_OK; quiet gaps and successful submissions do not replenish it.
  // Console: display pressure defers it for as long as it lasts (no budget).
  uint32_t pressureAt=0,pressureMs=0;
  bool pressureActive=false;
  // The slot's class, fixed by the Session (slot kConsoleSlot); reset() keeps it.
  bool console=false;
  bool deferred() const { return pressureActive && (console || pressureMs<2000); }
  // openSubmittedAt: local acceptance of the last OPEN fragment (A-1 response
  // allowance starts here). remoteEnd: the phone ended it (OPEN_ERR or RESET).
  bool eof=false,openSubmitted=false,remoteEnd=false;
  uint32_t openSubmittedAt=0;
  // closeAfterDrain: CLOSE(drainReason) once tx is empty or 3 s after drainAt.
  bool draining=false;
  uint16_t drainReason=0;
  uint32_t drainAt=0;
  // closeReason: the reason of this P4's CLOSE; peerCloseReason: the phone's (0: none).
  uint16_t closeReason=0,peerCloseReason=0;
  void reset() {
    id=0; state=StreamState::Free; error=Error::None; tx.clear(); rx.clear();
    sent=received=consumed=peerLimit=receiveLimit=queuedLimit=creditAt=closeAt=0;
    eof=openSubmitted=remoteEnd=pressureActive=draining=false; openedAt=pressureAt=pressureMs=openSubmittedAt=drainAt=0;
    drainReason=closeReason=peerCloseReason=0;
  }
  bool live() const { return state==StreamState::Opening || state==StreamState::Open || state==StreamState::Closing; }
};
struct Message { Op op=Op::Reset; uint16_t id=0,len=0,offset=0; uint8_t body[kControl]{}; };

class Session {
 public:
  State state=State::Down;
  Error error=Error::None;
  bool peerReset=false;  // the phone ended this session with a stream-0 RESET
  Stream streams[kMaxStreams];  // [0..kRelaySlots) relay, [kConsoleSlot] console
  uint16_t value=20;
  uint32_t txSequence=0,rxSequence=0;
  Session() { classify(); }
  // This session's pipe HELLO nonce: random per session and known to both
  // ends, so diagnostics on both sides can use it as a correlation tag.
  const uint8_t* nonce() const { return nonce_; }
  void down(Error why=Error::Connect) { teardown(why); }  // untimed: never a strike
  // The adapter's link end: with `now`, a link loss within 1.5 s of our HELLO
  // going out, before any answer, latches HelloEnd::Silent.
  void down(Error why,uint32_t now) {
    if(why==Error::Connect && helloUnanswered() && !expired(now,helloAt_,1500)) latchHello(HelloEnd::Silent,now);
    teardown(why);
  }
  bool start(uint16_t mtu,const uint8_t nonce[8],uint32_t now,uint16_t outerOverhead=0) {
    down();
    helloEnd_=HelloEnd::None; helloEndAfter_=helloAt_=0; helloAccepted_=false;
    if (!nonce || !(value=valueLimit(mtu))) return fail(Error::Protocol);
    if(value<outerOverhead+20)return fail(Error::Protocol);
    value-=outerOverhead;
    bool nonzero=false; for(unsigned i=0;i<8;++i) nonzero|=nonce[i]!=0;
    if(!nonzero) return fail(Error::Protocol);
    std::memcpy(nonce_,nonce,8); txSequence=rxSequence=0; nextId_=1; peerReset=false;
    classify(); for(auto& s:streams) s.reset();
    error=Error::None; state=State::Hello; started_=lastRx_=lastTx_=now; pingPending_=false;
    round_=relayTurns_=0; peerFlags_=0; sentFlags_=localFlags_; linkStateSent_=false;
    uint8_t b[12]{}; std::memcpy(b,nonce,8); b[8]=kMaxStreams; b[9]=localFlags_; put16(b+10,value);
    return queue(Op::Hello,0,b,sizeof(b));
  }
  // The owner sets the P4 flags before every poll: HELLO carries them, and a
  // change after READY goes out as LINK_STATE (one queued per second at most;
  // an unsent one is overwritten).
  void setLocalFlags(uint8_t flags) { localFlags_=uint8_t(flags&kP4FlagMask); }
  uint8_t localFlags() const { return localFlags_; }
  // The phone's flags from HELLO_ACK and its latest LINK_STATE (0 unless READY).
  uint8_t peerFlags() const { return state==State::Ready ? peerFlags_ : 0; }
  // Latched when a session ends before READY, reset by start(); the time is
  // from our HELLO's acceptance by the BLE API to that end.
  HelloEnd helloEnd() const { return helloEnd_; }
  uint32_t helloEndAfterMs() const { return helloEndAfter_; }
  // The caller retains an ended stream until it consumes its outcome and releases it.
  // This prevents another HTTP worker from replacing an unread failure/EOF.
  Stream* find(uint16_t id) { for(auto& s:streams) if(id && s.id==id) return &s; return nullptr; }
  const Stream* find(uint16_t id) const { for(const auto& s:streams) if(id && s.id==id) return &s; return nullptr; }
  void release(uint16_t id) { auto* s=find(id); if(s && !s->live()) s->reset(); }
  // Relay slots only; the console opens through openConsole().
  uint16_t open(uint8_t type,const uint8_t* host,size_t length,uint16_t port,uint32_t now) {
    if(state!=State::Ready || !host || !port || !nextId_ || length>253 ||
       !((type==1 && length==4)||(type==2 && length==16)||(type==3 && length))) return 0;
    if(type==3) for(size_t i=0;i<length;++i)
      if(!((host[i]>='a'&&host[i]<='z')||(host[i]>='0'&&host[i]<='9')||host[i]=='-'||host[i]=='.')) return 0;
    Stream* s=nullptr; for(unsigned i=0;i<kRelaySlots;++i) if(streams[i].state==StreamState::Free) {s=&streams[i];break;}
    if(!s) return 0;
    s->reset(); s->id=nextId_++; s->state=StreamState::Opening;
    s->receiveLimit=s->queuedLimit=kRelayCredit; s->creditAt=now;
    uint8_t b[kControl]{}; put32(b,kRelayCredit); put16(b+4,port); b[6]=type; b[7]=uint8_t(length);
    std::memcpy(b+8,host,length);
    if(!queue(Op::Open,s->id,b,length+8)) return 0;
    return s->id;
  }
  // Slot 2: OPEN limit 4096 | port 1 | type 4 (local) | no host. 0 unless READY and the slot is Free.
  uint16_t openConsole(uint32_t now) {
    auto& s=streams[kConsoleSlot];
    if(state!=State::Ready || !nextId_ || s.state!=StreamState::Free) return 0;
    s.reset(); s.id=nextId_++; s.state=StreamState::Opening;
    s.receiveLimit=s.queuedLimit=kConsoleCredit; s.creditAt=now;
    uint8_t b[8]{}; put32(b,kConsoleCredit); put16(b+4,1); b[6]=4;
    if(!queue(Op::Open,s.id,b,sizeof(b))) return 0;
    return s.id;
  }
  bool relayLive() const { for(unsigned i=0;i<kRelaySlots;++i) if(streams[i].live()) return true; return false; }
  unsigned controlQueued() const { return count_; }  // diagnostics and the queue-bound proof
  size_t write(uint16_t id,const uint8_t* p,size_t n) {
    auto* s=find(id); if(state!=State::Ready || !s || s->state!=StreamState::Open || s->draining || !p) return 0;
    return s->tx.push(p,n);
  }
  size_t read(uint16_t id,uint8_t* p,size_t n) {
    auto* s=find(id); if(!s || !p || s->state!=StreamState::Open) return 0;
    n=s->rx.pop(p,n); s->consumed+=uint32_t(n); return n;
  }
  // Record ingress: look at whole records in place and consume() only what was
  // handed on. Bytes left in rx withhold credit; consume wipes them.
  size_t rxAvailable(uint16_t id) const {
    auto* s=find(id); return s && s->state==StreamState::Open ? s->rx.size : 0;
  }
  size_t rxPeek(uint16_t id,uint8_t* out,size_t n,size_t offset=0) const {
    auto* s=find(id); if(!s || !out || s->state!=StreamState::Open) return 0;
    return s->rx.peek(out,n,offset);
  }
  bool consume(uint16_t id,size_t n) {
    auto* s=find(id); if(!s || s->state!=StreamState::Open || n>s->rx.size) return false;
    s->rx.drop(n); s->consumed+=uint32_t(n); return true;
  }
  size_t txSpace(uint16_t id) const {
    auto* s=find(id);
    return state==State::Ready && s && s->state==StreamState::Open && !s->draining ? s->tx.space() : 0;
  }
  // One whole record (head, then payload) or nothing; `reserve` bytes stay free
  // after it (the writer's room for an ABORT). Gone: not READY, not Open, draining.
  PushResult pushRecord(uint16_t id,const uint8_t* head,size_t headLen,const uint8_t* payload,size_t n,size_t reserve) {
    auto* s=find(id);
    if(state!=State::Ready || !s || s->state!=StreamState::Open || s->draining || (headLen && !head) || (n && !payload))
      return PushResult::Gone;
    const size_t space=s->tx.space();
    if(headLen>space || n>space-headLen || reserve>space-headLen-n) return PushResult::NoSpace;
    s->tx.push(head,headLen); if(n) s->tx.push(payload,n);
    return PushResult::Ok;
  }
  // Relay close: reason 0 normal, 1 cancelled. On the console slot it sends
  // ConsoleClose::Error (the P4 never sends console reason 0).
  bool close(uint16_t id,uint32_t now,bool cancelled=false) {
    auto* s=find(id); if(!s || !s->live() || s->state==StreamState::Closing) return true;
    return closeNow(*s,now,s->console ? uint16_t(ConsoleClose::Error) : uint16_t(cancelled?1:0));
  }
  // CLOSE(reason) once everything already in tx has been accepted, or after
  // 3 s (the rest is wiped). Opening, or nothing left to send: at once. write()
  // and pushRecord() refuse while draining; inbound DATA within credit is still
  // taken (closeNow wipes it). The first reason stands.
  bool closeAfterDrain(uint16_t id,uint32_t now,uint16_t reason) {
    auto* s=find(id); if(!s || !s->live() || s->state==StreamState::Closing || s->draining) return true;
    if(s->state==StreamState::Opening || !s->tx.size) return closeNow(*s,now,reason);
    s->draining=true; s->drainReason=reason; s->drainAt=now; return true;
  }
  // Allocate no work on callbacks. Adapter drives tick and TX in its worker.
  // relayPressure defers relay DATA/CREDIT within each stream's 2 s budget;
  // consoleDisplayBusy defers console DATA/CREDIT for as long as it lasts.
  // Control frames never yield.
  bool tick(uint32_t now,bool relayPressure=false,bool consoleDisplayBusy=false) {
    if(state==State::Hello && expired(now,started_,3000)) {
      if(helloUnanswered()) latchHello(HelloEnd::Silent,now);
      return fail(Error::Deadline);
    }
    if(assembling_ && expired(now,assemblyAt_,2000)) return fail(Error::Deadline);
    if(state!=State::Ready) return state!=State::Failed;
    if(!linkState(now)) return false;
    for(auto& s:streams) {
      if(s.state==StreamState::Closing && expired(now,s.closeAt,2000)) return fail(Error::Deadline);
      if(s.state!=StreamState::Open) continue;
      if(s.console) s.pressureActive=consoleDisplayBusy;
      else {
        if(s.pressureActive) {
          const uint32_t elapsed=uint32_t(now-s.pressureAt);
          const uint32_t left=2000-s.pressureMs;
          s.pressureMs+=elapsed<left?elapsed:left;
        }
        s.pressureActive=relayPressure;
      }
      s.pressureAt=now;
      if(s.draining && (!s.tx.size || expired(now,s.drainAt,3000))) {
        if(!closeNow(s,now,s.drainReason)) return false;
        continue;
      }
      if(s.eof || s.deferred()) continue;
      const uint32_t window=s.console?kConsoleCredit:kRelayCredit;
      if(s.consumed>UINT32_MAX-window) return fail(Error::Flow);
      uint32_t limit=s.consumed+window;
      if(limit>s.queuedLimit && (limit-s.queuedLimit>=512 || expired(now,s.creditAt,100))) {
        uint8_t b[4]; put32(b,limit);
        if(!queue(Op::Credit,s.id,b,4)) return false;
        s.queuedLimit=limit; s.creditAt=now;
      }
    }
    if(pingPending_ && expired(now,pingAt_,5000)) return fail(Error::Deadline);
    // Relay PING: 5 s with nothing received while a relay stream is live. Link
    // PING: 60 s idle either way (the phone's watchdog runs on what we send).
    if(!pingPending_ && ((relayLive() && expired(now,lastRx_,5000)) ||
                         expired(now,lastRx_,60000) || expired(now,lastTx_,60000))) {
      uint8_t b[4]; put32(b,now); pingCookie_=now; pingAt_=now; pingPending_=true;
      return queue(Op::Ping,0,b,4);
    }
    return true;
  }
  // peek/accept form a submission transaction. Adapter holds its state lock across
  // them and calls accept only if the BLE API definitely accepted the copied value.
  size_t peek(uint8_t out[kValue],bool allowData=true) {
    if(state!=State::Ready && state!=State::Hello) return 0;
    if(txSequence==UINT32_MAX) { fail(Error::Protocol); return 0; }
    if(pending_) {
      auto* s=find(get16(packet_+2));
      const Op op=Op(packet_[1]);
      if(s && s->deferred() && (op==Op::Data || op==Op::Credit)) unpend();
      else { std::memcpy(out,packet_,packetSize_); return packetSize_; }
    }
    size_t n=0; Op op; uint16_t id; uint8_t flags=0;
    // Deferred CREDIT cannot hold CLOSE/RESET/PING or another stream hostage.
    unsigned selected=0;
    while(selected<count_) {
      const auto& m=messages_[selected]; auto* s=find(m.id);
      if(m.op!=Op::Credit || !s || !s->deferred()) break;
      ++selected;
    }
    if(selected<count_) {
      if(selected) {
        const Message next=messages_[selected];
        for(unsigned i=selected;i>0;--i) messages_[i]=messages_[i-1];
        messages_[0]=next;
      }
      auto& m=messages_[0]; op=m.op; id=m.id;
      n=m.len-m.offset; if(n>value-kHeader) n=value-kHeader;
      flags=uint8_t((m.offset==0?kFirst:0)|(m.offset+n==m.len?kLast:0));
      std::memcpy(packet_+kHeader,m.body+m.offset,n); pendingControl_=true;
    } else {
      if(!allowData) return 0;
      Stream* s=nextData();
      if(!s) return 0;
      n=value-kHeader; if(n>s->peerLimit-s->sent) n=s->peerLimit-s->sent;
      n=s->tx.peek(packet_+kHeader,n); op=Op::Data; id=s->id; pendingControl_=false;
    }
    packet_[0]=kVersion; packet_[1]=uint8_t(op); put16(packet_+2,id); put32(packet_+4,txSequence);
    put16(packet_+8,uint16_t(n)); packet_[10]=flags; packet_[11]=0;
    packetSize_=n+kHeader; pending_=true; std::memcpy(out,packet_,packetSize_); return packetSize_;
  }
  bool accept(uint32_t now) {
    if(!pending_) return false;
    const size_t n=packetSize_-kHeader;
    if(pendingControl_) {
      auto& m=messages_[0]; m.offset=uint16_t(m.offset+n);
      if(m.offset==m.len) {
        if(m.op==Op::Hello) {helloAccepted_=true;helloAt_=now;}
        if(m.op==Op::Open) {auto* s=find(m.id);if(s){s->openSubmitted=true;s->openSubmittedAt=now;}}
        if(m.op==Op::CloseAck) {auto* s=find(m.id);if(s && s->live())end(*s,s->error);}
        if(m.op==Op::Credit) {auto* s=find(m.id);if(s && s->state==StreamState::Open)s->receiveLimit=get32(m.body);}
        for(unsigned i=1;i<count_;++i) messages_[i-1]=messages_[i];
        --count_;
      }
    } else {
      auto* s=find(get16(packet_+2)); if(!s || s->tx.size<n) return fail(Error::Protocol);
      s->tx.drop(n); s->sent+=uint32_t(n);
      // The console's turn: one accepted frame after three relay frames that
      // were accepted while it was waiting.
      if(s->console) relayTurns_=0; else if(eligible(streams[kConsoleSlot])) ++relayTurns_;
    }
    ++txSequence; lastTx_=now; unpend(); return true;
  }
  bool receive(const uint8_t* p,size_t size,uint32_t now) {
    if(state!=State::Hello && state!=State::Ready) return false;
    // A phone that cannot speak this version answers HELLO with a stream-0
    // RESET 2 under a v1 or v2 header; it is the only non-v2 frame accepted.
    if(state==State::Hello && versionSignal(p,size)) {
      ++rxSequence; lastRx_=now; peerReset=true;
      if(helloAccepted_ && helloEnd_==HelloEnd::None) latchHello(HelloEnd::VersionSignal,now);
      return fail(Error::Version);
    }
    if(!p || size<kHeader || size>value || p[0]!=kVersion || p[11] || get16(p+8)!=size-kHeader ||
       get32(p+4)!=rxSequence || rxSequence==UINT32_MAX) return fail(Error::Protocol);
    ++rxSequence; lastRx_=now;
    Op op=Op(p[1]); uint16_t id=get16(p+2); size_t n=size-kHeader; uint8_t flags=p[10]; p+=kHeader;
    if(op==Op::Data) {
      if(flags || assembling_ || !n || state!=State::Ready) return fail(Error::Protocol);
      auto* s=find(id);
      if(!s) return retired(id) ? true : fail(Error::Protocol);
      if(s->state==StreamState::Ended || s->state==StreamState::Closing) return true;
      if(s->state!=StreamState::Open || s->eof) return fail(Error::Protocol);
      if(n>s->receiveLimit-s->received || n>s->rx.space()) return fail(Error::Flow);
      s->rx.push(p,n); s->received+=uint32_t(n); return true;
    }
    if(flags&~3 || !(flags || assembling_)) return fail(Error::Protocol);
    if(flags&kFirst) {
      if(assembling_) return fail(Error::Protocol);
      assembling_=true; assemblyOp_=op; assemblyId_=id; assemblySize_=0; assemblyAt_=now;
    }
    if(!assembling_ || op!=assemblyOp_ || id!=assemblyId_ || n>kControl-assemblySize_) return fail(Error::Protocol);
    std::memcpy(assembly_+assemblySize_,p,n); assemblySize_+=n;
    if(!(flags&kLast)) return true;
    assembling_=false; return control(op,id,assembly_,assemblySize_,now);
  }
 private:
  friend struct SessionProbe;
  Message messages_[kControlQueue]; unsigned count_=0,round_=0,relayTurns_=0;
  uint16_t nextId_=1; uint8_t nonce_[8]{},packet_[kValue]{},assembly_[kControl]{};
  size_t packetSize_=0,assemblySize_=0; bool pending_=false,pendingControl_=false,assembling_=false;
  Op assemblyOp_=Op::Reset; uint16_t assemblyId_=0;
  uint32_t started_=0,lastRx_=0,lastTx_=0,assemblyAt_=0,pingCookie_=0,pingAt_=0; bool pingPending_=false;
  uint8_t localFlags_=0,sentFlags_=0,peerFlags_=0; bool linkStateSent_=false; uint32_t linkStateAt_=0;
  bool helloAccepted_=false; HelloEnd helloEnd_=HelloEnd::None; uint32_t helloAt_=0,helloEndAfter_=0;
  void classify() { for(unsigned i=0;i<kMaxStreams;++i) streams[i].console=i==kConsoleSlot; }
  bool retired(uint16_t id) const { return id && (!nextId_ || id<nextId_); }
  // packet_ holds plaintext only while a frame is pending: zero it whenever that ends.
  void unpend() { std::memset(packet_,0,packetSize_); packetSize_=0; pending_=false; }
  void cancelData(uint16_t id) { if(pending_ && !pendingControl_ && get16(packet_+2)==id) unpend(); }
  void teardown(Error why) {
    state=State::Down; error=why; count_=0; assembling_=false; pending_=false;
    std::memset(packet_,0,sizeof(packet_)); std::memset(assembly_,0,sizeof(assembly_)); packetSize_=assemblySize_=0;
    for(auto& s:streams) if(s.live()) end(s,why);
  }
  // Our HELLO went out and nothing at all has come back yet.
  bool helloUnanswered() const { return state==State::Hello && helloAccepted_ && !rxSequence && helloEnd_==HelloEnd::None; }
  void latchHello(HelloEnd how,uint32_t now) { helloEnd_=how; helloEndAfter_=uint32_t(now-helloAt_); }
  bool versionSignal(const uint8_t* p,size_t size) const {
    return p && size==kHeader+2 && (p[0]==1 || p[0]==kVersion) && p[1]==uint8_t(Op::Reset) && !get16(p+2) &&
      get32(p+4)==rxSequence && get16(p+8)==2 && p[10]==(kFirst|kLast) && !p[11] &&
      get16(p+kHeader)==uint16_t(Error::Version);
  }
  bool eligible(const Stream& c) const {
    return c.state==StreamState::Open && !c.deferred() && c.tx.size && c.peerLimit>c.sent;
  }
  // Relays round-robin over their slots; the console is chosen when no relay
  // can send or after three relay frames (one turn in four).
  Stream* nextData() {
    unsigned pick=kRelaySlots;
    for(unsigned i=0;i<kRelaySlots && pick==kRelaySlots;++i)
      if(eligible(streams[(round_+i)%kRelaySlots])) pick=(round_+i)%kRelaySlots;
    if(eligible(streams[kConsoleSlot]) && (pick==kRelaySlots || relayTurns_>=3)) return &streams[kConsoleSlot];
    if(pick==kRelaySlots) return nullptr;
    round_=(pick+1)%kRelaySlots; return &streams[pick];
  }
  bool linkState(uint32_t now) {
    if(localFlags_==sentFlags_) return true;
    uint8_t b[4]; put32(b,localFlags_);
    if(auto* m=unstarted(Op::LinkState,0)) { std::memcpy(m->body,b,4); sentFlags_=localFlags_; return true; }
    // The first change after READY goes at once (HELLO was the announcement).
    if(linkStateSent_ && !expired(now,linkStateAt_,1000)) return true;
    if(!queue(Op::LinkState,0,b,4)) return false;
    sentFlags_=localFlags_; linkStateAt_=now; linkStateSent_=true; return true;
  }
  // A CLOSE with a reason this class may send (anything else becomes 1).
  bool closeNow(Stream& s,uint32_t now,uint16_t reason) {
    if(s.console ? reason<1 || reason>5 : reason>1) reason=1;
    cancelData(s.id); s.tx.clear(); s.rx.clear(); s.state=StreamState::Closing; s.closeAt=now;
    s.draining=false; s.closeReason=reason; purge(s.id);
    uint8_t b[2]; put16(b,reason); return queue(Op::Close,s.id,b,2);
  }
  void end(Stream& s,Error why) {
    cancelData(s.id); s.state=StreamState::Ended; s.error=why; s.draining=false; s.tx.clear(); s.rx.clear(); purge(s.id);
  }
  bool fail(Error why) { down(why); state=State::Failed; return false; }
  // Not yet started: no fragment accepted and not the frame pending submission.
  bool startedAt(unsigned i) const { return messages_[i].offset || (i==0 && pending_ && pendingControl_); }
  Message* unstarted(Op op,uint16_t id) {
    for(unsigned i=0;i<count_;++i) if(messages_[i].op==op && messages_[i].id==id && !startedAt(i)) return &messages_[i];
    return nullptr;
  }
  // A closing or ended id drops only its unstarted CREDITs. OPEN and CLOSE are
  // never dropped or reordered: the phone fails a CLOSE for an id it never saw.
  void purge(uint16_t id) {
    unsigned kept=0;
    for(unsigned i=0;i<count_;++i) {
      if(messages_[i].op==Op::Credit && messages_[i].id==id && !startedAt(i)) continue;
      if(kept!=i) messages_[kept]=messages_[i];
      ++kept;
    }
    count_=kept;
  }
  bool queue(Op op,uint16_t id,const uint8_t* p,size_t n) {
    if(n>kControl) return fail(Error::Protocol);
    // CREDIT, LINK_STATE and PONG (newest cookie) are snapshots: an unstarted
    // one is overwritten in place and keeps its position.
    if(op==Op::Credit || (!id && (op==Op::LinkState || op==Op::Pong)))
      if(auto* m=unstarted(op,id)) { std::memcpy(m->body,p,n); return true; }
    if(count_==kControlQueue) return fail(Error::Resources);
    auto& m=messages_[count_++]; m=Message{}; m.op=op; m.id=id; m.len=uint16_t(n);
    if(n) std::memcpy(m.body,p,n);
    return true;
  }
  bool control(Op op,uint16_t id,const uint8_t* p,size_t n,uint32_t now) {
    if(!id) {
      if(op==Op::Reset && n==2 && knownError(get16(p))) {peerReset=true;return fail(Error(get16(p)));}
      if(state==State::Hello) {
        if(count_ || pending_ || op!=Op::HelloAck || n!=20 || std::memcmp(p,nonce_,8) || p[16]!=kMaxStreams ||
          (p[17]&uint8_t(~kPhoneFlagMask)) || get16(p+18)<20 || get16(p+18)>value) return fail(Error::Protocol);
        bool nonzero=false; for(unsigned i=8;i<16;++i) nonzero|=p[i]!=0;
        if(!nonzero) return fail(Error::Protocol);
        value=get16(p+18); peerFlags_=p[17]; state=State::Ready; return true;
      }
      if(op==Op::Ping && n==4) return queue(Op::Pong,0,p,n);
      if(op==Op::Pong && n==4 && pingPending_ && get32(p)==pingCookie_) {pingPending_=false;return true;}
      // A full snapshot of the phone's flags. It asks for no answer, so no rate is enforced.
      if(op==Op::LinkState && n==4 && !(get32(p)&~uint32_t(kPhoneFlagMask))) {peerFlags_=p[0];return true;}
      return fail(Error::Protocol);
    }
    if(state!=State::Ready) return fail(Error::Protocol);
    const bool valid=(op==Op::OpenOk && n==4)||(op==Op::OpenErr && n==2 && knownError(get16(p)))||
      (op==Op::Credit && n==4)||(op==Op::Fin && n==4)||(op==Op::Close && n==2)||
      (op==Op::CloseAck && n==0)||(op==Op::Reset && n==2 && knownError(get16(p)));
    if(!valid) return fail(Error::Protocol);
    auto* s=find(id);
    // CLOSE reasons by class: a live relay 0..1, the console 0..5, an ended or
    // retired id 0..5.
    if(op==Op::Close && get16(p)>(s && s->live() && !s->console ? 1 : 5)) return fail(Error::Protocol);
    // The console has no TCP end to report.
    if(op==Op::Fin && s && s->console) return fail(Error::Protocol);
    if(!s || s->state==StreamState::Ended) {
      if(!retired(id) || op==Op::OpenOk || op==Op::OpenErr) return fail(Error::Protocol);
      return op==Op::Close ? queue(Op::CloseAck,id,nullptr,0) : true;
    }
    if(op==Op::Close) {
      cancelData(id);
      if(s->state!=StreamState::Closing)s->closeAt=now;
      s->state=StreamState::Closing; s->error=get16(p)?Error::Cancelled:Error::None; s->peerCloseReason=get16(p);
      s->draining=false; s->tx.clear(); s->rx.clear(); purge(id); return queue(Op::CloseAck,id,nullptr,0);
    }
    if(op==Op::Reset) { end(*s,Error(get16(p))); s->remoteEnd=true; return true; }
    if(s->state==StreamState::Closing) {
      if(op==Op::CloseAck) end(*s,Error::None);
      else if(op==Op::OpenErr) {end(*s,Error(get16(p))); s->remoteEnd=true;}
      // Already submitted OPEN can finish after our cancellation; it cannot revive us.
      return true;
    }
    if(s->state==StreamState::Opening) {
      if(!s->openSubmitted) return fail(Error::Protocol);
      if(op==Op::OpenErr) {end(*s,Error(get16(p)));s->remoteEnd=true;return true;}
      if(op!=Op::OpenOk || !get32(p) || get32(p)>kWindow) return fail(Error::Protocol);
      s->peerLimit=get32(p); s->state=StreamState::Open; s->openedAt=s->pressureAt=s->creditAt=now; return true;
    }
    if(op==Op::Credit) {
      const uint32_t limit=get32(p);
      if(limit<s->peerLimit || uint64_t(limit)>uint64_t(s->sent)+kWindow) return fail(Error::Flow);
      s->peerLimit=limit;return true;
    }
    if(op==Op::Fin && !s->eof && get32(p)==s->received) {s->eof=true;return true;}
    return fail(Error::Protocol);
  }
};
// Three 8 KiB stream windows, 12 control messages and the frame buffers: PSRAM
// (BLE_PhonePipe.cpp places the Session with EXT_RAM_BSS_ATTR).
static_assert(sizeof(Session)<32*1024,"Pipe buffers exceed the P4 PSRAM budget");
} // namespace hw1pipe
