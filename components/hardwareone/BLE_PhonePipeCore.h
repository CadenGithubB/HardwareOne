#pragma once
// Android pipe v1: allocation-free wire/state owner. Call under the adapter's lock.
// No BLE, TLS, credentials, Arduino or OS dependencies.
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
enum class Op : uint8_t { Hello=1, HelloAck=2, Open=0x10, OpenOk=0x11,
  OpenErr=0x12, Data=0x20, Credit=0x21, Fin=0x22, Close=0x23,
  CloseAck=0x24, Reset=0x25, Ping=0x30, Pong=0x31 };
enum class Error : uint16_t { None=0, Protocol=1, Version=2, Unauthorized=3,
  Target=4, Vpn=5, Dns=6, Connect=7, ConnectTimeout=8, Busy=9,
  Resources=10, Tcp=11, Flow=12, Deadline=13, Cancelled=14, Display=15 };
enum class State : uint8_t { Down, Hello, Ready, Failed };
enum class StreamState : uint8_t { Free, Opening, Open, Closing, Ended };
inline uint16_t get16(const uint8_t* p) { return uint16_t(p[0]) | uint16_t(p[1])<<8; }
inline uint32_t get32(const uint8_t* p) { return uint32_t(get16(p)) | uint32_t(get16(p+2))<<16; }
inline void put16(uint8_t* p, uint16_t v) { p[0]=uint8_t(v); p[1]=uint8_t(v>>8); }
inline void put32(uint8_t* p, uint32_t v) { put16(p,uint16_t(v)); put16(p+2,uint16_t(v>>16)); }
inline uint16_t valueLimit(uint16_t mtu) { return mtu<23 ? 0 : uint16_t(mtu>515 ? 512 : mtu-3); }
inline bool expired(uint32_t now, uint32_t start, uint32_t duration) { return uint32_t(now-start)>=duration; }
inline bool knownError(uint16_t e) { return e>=1 && e<=15; }

struct Buffer {
  uint8_t bytes[kWindow]{};
  size_t head=0, size=0;
  size_t space() const { return kWindow-size; }
  void clear() { head=size=0; }
  size_t push(const uint8_t* p,size_t n) {
    if (n>space()) n=space();
    for (size_t i=0;i<n;++i) bytes[(head+size+i)%kWindow]=p[i];
    size+=n; return n;
  }
  size_t peek(uint8_t* p,size_t n) const {
    if (n>size) n=size;
    for(size_t i=0;i<n;++i) p[i]=bytes[(head+i)%kWindow];
    return n;
  }
  void drop(size_t n) { head=(head+n)%kWindow; size-=n; }
  size_t pop(uint8_t* p,size_t n) { n=peek(p,n); drop(n); return n; }
};
struct Stream {
  uint16_t id=0;
  StreamState state=StreamState::Free;
  Error error=Error::None;
  Buffer tx,rx;
  uint32_t sent=0,received=0,consumed=0,peerLimit=0,receiveLimit=0,queuedLimit=0;
  uint32_t creditAt=0,closeAt=0;
  bool eof=false,openSubmitted=false;
  void reset() {
    id=0; state=StreamState::Free; error=Error::None; tx.clear(); rx.clear();
    sent=received=consumed=peerLimit=receiveLimit=queuedLimit=creditAt=closeAt=0;
    eof=openSubmitted=false;
  }
  bool live() const { return state==StreamState::Opening || state==StreamState::Open || state==StreamState::Closing; }
};
struct Message { Op op=Op::Reset; uint16_t id=0,len=0,offset=0; uint8_t body[kControl]{}; };

class Session {
 public:
  State state=State::Down;
  Error error=Error::None;
  Stream streams[2];
  uint16_t value=20;
  uint32_t txSequence=0,rxSequence=0;
  void down(Error why=Error::Connect) {
    state=State::Down; error=why; pending_=false; count_=0; assembling_=false;
    for(auto& s:streams) if(s.live()) end(s,why);
  }
  bool start(uint16_t mtu,const uint8_t nonce[8],uint32_t now) {
    down();
    if (!nonce || !(value=valueLimit(mtu))) return fail(Error::Protocol);
    bool nonzero=false; for(unsigned i=0;i<8;++i) nonzero|=nonce[i]!=0;
    if(!nonzero) return fail(Error::Protocol);
    std::memcpy(nonce_,nonce,8); txSequence=rxSequence=0; nextId_=1;
    for(auto& s:streams) s.reset();
    error=Error::None; state=State::Hello; started_=lastRx_=now; pingPending_=false;
    uint8_t b[12]{}; std::memcpy(b,nonce,8); b[8]=2; put16(b+10,value);
    return queue(Op::Hello,0,b,sizeof(b));
  }
  // The caller retains an ended stream until it consumes its outcome and releases it.
  // This prevents another HTTP worker from replacing an unread failure/EOF.
  Stream* find(uint16_t id) { for(auto& s:streams) if(id && s.id==id) return &s; return nullptr; }
  void release(uint16_t id) { auto* s=find(id); if(s && !s->live()) s->reset(); }
  uint16_t open(uint8_t type,const uint8_t* host,size_t length,uint16_t port,uint32_t now) {
    if(state!=State::Ready || !host || !port || !nextId_ || length>253 ||
       !((type==1 && length==4)||(type==2 && length==16)||(type==3 && length))) return 0;
    if(type==3) for(size_t i=0;i<length;++i)
      if(!((host[i]>='a'&&host[i]<='z')||(host[i]>='0'&&host[i]<='9')||host[i]=='-'||host[i]=='.')) return 0;
    Stream* s=nullptr; for(auto& candidate:streams) if(candidate.state==StreamState::Free) {s=&candidate;break;}
    if(!s) return 0;
    s->reset(); s->id=nextId_++; s->state=StreamState::Opening;
    s->receiveLimit=s->queuedLimit=1024; s->creditAt=now;
    uint8_t b[kControl]{}; put32(b,1024); put16(b+4,port); b[6]=type; b[7]=uint8_t(length);
    std::memcpy(b+8,host,length);
    if(!queue(Op::Open,s->id,b,length+8)) return 0;
    return s->id;
  }
  size_t write(uint16_t id,const uint8_t* p,size_t n) {
    auto* s=find(id); if(state!=State::Ready || !s || s->state!=StreamState::Open || !p) return 0;
    return s->tx.push(p,n);
  }
  size_t read(uint16_t id,uint8_t* p,size_t n) {
    auto* s=find(id); if(!s || !p || s->state!=StreamState::Open) return 0;
    n=s->rx.pop(p,n); s->consumed+=uint32_t(n); return n;
  }
  bool close(uint16_t id,uint32_t now,bool cancelled=false) {
    auto* s=find(id); if(!s || !s->live()) return true;
    if(s->state==StreamState::Closing) return true;
    // An outstanding packet belongs to this link. Never cancel/reassign its sequence.
    if(pending_ && !pendingControl_ && get16(packet_+2)==id) pending_=false;
    s->tx.clear(); s->rx.clear(); s->state=StreamState::Closing; s->closeAt=now;
    uint8_t b[2]; put16(b,cancelled?1:0); return queue(Op::Close,id,b,2);
  }
  // Allocate no work on callbacks. Adapter drives tick and TX in its worker.
  bool tick(uint32_t now,bool displayBusy=false) {
    if(state==State::Hello && expired(now,started_,3000)) return fail(Error::Deadline);
    if(assembling_ && expired(now,assemblyAt_,2000)) return fail(Error::Deadline);
    if(state!=State::Ready) return state!=State::Failed;
    bool live=false;
    for(auto& s:streams) {
      live|=s.live();
      if(s.state==StreamState::Closing && expired(now,s.closeAt,2000)) return fail(Error::Deadline);
      if(s.state!=StreamState::Open || s.eof || displayBusy) continue;
      if(s.consumed>UINT32_MAX-1024) return fail(Error::Flow);
      uint32_t limit=s.consumed+1024;
      if(limit>s.queuedLimit && (limit-s.queuedLimit>=512 || expired(now,s.creditAt,100))) {
        uint8_t b[4]; put32(b,limit);
        if(!queue(Op::Credit,s.id,b,4)) return false;
        s.queuedLimit=limit; s.creditAt=now;
      }
    }
    if(pingPending_ && expired(now,pingAt_,5000)) return fail(Error::Deadline);
    if(live && !pingPending_ && expired(now,lastRx_,5000)) {
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
    if(pending_) { std::memcpy(out,packet_,packetSize_); return packetSize_; }
    size_t n=0; Op op; uint16_t id; uint8_t flags=0;
    if(count_) {
      auto& m=messages_[0]; op=m.op; id=m.id;
      n=m.len-m.offset; if(n>value-kHeader) n=value-kHeader;
      flags=uint8_t((m.offset==0?kFirst:0)|(m.offset+n==m.len?kLast:0));
      std::memcpy(packet_+kHeader,m.body+m.offset,n); pendingControl_=true;
    } else {
      if(!allowData) return 0;
      Stream* s=nullptr;
      for(unsigned i=0;i<2;++i) { auto& c=streams[(round_+i)%2];
        if(c.state==StreamState::Open && c.tx.size && c.peerLimit>c.sent) {s=&c; round_=unsigned((round_+i+1)%2);break;}}
      if(!s) return 0;
      n=value-kHeader; if(n>s->peerLimit-s->sent) n=s->peerLimit-s->sent;
      n=s->tx.peek(packet_+kHeader,n); op=Op::Data; id=s->id; pendingControl_=false;
    }
    packet_[0]=1; packet_[1]=uint8_t(op); put16(packet_+2,id); put32(packet_+4,txSequence);
    put16(packet_+8,uint16_t(n)); packet_[10]=flags; packet_[11]=0;
    packetSize_=n+kHeader; pending_=true; std::memcpy(out,packet_,packetSize_); return packetSize_;
  }
  bool accept() {
    if(!pending_) return false;
    const size_t n=packetSize_-kHeader;
    if(pendingControl_) {
      auto& m=messages_[0]; m.offset=uint16_t(m.offset+n);
      if(m.offset==m.len) {
        if(m.op==Op::Open) {auto* s=find(m.id);if(s)s->openSubmitted=true;}
        if(m.op==Op::Credit) {auto* s=find(m.id);if(s && s->state==StreamState::Open)s->receiveLimit=get32(m.body);}
        for(unsigned i=1;i<count_;++i) messages_[i-1]=messages_[i];
        --count_;
      }
    } else {
      auto* s=find(get16(packet_+2)); if(!s || s->tx.size<n) return fail(Error::Protocol);
      s->tx.drop(n); s->sent+=uint32_t(n);
    }
    ++txSequence; pending_=false; return true;
  }
  bool receive(const uint8_t* p,size_t size,uint32_t now) {
    if(state!=State::Hello && state!=State::Ready) return false;
    if(!p || size<kHeader || size>value || p[0]!=1 || p[11] || get16(p+8)!=size-kHeader ||
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
  Message messages_[8]; unsigned count_=0,round_=0;
  uint16_t nextId_=1; uint8_t nonce_[8]{},packet_[kValue]{},assembly_[kControl]{};
  size_t packetSize_=0,assemblySize_=0; bool pending_=false,pendingControl_=false,assembling_=false;
  Op assemblyOp_=Op::Reset; uint16_t assemblyId_=0;
  uint32_t started_=0,lastRx_=0,assemblyAt_=0,pingCookie_=0,pingAt_=0; bool pingPending_=false;
  bool retired(uint16_t id) const { return id && (!nextId_ || id<nextId_); }
  void end(Stream& s,Error why) { s.state=StreamState::Ended; s.error=why; s.tx.clear(); s.rx.clear(); }
  bool fail(Error why) { down(why); state=State::Failed; return false; }
  bool queue(Op op,uint16_t id,const uint8_t* p,size_t n) {
    if(n>kControl) return fail(Error::Protocol);
    if(op==Op::Credit) for(unsigned i=0;i<count_;++i)
      if(messages_[i].op==op && messages_[i].id==id && messages_[i].offset==0 && !(i==0&&pending_)) {
        std::memcpy(messages_[i].body,p,n); return true;
      }
    if(count_==8) return fail(Error::Resources);
    auto& m=messages_[count_++]; m=Message{}; m.op=op; m.id=id; m.len=uint16_t(n);
    if(n) std::memcpy(m.body,p,n);
    return true;
  }
  bool control(Op op,uint16_t id,const uint8_t* p,size_t n,uint32_t now) {
    if(!id) {
      if(op==Op::Reset && n==2 && knownError(get16(p))) return fail(Error(get16(p)));
      if(state==State::Hello) {
        if(count_ || pending_ || op!=Op::HelloAck || n!=20 || std::memcmp(p,nonce_,8) || p[16]!=2 || p[17] ||
          get16(p+18)<20 || get16(p+18)>value) return fail(Error::Protocol);
        bool nonzero=false; for(unsigned i=8;i<16;++i) nonzero|=p[i]!=0;
        if(!nonzero) return fail(Error::Protocol);
        value=get16(p+18); state=State::Ready; return true;
      }
      if(op==Op::Ping && n==4) return queue(Op::Pong,0,p,n);
      if(op==Op::Pong && n==4 && pingPending_ && get32(p)==pingCookie_) {pingPending_=false;return true;}
      return fail(Error::Protocol);
    }
    if(state!=State::Ready) return fail(Error::Protocol);
    const bool valid=(op==Op::OpenOk && n==4)||(op==Op::OpenErr && n==2 && knownError(get16(p)))||
      (op==Op::Credit && n==4)||(op==Op::Fin && n==4)||(op==Op::Close && n==2 && get16(p)<=1)||
      (op==Op::CloseAck && n==0)||(op==Op::Reset && n==2 && knownError(get16(p)));
    if(!valid) return fail(Error::Protocol);
    auto* s=find(id);
    if(!s || s->state==StreamState::Ended) {
      if(!retired(id) || op==Op::OpenOk || op==Op::OpenErr) return fail(Error::Protocol);
      return op==Op::Close ? queue(Op::CloseAck,id,nullptr,0) : true;
    }
    if(op==Op::Close) { end(*s,get16(p)?Error::Cancelled:Error::None); return queue(Op::CloseAck,id,nullptr,0); }
    if(op==Op::Reset) { end(*s,Error(get16(p))); return true; }
    if(s->state==StreamState::Closing) {
      if(op==Op::CloseAck) end(*s,Error::None);
      // Already submitted OPEN can finish after our cancellation; it cannot revive us.
      return true;
    }
    if(s->state==StreamState::Opening) {
      if(!s->openSubmitted) return fail(Error::Protocol);
      if(op==Op::OpenErr) {end(*s,Error(get16(p)));return true;}
      if(op!=Op::OpenOk || !get32(p) || get32(p)>kWindow) return fail(Error::Protocol);
      s->peerLimit=get32(p); s->state=StreamState::Open; s->creditAt=now; return true;
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
static_assert(sizeof(Session)<24*1024,"Pipe buffers exceed P4 budget");
} // namespace hw1pipe
