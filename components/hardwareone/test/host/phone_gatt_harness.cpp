#include "phone_gatt_fake_sdk.h"
#include "phone_secure_peer.h"
#include <algorithm>
#include <cstdio>
static PhoneSecurePeer peer;
static uint8_t psk[32]={1};
static void begin(Gatt& g) {beginWith(g,psk);}
// Secure handshake, then our pipe HELLO (v2: max_streams 3, the P4 flags)
// fully submitted; returns when the phone's answer is due.
static void hello(Gatt& g,uint16_t mtu=247) {
  begin(g);opened(g);discover(g,mtu);subscribed(g);
  peer=PhoneSecurePeer{};auto hello=peer.hello();notify(g,hello);tick(g,30);
  assert(writes.back().size()==49&&writes.back()[0]==2&&!g.ready());
  auto confirm=peer.confirm(writes.back(),psk);writeDone(g);notify(g,confirm);tick(g,30);
  assert(peer.confirmAck(writes.back())&&!g.ready());writeDone(g);
  Bytes plaintext=peer.open(writes.back()),body;
  for(;;) {
    assert(plaintext[0]==hw1pipe::kVersion && plaintext[1]==uint8_t(hw1pipe::Op::Hello));
    body.insert(body.end(),plaintext.begin()+12,plaintext.end());
    if(plaintext[10]&hw1pipe::kLast)break;
    writeDone(g);plaintext=peer.open(writes.back());
  }
  assert(body.size()==12 && body[8]==hw1pipe::kMaxStreams && body[9]==g.pipe.localFlags());
  writeDone(g);assert(g.pipe.state==hw1pipe::State::Hello);
}
// One inner frame from the phone (v2 header unless `version` says otherwise).
static void phoneFrame(Gatt& g,hw1pipe::Op op,uint16_t id,const Bytes& body,uint8_t flags=3,uint8_t version=hw1pipe::kVersion) {
  Bytes bytes(12+body.size());bytes[0]=version;bytes[1]=uint8_t(op);hw1pipe::put16(bytes.data()+2,id);
  hw1pipe::put32(bytes.data()+4,g.pipe.rxSequence);hw1pipe::put16(bytes.data()+8,uint16_t(body.size()));
  bytes[10]=flags;if(!body.empty())std::memcpy(bytes.data()+12,body.data(),body.size());
  bytes=peer.seal(bytes);notify(g,bytes);tick(g);
}
static void ready(Gatt& g,uint16_t mtu=247,uint8_t phoneFlags=hw1pipe::kPhoneRelayOn|hw1pipe::kPhoneConsoleWanted) {
  hello(g,mtu);
  uint8_t body[20]{};std::memset(body,0x51,8);std::memset(body+8,0x22,8);body[16]=hw1pipe::kMaxStreams;body[17]=phoneFlags;
  hw1pipe::put16(body+18,(hw1pipe::valueLimit(mtu)-25));
  unsigned seq=0;size_t offset=0;
  while(offset<20) {
    size_t n=std::min(size_t((hw1pipe::valueLimit(mtu)-25)-12),20-offset);
    Bytes bytes(12+n);bytes[0]=hw1pipe::kVersion;bytes[1]=uint8_t(hw1pipe::Op::HelloAck);
    hw1pipe::put32(bytes.data()+4,seq++);hw1pipe::put16(bytes.data()+8,uint16_t(n));
    bytes[10]=uint8_t((offset==0?1:0)|(offset+n==20?2:0));
    std::memcpy(bytes.data()+12,body+offset,n);bytes=peer.seal(bytes);notify(g,bytes);tick(g);offset+=n;
  }
  assert(g.ready() && g.pipe.peerFlags()==phoneFlags);
}
static void finish(Gatt& g) {
  g.stop(now);tick(g);assert(calls.back()=="close");assert(!g.ready());
  esp_ble_gattc_cb_param_t e{};e.close.conn_id=4;post(g,ESP_GATTC_CLOSE_EVT,e);tick(g);
  assert(calls.back()=="unregister" && !g.idle());
  assert(g.post(ESP_GATTC_UNREG_EVT,7,nullptr));tick(g);assert(g.idle());
}
int main() {
  // Bench S8 (57a0febd): IDF 5.5.5 queues the CFG_MTU sent at OPEN behind
  // Bluedroid's automatic discovery, with no callback for more than 2 s; it
  // runs once DIS_SRVC_CMPL is posted. The phase waits without a second
  // Exchange MTU and proceeds when the queued request completes (clock wrap too).
  for(bool wrap:{false,true}) {
    now=wrap?UINT32_MAX-1500:100;
    hw1pipe::Session session;Gatt::Event slabs[Gatt::kSlabCount]{};Gatt g{session,slabs};begin(g);
    const size_t before=calls.size();opened(g);
    tick(g,3000);assert(g.phase()==Phase::Mtu);
    esp_ble_gattc_cb_param_t d{};d.dis_srvc_cmpl.conn_id=4;post(g,ESP_GATTC_DIS_SRVC_CMPL_EVT,d);tick(g);
    tick(g,1900);assert(g.phase()==Phase::Mtu);
    discover(g,244);assert(std::count(calls.begin()+before,calls.end(),"mtu")==1); // one per link
    subscribed(g);finish(g);
  }
  // After discovery, an unanswered MTU request fails 2 s later (clock wrap too).
  for(bool wrap:{false,true}) {
    now=wrap?UINT32_MAX-1000:100;
    hw1pipe::Session session;Gatt::Event slabs[Gatt::kSlabCount]{};Gatt g{session,slabs};begin(g);opened(g);tick(g,500);
    esp_ble_gattc_cb_param_t d{};d.dis_srvc_cmpl.conn_id=4;post(g,ESP_GATTC_DIS_SRVC_CMPL_EVT,d);tick(g);
    const uint32_t doneAt=now;const size_t n=calls.size();
    while(g.phase()==Phase::Mtu) tick(g,50);
    assert(uint32_t(now-doneAt)>=2000 && uint32_t(now-doneAt)<2100);
    assert(std::count(calls.begin()+n,calls.end(),"mtu")==0);
    assert(g.pipe.error==hw1pipe::Error::Deadline && !g.authenticationFailed());finish(g);
  }
  // With no discovery event and no answer, Phase::Mtu still ends at 10 s
  // (clock wrap too).
  for(bool wrap:{false,true}) {
    now=wrap?UINT32_MAX-5000:100;
    hw1pipe::Session session;Gatt::Event slabs[Gatt::kSlabCount]{};Gatt g{session,slabs};begin(g);opened(g);
    const uint32_t openedAt=now-100;
    while(g.phase()==Phase::Mtu) tick(g,50);
    assert(uint32_t(now-openedAt)>=10000 && uint32_t(now-openedAt)<10100);
    assert(g.pipe.error==hw1pipe::Error::Deadline && !g.authenticationFailed());finish(g);
  }
  // Discovery events for another connection or another app's interface are
  // not ours: the 10 s wait continues instead of the 2 s one.
  {
    now=100;
    hw1pipe::Session session;Gatt::Event slabs[Gatt::kSlabCount]{};Gatt g{session,slabs};begin(g);opened(g);
    esp_ble_gattc_cb_param_t d{};d.dis_srvc_cmpl.conn_id=5;post(g,ESP_GATTC_DIS_SRVC_CMPL_EVT,d);
    d.dis_srvc_cmpl.conn_id=4;assert(!g.post(ESP_GATTC_DIS_SRVC_CMPL_EVT,9,&d));tick(g);
    const size_t n=calls.size();tick(g,2500);assert(calls.size()==n && g.phase()==Phase::Mtu);
    discover(g,244);subscribed(g);finish(g);
  }
  // Cancellation, disconnect, a refused MTU submission and an undersized
  // negotiation all fail safely, without charging authentication.
  for(int kind=0;kind<4;++kind) {
    hw1pipe::Session session;Gatt::Event slabs[Gatt::kSlabCount]{};Gatt g{session,slabs};begin(g);
    if(kind==2) {
      failApi="mtu";esp_ble_gattc_cb_param_t e{};e.open.conn_id=4;std::memcpy(e.open.remote_bda,address,6);
      post(g,ESP_GATTC_OPEN_EVT,e);tick(g);failApi.clear();assert(g.pipe.error==hw1pipe::Error::Connect);
    } else {
      opened(g);
      esp_ble_gattc_cb_param_t e{};const size_t n=calls.size();
      if(kind==0) {g.stop(now);tick(g,50);assert(calls.size()==n+1 && calls.back()=="close");}
      if(kind==1) {e.disconnect.conn_id=4;post(g,ESP_GATTC_DISCONNECT_EVT,e);tick(g,50);
        assert(calls.size()==n+1 && calls.back()=="unregister");}
      if(kind==3) {e.cfg_mtu.conn_id=4;e.cfg_mtu.status=ESP_GATT_OK;e.cfg_mtu.mtu=23;
        post(g,ESP_GATTC_CFG_MTU_EVT,e);tick(g);assert(g.pipe.error==hw1pipe::Error::Connect);}
    }
    assert(g.phase()==Phase::Retiring && !g.authenticationFailed());g.hostStopped();
  }
  // ---- The attempt's end, for the coexistence record ------------------------
  // Latched once, by the first stop after a successful registration.
  using S=hw1coex::ConnectStage;
  const auto disconnect=[](Gatt& g,uint16_t conn,uint8_t reason,bool phone) {
    esp_ble_gattc_cb_param_t d{};d.disconnect.conn_id=conn;d.disconnect.reason=reason;
    std::memcpy(d.disconnect.remote_bda,address,6);if(!phone)d.disconnect.remote_bda[5]^=0x5a;
    post(g,ESP_GATTC_DISCONNECT_EVT,d);tick(g);
  };
  const auto autoDiscovered=[](Gatt& g,int status=ESP_GATT_OK) {
    esp_ble_gattc_cb_param_t d{};d.dis_srvc_cmpl.conn_id=4;d.dis_srvc_cmpl.status=esp_gatt_status_t(status);
    post(g,ESP_GATTC_DIS_SRVC_CMPL_EVT,d);tick(g);
  };
  // G1: a failed open. Bluedroid's DISCONNECT for the phone (0x3e after its
  // silent retries) precedes the generic OPEN 0x85; another link's copy (a G2
  // temple's 0x08) is not the phone's; the first reason wins.
  {now=100;hw1pipe::Session session;Gatt::Event slabs[Gatt::kSlabCount]{};Gatt g{session,slabs};begin(g);assert(!g.attemptEnd());
    disconnect(g,2,0x08,false);disconnect(g,0xff,0x3e,true);disconnect(g,0xff,0x22,true);
    assert(g.phase()==Phase::Opening && !g.attemptEnd());
    esp_ble_gattc_cb_param_t o{};o.open.status=esp_gatt_status_t(0x85);std::memcpy(o.open.remote_bda,address,6);
    post(g,ESP_GATTC_OPEN_EVT,o);tick(g);
    const auto* e=g.attemptEnd();
    assert(e && e->stage==S::Open && e->error==hw1pipe::Error::Connect && e->gatt==0x85 && e->reason==0x3e && e->at==now);
    assert(g.phase()==Phase::Retiring);g.hostStopped();}
  // An open the stack refuses at once: stage open, no status, no reason.
  const auto registered=[](Gatt& g,int status=ESP_GATT_OK) {
    assert(g.begin(42,address,1,now,psk));
    esp_ble_gattc_cb_param_t e{};e.reg.app_id=42;e.reg.status=esp_gatt_status_t(status);post(g,ESP_GATTC_REG_EVT,e);tick(g);
  };
  {now=100;hw1pipe::Session session;Gatt::Event slabs[Gatt::kSlabCount]{};Gatt g{session,slabs};failApi="open";registered(g);failApi.clear();
    const auto* e=g.attemptEnd();
    assert(e && e->stage==S::Open && e->error==hw1pipe::Error::Connect && e->gatt==-1 && !e->reason);g.hostStopped();}
  // G2: the live link drops during Bluedroid's automatic discovery (new: the
  // reason after the link is up); our own close's DISCONNECT changes nothing.
  {now=100;hw1pipe::Session session;Gatt::Event slabs[Gatt::kSlabCount]{};Gatt g{session,slabs};begin(g);opened(g);tick(g,3000);
    disconnect(g,4,0x08,true);
    const auto* e=g.attemptEnd();
    assert(e && e->stage==S::Discovery && e->error==hw1pipe::Error::Connect && e->reason==0x08 && e->gatt==-1);
    disconnect(g,4,0x16,true);assert(g.attemptEnd()->reason==0x08);g.hostStopped();}
  // G3: no DIS_SRVC_CMPL within 10 s ends at discovery as our deadline; later
  // stops (our close, an overflow) never overwrite the first.
  {now=100;hw1pipe::Session session;Gatt::Event slabs[Gatt::kSlabCount]{};Gatt g{session,slabs};begin(g);opened(g);
    while(g.phase()==Phase::Mtu)tick(g,50);
    const auto* e=g.attemptEnd();
    assert(e && e->stage==S::Discovery && e->error==hw1pipe::Error::Deadline && !e->reason);
    const uint32_t at=e->at;
    g.stop(now,hw1pipe::Error::Resources);disconnect(g,4,0x16,true);
    assert(g.attemptEnd()->error==hw1pipe::Error::Deadline && g.attemptEnd()->at==at && !g.attemptEnd()->reason);
    assert(g.pipe.error==hw1pipe::Error::Resources);g.hostStopped();}
  // G4: discovery reports a failure: its status is kept, and the attempt
  // reads discovery wherever it then ends.
  {now=100;hw1pipe::Session session;Gatt::Event slabs[Gatt::kSlabCount]{};Gatt g{session,slabs};begin(g);opened(g);autoDiscovered(g,0x85);
    assert(g.phase()==Phase::Mtu && !g.attemptEnd());
    // The queued MTU answer runs; our search over the partial cache finds nothing.
    esp_ble_gattc_cb_param_t e{};e.cfg_mtu.conn_id=4;e.cfg_mtu.mtu=247;post(g,ESP_GATTC_CFG_MTU_EVT,e);tick(g);
    e.search_cmpl.conn_id=4;e.search_cmpl.status=esp_gatt_status_t(0x81);post(g,ESP_GATTC_SEARCH_CMPL_EVT,e);tick(g);
    const auto* end=g.attemptEnd();  // the first non-OK status wins (not the search's 0x81)
    assert(end && end->stage==S::Discovery && end->gatt==0x85 && end->error==hw1pipe::Error::Protocol);g.hostStopped();}
  {now=100;hw1pipe::Session session;Gatt::Event slabs[Gatt::kSlabCount]{};Gatt g{session,slabs};begin(g);opened(g);autoDiscovered(g,0x85);
    g.stop(now,hw1pipe::Error::Cancelled);assert(g.attemptEnd()->stage==S::Discovery);g.hostStopped();}
  // Our CFG_MTU ran without DIS_SRVC_CMPL: Bluedroid's discovery ended
  // without reporting, so a failure in our search still reads discovery...
  {now=100;hw1pipe::Session session;Gatt::Event slabs[Gatt::kSlabCount]{};Gatt g{session,slabs};begin(g);opened(g);
    esp_ble_gattc_cb_param_t e{};e.cfg_mtu.conn_id=4;e.cfg_mtu.mtu=247;post(g,ESP_GATTC_CFG_MTU_EVT,e);tick(g);
    assert(g.phase()==Phase::Discovering);
    e.search_cmpl.conn_id=4;post(g,ESP_GATTC_SEARCH_CMPL_EVT,e);tick(g);
    assert(g.attemptEnd()->stage==S::Discovery && g.attemptEnd()->gatt==-1);g.hostStopped();}
  // ...while after a clean discovery it reads search.
  {now=100;hw1pipe::Session session;Gatt::Event slabs[Gatt::kSlabCount]{};Gatt g{session,slabs};begin(g);opened(g);autoDiscovered(g);
    esp_ble_gattc_cb_param_t e{};e.cfg_mtu.conn_id=4;e.cfg_mtu.mtu=247;post(g,ESP_GATTC_CFG_MTU_EVT,e);tick(g);
    assert(g.phase()==Phase::Discovering);
    e.search_cmpl.conn_id=4;e.search_cmpl.status=esp_gatt_status_t(0x81);post(g,ESP_GATTC_SEARCH_CMPL_EVT,e);tick(g);
    const auto* end=g.attemptEnd();
    assert(end && end->stage==S::Search && end->error==hw1pipe::Error::Protocol && end->gatt==0x81);g.hostStopped();}
  // G5: after a clean discovery, no MTU answer within 2 s reads mtu; a
  // refused MTU request reads mtu too.
  {now=100;hw1pipe::Session session;Gatt::Event slabs[Gatt::kSlabCount]{};Gatt g{session,slabs};begin(g);opened(g);autoDiscovered(g);
    while(g.phase()==Phase::Mtu)tick(g,50);
    assert(g.attemptEnd()->stage==S::Mtu && g.attemptEnd()->error==hw1pipe::Error::Deadline);g.hostStopped();}
  {now=100;hw1pipe::Session session;Gatt::Event slabs[Gatt::kSlabCount]{};Gatt g{session,slabs};begin(g);failApi="mtu";
    esp_ble_gattc_cb_param_t e{};e.open.conn_id=4;std::memcpy(e.open.remote_bda,address,6);
    post(g,ESP_GATTC_OPEN_EVT,e);tick(g);failApi.clear();
    assert(g.attemptEnd()->stage==S::Mtu && g.attemptEnd()->error==hw1pipe::Error::Connect);g.hostStopped();}
  // G6: subscribe, and a CCCD write still unanswered: the phone has not seen
  // a subscription yet (stage cccd, not secure).
  {now=100;hw1pipe::Session session;Gatt::Event slabs[Gatt::kSlabCount]{};Gatt g{session,slabs};begin(g);opened(g);autoDiscovered(g);
    esp_ble_gattc_cb_param_t e{};e.cfg_mtu.conn_id=4;e.cfg_mtu.mtu=247;post(g,ESP_GATTC_CFG_MTU_EVT,e);tick(g);
    e={};e.search_res.conn_id=4;e.search_res.start_handle=10;e.search_res.end_handle=20;e.search_res.is_primary=true;
    e.search_res.srvc_id.uuid.len=16;
    const uint8_t uuid[16]={0xa0,0xc6,0x21,0x50,0x9f,0x3d,0xc4,0xa8,0x2e,0x4b,0x7d,0x6f,0,0,0xe1,0xb7};
    std::memcpy(e.search_res.srvc_id.uuid.uuid.uuid128,uuid,16);post(g,ESP_GATTC_SEARCH_RES_EVT,e);
    e.search_cmpl.conn_id=4;post(g,ESP_GATTC_SEARCH_CMPL_EVT,e);tick(g);
    assert(g.phase()==Phase::Subscribe);
    e={};e.reg_for_notify.handle=13;post(g,ESP_GATTC_REG_FOR_NOTIFY_EVT,e);tick(g,30);
    assert(g.phase()==Phase::Secure);  // the CCCD write is out, not answered
    disconnect(g,4,0x08,true);
    assert(g.attemptEnd()->stage==S::Cccd && g.attemptEnd()->reason==0x08);g.hostStopped();}
  {now=100;hw1pipe::Session session;Gatt::Event slabs[Gatt::kSlabCount]{};Gatt g{session,slabs};begin(g);opened(g);autoDiscovered(g);discover(g);
    esp_ble_gattc_cb_param_t e{};e.reg_for_notify.handle=13;e.reg_for_notify.status=esp_gatt_status_t(0x80);
    post(g,ESP_GATTC_REG_FOR_NOTIFY_EVT,e);tick(g);
    assert(g.attemptEnd()->stage==S::Subscribe && g.attemptEnd()->gatt==0x80);g.hostStopped();}
  {now=100;hw1pipe::Session session;Gatt::Event slabs[Gatt::kSlabCount]{};Gatt g{session,slabs};begin(g);opened(g);autoDiscovered(g);discover(g);
    esp_ble_gattc_cb_param_t e{};e.reg_for_notify.handle=13;post(g,ESP_GATTC_REG_FOR_NOTIFY_EVT,e);tick(g,30);
    e={};e.write.conn_id=4;e.write.handle=14;e.write.status=esp_gatt_status_t(0x0d);post(g,ESP_GATTC_WRITE_DESCR_EVT,e);tick(g);
    assert(g.attemptEnd()->stage==S::Cccd && g.attemptEnd()->gatt==0x0d && g.attemptEnd()->error==hw1pipe::Error::Protocol);
    g.hostStopped();}
  // G7: Service Changed is marked.
  {now=100;hw1pipe::Session session;Gatt::Event slabs[Gatt::kSlabCount]{};Gatt g{session,slabs};begin(g);opened(g);
    esp_ble_gattc_cb_param_t e{};std::memcpy(e.srvc_chg.remote_bda,address,6);post(g,ESP_GATTC_SRVC_CHG_EVT,e);tick(g);
    assert(g.attemptEnd()->serviceChanged && g.attemptEnd()->error==hw1pipe::Error::Protocol);g.hostStopped();}
  // G8: a refused registration ends there with its status; the 2 s
  // registration deadline reads register too.
  {now=100;hw1pipe::Session session;Gatt::Event slabs[Gatt::kSlabCount]{};Gatt g{session,slabs};registered(g,0x85);
    assert(g.idle() && g.attemptEnd() && g.attemptEnd()->stage==S::RegisterEvent && g.attemptEnd()->gatt==0x85 &&
           g.attemptEnd()->error==hw1pipe::Error::Connect);}
  {now=100;hw1pipe::Session session;Gatt::Event slabs[Gatt::kSlabCount]{};Gatt g{session,slabs};assert(g.begin(42,address,1,now,psk));
    while(g.phase()==Phase::Registering)tick(g,50);
    assert(g.attemptEnd()->stage==S::RegisterEvent && g.attemptEnd()->error==hw1pipe::Error::Deadline);g.hostStopped();}
  // G9: an unexpected UNREG while opening.
  {now=100;hw1pipe::Session session;Gatt::Event slabs[Gatt::kSlabCount]{};Gatt g{session,slabs};begin(g);
    assert(g.post(ESP_GATTC_UNREG_EVT,7,nullptr));tick(g);
    assert(g.attemptEnd() && g.attemptEnd()->stage==S::Open && g.attemptEnd()->error==hw1pipe::Error::Protocol);}
  // G10: a READY session has no end yet; begin() clears the last attempt's
  // end, a refused registration call arms nothing, and a host stop disarms.
  {now=100;hw1pipe::Session session;Gatt::Event slabs[Gatt::kSlabCount]{};Gatt g{session,slabs};ready(g);assert(!g.attemptEnd());finish(g);
    assert(g.attemptEnd() && g.attemptEnd()->stage==S::Hello);
    begin(g);assert(!g.attemptEnd());g.hostStopped();g.stop(now,hw1pipe::Error::Connect);assert(!g.attemptEnd());
    g.hostStopped();failApi="register";assert(!g.begin(42,address,1,now,psk));failApi.clear();
    g.stop(now,hw1pipe::Error::Connect);assert(!g.attemptEnd());}
  // G11: the first stop's cause stands.
  {now=100;hw1pipe::Session session;Gatt::Event slabs[Gatt::kSlabCount]{};Gatt g{session,slabs};begin(g);opened(g);
    g.stop(now,hw1pipe::Error::Deadline);g.stop(now,hw1pipe::Error::Cancelled);
    assert(g.attemptEnd()->error==hw1pipe::Error::Deadline);g.hostStopped();}
  // An authenticated frame the pipe refuses keeps the core's own cause; only
  // frames that fail authentication end as Unauthorized (firmware-007; the
  // phone's RESET used to read as end reason 3).
  // The setup breakdown's stamps: every phase up to Pipe, in order, and no
  // discovery time when Bluedroid never reported one.
  {now=100;hw1pipe::Session session;Gatt::Event slabs[Gatt::kSlabCount]{};Gatt g{session,slabs};ready(g);
    uint32_t last=g.enteredAt(Phase::Registering);assert(last==100);
    for(Phase p:{Phase::Opening,Phase::Mtu,Phase::Discovering,Phase::Subscribe,Phase::Cccd,Phase::Secure,Phase::Pipe})
      {assert(g.enteredAt(p)>=last);last=g.enteredAt(p);}
    assert(g.enteredAt(Phase::Pipe)>g.enteredAt(Phase::Registering) && !g.discoveredAt());
    assert(!g.enteredAt(Phase::Retiring));finish(g);
    // A new attempt starts from zero.
    const uint32_t at=now;begin(g);assert(g.enteredAt(Phase::Registering)==at && !g.enteredAt(Phase::Mtu));g.stop(now);tick(g);g.hostStopped();}
  for(int kind=0;kind<2;++kind) {
    hw1pipe::Session session;Gatt::Event slabs[Gatt::kSlabCount]{};Gatt g{session,slabs};ready(g);
    Bytes b(14);b[0]=hw1pipe::kVersion;b[1]=uint8_t(hw1pipe::Op::Reset);
    hw1pipe::put32(b.data()+4,g.pipe.rxSequence+uint32_t(kind));  // kind 1: a sequence gap
    hw1pipe::put16(b.data()+8,2);b[10]=3;hw1pipe::put16(b.data()+12,14);
    b=peer.seal(b);notify(g,b);tick(g);
    assert(!g.ready() && !g.authenticationFailed());
    if(kind==0)assert(g.pipe.error==hw1pipe::Error::Cancelled && g.pipe.peerReset);
    else assert(g.pipe.error==hw1pipe::Error::Protocol && !g.pipe.peerReset);
    finish(g);
  }
  // Pressure never gates OPEN/control. DATA and CREDIT share one 2 s
  // budget, then submit through the real adapter even while pressure persists.
  {hw1pipe::Session session;Gatt::Event slabs[Gatt::kSlabCount]{};Gatt g{session,slabs};ready(g);
    const uint8_t host[]={192,0,2,1};auto id=g.pipe.open(1,host,4,443,now);assert(id);
    auto n=writes.size();tick(g,30,true);assert(writes.size()==n+1);
    assert(peer.open(writes.back())[1]==uint8_t(hw1pipe::Op::Open));writeDone(g,true);
    Bytes packet(16);packet[0]=hw1pipe::kVersion;packet[1]=uint8_t(hw1pipe::Op::OpenOk);
    hw1pipe::put16(packet.data()+2,id);hw1pipe::put32(packet.data()+4,g.pipe.rxSequence);
    hw1pipe::put16(packet.data()+8,4);packet[10]=3;hw1pipe::put32(packet.data()+12,1024);
    assert(g.pipe.receive(packet.data(),packet.size(),now));assert(g.pipe.find(id)->openedAt==now);
    uint8_t payload[16]{};assert(g.pipe.write(id,payload,16)==16);
    packet[1]=uint8_t(hw1pipe::Op::Data);packet[10]=0;
    hw1pipe::put32(packet.data()+4,g.pipe.rxSequence);
    assert(g.pipe.receive(packet.data(),packet.size(),now));assert(g.pipe.read(id,payload,4)==4);
    tick(g,30,true);n=writes.size();tick(g,1999,true);assert(writes.size()==n);
    gate=false;tick(g,1,true);assert(writes.size()==n); // Still cannot steal the TX mutex.
    gate=true;tick(g,1,true);assert(writes.size()==n+1);
    assert(peer.open(writes.back())[1]==uint8_t(hw1pipe::Op::Credit));
    writeDone(g,true);assert(writes.size()==n+2);
    assert(peer.open(writes.back())[1]==uint8_t(hw1pipe::Op::Data));writeDone(g,true);
    finish(g);
  }
  // Congested NO_RSP completions are accepted exactly once, including both
  // confirmations and fragmented pipe HELLO; peer counters catch replays.
  { hw1pipe::Session session; Gatt::Event slabs[Gatt::kSlabCount]{};Gatt g{session,slabs};completionStatus=ESP_GATT_CONGESTED;
    ready(g,64);assert(!g.authenticationFailed());finish(g);completionStatus=ESP_GATT_OK; }
  // A transport disconnect or a genuine rejected write during authentication
  // must not charge the key-failure cooldown.
  for(int kind=0;kind<3;++kind) {
    hw1pipe::Session session;Gatt::Event slabs[Gatt::kSlabCount]{};Gatt g{session,slabs};begin(g);opened(g);discover(g);subscribed(g);
    esp_ble_gattc_cb_param_t e{};
    if(kind==0){e.disconnect.conn_id=4;post(g,ESP_GATTC_DISCONNECT_EVT,e);tick(g);}
    else if(kind==1){g.stop(now,hw1pipe::Error::Resources);tick(g);}
    else {peer=PhoneSecurePeer{};auto hello=peer.hello();notify(g,hello);tick(g,30);
      e.write.conn_id=4;e.write.handle=11;e.write.status=ESP_GATT_BUSY;
      post(g,ESP_GATTC_WRITE_CHAR_EVT,e);tick(g);}
    assert(!g.authenticationFailed()&&!g.ready());g.hostStopped();
  }
  // Congestion pauses fresh writes, clearing it resumes without replaying
  // the preceding accepted encrypted value.
  {hw1pipe::Session session;Gatt::Event slabs[Gatt::kSlabCount]{};Gatt g{session,slabs};ready(g);
    esp_ble_gattc_cb_param_t e{};e.congest.conn_id=4;e.congest.congested=true;
    post(g,ESP_GATTC_CONGEST_EVT,e);tick(g);size_t n=writes.size();
    const uint8_t host[]={192,0,2,1};assert(g.pipe.open(1,host,4,443,now));
    tick(g,100);assert(writes.size()==n&&g.ready());
    e.congest.congested=false;post(g,ESP_GATTC_CONGEST_EVT,e);tick(g);
    assert(g.ready()&&!g.authenticationFailed()&&writes.size()==n+1);
    auto frame=peer.open(writes.back());assert(frame[1]==uint8_t(hw1pipe::Op::Open));
    writeDone(g);assert(writes.size()==n+1);finish(g);}

  for(uint16_t small:{23,52,63}) {
    hw1pipe::Session session; Gatt::Event slabs[Gatt::kSlabCount]{};Gatt g{session,slabs};begin(g);opened(g);esp_ble_gattc_cb_param_t e{};e.cfg_mtu.conn_id=4;e.cfg_mtu.mtu=small;
    post(g,ESP_GATTC_CFG_MTU_EVT,e);tick(g);assert(g.phase()==Phase::Retiring);finish(g);
  }
  for(uint16_t mtu:{64,244,247,517}) {
    hw1pipe::Session session; Gatt::Event slabs[Gatt::kSlabCount]{};Gatt g{session,slabs};ready(g,mtu);uint32_t generation=g.generation();
    esp_ble_gattc_cb_param_t wrong{};assert(!g.post(ESP_GATTC_NOTIFY_EVT,8,&wrong));
    finish(g);begin(g);assert(g.generation()!=generation);g.hostStopped();assert(g.idle());
  }
  // A peer offering only WRITE (with response) is incompatible with D2.
  { hw1pipe::Session session; Gatt::Event slabs[Gatt::kSlabCount]{};Gatt g{session,slabs};begin(g);opened(g);properties=ESP_GATT_CHAR_PROP_BIT_WRITE;discover(g);
    assert(g.phase()==Phase::Retiring);properties=0xff;finish(g); }
  for(bool blocked:{false,true}) {
    hw1pipe::Session session; Gatt::Event slabs[Gatt::kSlabCount]{};Gatt g{session,slabs};begin(g);opened(g);discover(g);subscribed(g);peer=PhoneSecurePeer{};
    auto hello=peer.hello();notify(g,hello);tick(g,30);uint8_t wrong[32]={9};
    auto confirm=peer.confirm(writes.back(),wrong);writeDone(g);gate=!blocked;
    const size_t before=writes.size();notify(g,confirm);tick(g,30);
    if(blocked){assert(g.phase()==Phase::Secure&&writes.size()==before);tick(g,251);gate=true;}
    else {assert(writes.back()==Bytes({5,2})&&writes.size()==before+1);}
    assert(g.phase()==Phase::Retiring&&g.authenticationFailed()&&!g.ready());finish(g);
  }
  { hw1pipe::Session session; Gatt::Event slabs[Gatt::kSlabCount]{};Gatt g{session,slabs};begin(g);opened(g);discover(g);subscribed(g);peer=PhoneSecurePeer{};
    auto hello=peer.hello();notify(g,hello);tick(g,30);const size_t before=writes.size();
    tick(g,2001);assert(g.phase()==Phase::Retiring&&!g.authenticationFailed()&&writes.size()==before);finish(g); }
  { hw1pipe::Session session; Gatt::Event slabs[Gatt::kSlabCount]{};Gatt g{session,slabs};begin(g);opened(g);charCount=2;discover(g);assert(g.phase()==Phase::Retiring);charCount=1;finish(g); }
  { hw1pipe::Session session; Gatt::Event slabs[Gatt::kSlabCount]{};Gatt g{session,slabs};begin(g);opened(g);properties=0;discover(g);assert(g.phase()==Phase::Retiring);properties=0xff;finish(g); }
  { hw1pipe::Session session; Gatt::Event slabs[Gatt::kSlabCount]{};Gatt g{session,slabs};begin(g);opened(g);discover(g);subscribed(g);size_t n=writes.size();
    tick(g,5001);assert(!g.ready() && g.phase()==Phase::Retiring && g.authenticationFailed() && writes.size()==n);finish(g); }
  { hw1pipe::Session session; Gatt::Event slabs[Gatt::kSlabCount]{};Gatt g{session,slabs};ready(g);size_t n=writes.size();tick(g,100,true);assert(writes.size()==n);finish(g); }
  { hw1pipe::Session session; Gatt::Event slabs[Gatt::kSlabCount]{};Gatt g{session,slabs};assert(g.begin(42,address,1,now,psk));g.stop(now);tick(g,4000);assert(g.phase()==Phase::Quarantined);
    esp_ble_gattc_cb_param_t e{};e.reg.app_id=42;post(g,ESP_GATTC_REG_EVT,e);tick(g);
    assert(calls.back()=="unregister");assert(g.post(ESP_GATTC_UNREG_EVT,7,nullptr));tick(g);assert(g.idle()); }
  { hw1pipe::Session session; Gatt::Event slabs[Gatt::kSlabCount]{};Gatt g{session,slabs};begin(g);g.stop(now);tick(g);assert(calls.back()=="cancel");
    // An OPEN that wins the cancellation race still needs a checked CLOSE.
    esp_ble_gattc_cb_param_t e{};e.open.conn_id=4;std::memcpy(e.open.remote_bda,address,6);
    post(g,ESP_GATTC_OPEN_EVT,e);tick(g);assert(calls.back()=="close");finish(g); }
  { hw1pipe::Session session; Gatt::Event slabs[Gatt::kSlabCount]{};Gatt g{session,slabs};ready(g);g.stop(now);tick(g);tick(g,4000);assert(g.phase()==Phase::Quarantined);
    assert(!g.begin(43,address,1,now,psk));finish(g); }
  { hw1pipe::Session session; Gatt::Event slabs[Gatt::kSlabCount]{};Gatt g{session,slabs};ready(g);esp_ble_gattc_cb_param_t e{};std::memcpy(e.srvc_chg.remote_bda,address,6);
    assert(g.post(ESP_GATTC_SRVC_CHG_EVT,ESP_GATT_IF_NONE,&e));tick(g);assert(!g.ready());finish(g); }
  { hw1pipe::Session session; Gatt::Event slabs[Gatt::kSlabCount]{};Gatt g{session,slabs};ready(g);Bytes invalid(12,0);for(unsigned i=0;i<4;++i)notify(g,invalid);
    tick(g);assert(!g.ready());finish(g); }
  // Thirteen notifications between polls fit beside the three control slots,
  // which stay free for completions; the fourteenth fails the pipe for
  // resources before any queued frame is parsed.
  { hw1pipe::Session session; Gatt::Event slabs[Gatt::kSlabCount]{};Gatt g{session,slabs};ready(g);Bytes invalid(12,0);for(unsigned i=0;i<13;++i)notify(g,invalid);
    esp_ble_gattc_cb_param_t e{};e.write.conn_id=4;e.write.handle=11;post(g,ESP_GATTC_WRITE_CHAR_EVT,e);
    e.congest.conn_id=4;post(g,ESP_GATTC_CONGEST_EVT,e);e.queue_full.conn_id=4;post(g,ESP_GATTC_QUEUE_FULL_EVT,e);
    tick(g);assert(!g.ready() && g.pipe.error==hw1pipe::Error::Unauthorized);finish(g); }
  { hw1pipe::Session session; Gatt::Event slabs[Gatt::kSlabCount]{};Gatt g{session,slabs};ready(g);Bytes invalid(12,0);for(unsigned i=0;i<14;++i)notify(g,invalid);
    tick(g);assert(!g.ready() && g.pipe.error==hw1pipe::Error::Resources);finish(g); }
  { hw1pipe::Session session; Gatt::Event slabs[Gatt::kSlabCount]{};Gatt g{session,slabs};ready(g);failApi="close";g.stop(now);tick(g);assert(g.phase()==Phase::Quarantined);
    failApi.clear();assert(!g.begin(43,address,1,now,psk));g.hostStopped();assert(g.idle()); }
  // A-5: the 5 s secure cap is absolute, but only peer silence is charged.
  // A reply the P4 could not submit, or more than 1 s of local delay, is not.
  for(unsigned blockedMs:{0u,500u,1200u,6000u}) {
    hw1pipe::Session session;Gatt::Event slabs[Gatt::kSlabCount]{};Gatt g{session,slabs};begin(g);opened(g);discover(g);subscribed(g);
    const uint32_t cccdAt=now-60;peer=PhoneSecurePeer{};auto hello=peer.hello();
    gate=false;notify(g,hello);
    for(unsigned t=0;t<blockedMs && g.phase()==Phase::Secure;t+=10)tick(g,10);
    gate=true;
    // Our HELLO_ACK completes as soon as it goes out; then the phone stays silent.
    for(size_t seen=writes.size(),guard=0;g.phase()==Phase::Secure && guard<1000;++guard) {
      tick(g,10);
      if(writes.size()>seen){seen=writes.size();esp_ble_gattc_cb_param_t e{};e.write.conn_id=4;e.write.handle=11;
        post(g,ESP_GATTC_WRITE_CHAR_EVT,e);}
    }
    assert(g.phase()==Phase::Retiring && !g.ready());
    assert(now-cccdAt>=5000 && now-cccdAt<5040);
    assert(g.authenticationFailed()==(blockedMs<1000));
    assert(g.pipe.error==(blockedMs<1000?hw1pipe::Error::Unauthorized:hw1pipe::Error::Deadline));
    finish(g);
  }
  // v2 version signal through the real adapter: a stream-0 RESET 2 before
  // HELLO_ACK, under a v1 or v2 header, ends with Version and a peer reset,
  // never an authentication charge, and the latch survives the adapter's stops.
  for(uint8_t version:{uint8_t(1),hw1pipe::kVersion}) {
    hw1pipe::Session session;Gatt::Event slabs[Gatt::kSlabCount]{};Gatt g{session,slabs};hello(g);const uint32_t helloAt=now-30;
    phoneFrame(g,hw1pipe::Op::Reset,0,{2,0},3,version);
    assert(!g.ready() && g.phase()==Phase::Retiring && !g.authenticationFailed());
    assert(g.pipe.error==hw1pipe::Error::Version && g.pipe.peerReset);
    assert(g.pipe.helloEnd()==hw1pipe::HelloEnd::VersionSignal && g.pipe.helloEndAfterMs()==now-helloAt);
    finish(g);assert(g.pipe.helloEnd()==hw1pipe::HelloEnd::VersionSignal);
    begin(g);assert(g.pipe.helloEnd()==hw1pipe::HelloEnd::VersionSignal);g.stop(now);tick(g);g.hostStopped();
  }
  // After READY the same v1 frame is just a protocol error, and no strike.
  {hw1pipe::Session session;Gatt::Event slabs[Gatt::kSlabCount]{};Gatt g{session,slabs};ready(g);phoneFrame(g,hw1pipe::Op::Reset,0,{2,0},3,1);
   assert(!g.ready() && g.pipe.error==hw1pipe::Error::Protocol && !g.pipe.peerReset);
   assert(g.pipe.helloEnd()==hw1pipe::HelloEnd::None);finish(g);}
  // A link loss before any answer counts as a silent HELLO only within 1.5 s of
  // our HELLO's acceptance; a begun answer never does.
  for(uint32_t wait:{0u,1468u,1469u}) {
    hw1pipe::Session session;Gatt::Event slabs[Gatt::kSlabCount]{};Gatt g{session,slabs};hello(g);const uint32_t helloAt=now-30;
    if(wait)tick(g,wait);
    esp_ble_gattc_cb_param_t e{};e.disconnect.conn_id=4;std::memcpy(e.disconnect.remote_bda,address,6);
    post(g,ESP_GATTC_DISCONNECT_EVT,e);tick(g);
    const uint32_t after=now-helloAt;
    assert(g.pipe.error==hw1pipe::Error::Connect && !g.authenticationFailed() && !g.ready());
    assert(g.pipe.helloEnd()==(after<1500?hw1pipe::HelloEnd::Silent:hw1pipe::HelloEnd::None));
    assert(after<1500 ? g.pipe.helloEndAfterMs()==after : !g.pipe.helloEndAfterMs());
    g.hostStopped();
  }
  {hw1pipe::Session session;Gatt::Event slabs[Gatt::kSlabCount]{};Gatt g{session,slabs};hello(g);
   phoneFrame(g,hw1pipe::Op::HelloAck,0,Bytes(8,0x51),1);  // FIRST fragment only: the phone answered
   esp_ble_gattc_cb_param_t e{};e.disconnect.conn_id=4;post(g,ESP_GATTC_DISCONNECT_EVT,e);tick(g);
   assert(g.pipe.error==hw1pipe::Error::Connect && g.pipe.helloEnd()==hw1pipe::HelloEnd::None);g.hostStopped();}
  // The core's 3 s HELLO deadline with nothing received is a silent HELLO.
  {hw1pipe::Session session;Gatt::Event slabs[Gatt::kSlabCount]{};Gatt g{session,slabs};hello(g);const uint32_t helloAt=now-30;
   while(g.phase()==Phase::Pipe)tick(g,10);
   assert(g.pipe.error==hw1pipe::Error::Deadline && !g.authenticationFailed());
   assert(g.pipe.helloEnd()==hw1pipe::HelloEnd::Silent && g.pipe.helloEndAfterMs()==now-helloAt);finish(g);}
  // HELLO_ACK must carry max_streams 3 and only defined phone flags.
  for(int bad=0;bad<2;++bad) {
    hw1pipe::Session session;Gatt::Event slabs[Gatt::kSlabCount]{};Gatt g{session,slabs};hello(g);
    Bytes ack(20);std::memset(ack.data(),0x51,8);std::memset(ack.data()+8,0x22,8);
    ack[16]=bad==0?2:hw1pipe::kMaxStreams;ack[17]=bad==1?0x04:0x03;hw1pipe::put16(ack.data()+18,200);
    phoneFrame(g,hw1pipe::Op::HelloAck,0,ack);
    assert(!g.ready() && g.pipe.error==hw1pipe::Error::Protocol && g.pipe.helloEnd()==hw1pipe::HelloEnd::None);finish(g);
  }
  // The adapter hands both pressure inputs to the core: display-busy holds the
  // console's DATA for as long as it lasts; relay pressure alone does not.
  {hw1pipe::Session session;Gatt::Event slabs[Gatt::kSlabCount]{};Gatt g{session,slabs};ready(g);
   const auto id=g.pipe.openConsole(now);assert(id);
   size_t n=writes.size();tick(g,30);assert(writes.size()==n+1);
   auto open=peer.open(writes.back());
   assert(open[1]==uint8_t(hw1pipe::Op::Open) && hw1pipe::get16(open.data()+2)==id && open.size()==20);
   assert(!std::memcmp(open.data()+12,"\x00\x10\x00\x00\x01\x00\x04\x00",8));writeDone(g);
   Bytes ok(16);ok[0]=hw1pipe::kVersion;ok[1]=uint8_t(hw1pipe::Op::OpenOk);hw1pipe::put16(ok.data()+2,id);
   hw1pipe::put32(ok.data()+4,g.pipe.rxSequence);hw1pipe::put16(ok.data()+8,4);ok[10]=3;hw1pipe::put32(ok.data()+12,4096);
   assert(g.pipe.receive(ok.data(),ok.size(),now));
   const uint8_t head[4]={2,0,2,0},text[2]={'O','K'};
   assert(g.pipe.pushRecord(id,head,4,text,2,4)==hw1pipe::PushResult::Ok);
   n=writes.size();
   for(unsigned i=0;i<5000;++i){now+=10;g.poll(now,true,true);assert(!held);}
   assert(writes.size()==n);  // 50 s of display updates (short of the link PING): nothing
   now+=30;g.poll(now,true,false);assert(writes.size()==n+1);
   auto data=peer.open(writes.back());
   assert(data[1]==uint8_t(hw1pipe::Op::Data) && data.size()==18 && !std::memcmp(data.data()+12,"\x02\x00\x02\x00OK",6));
   writeDone(g);finish(g);}
  std::printf("sizeof(Gatt)=%zu sizeof(Session)=%zu\n",sizeof(Gatt),sizeof(hw1pipe::Session));
  puts("Native phone GATT: A-5 expiry attribution, security barrier, MTUs, discovery, callback routing, pacing, deadlines, retirement and quarantine, v2 HELLO/version signal/silent HELLO and both pressure inputs passed");
}
