#include "System_BuildConfig.h"
#if ENABLE_BLUETOOTH && ENABLE_BLE_PHONE_PIPE
#include "BLE_PhoneGatt.h"
#include "BLE_CentralTx.h"
#include <esp_gap_ble_api.h>
#include <esp_random.h>
#include <cstring>

namespace hw1phone {
namespace {
esp_bt_uuid_t uuid(uint8_t characteristic) {
  // UUID bytes in ESP-IDF's little-endian representation.
  esp_bt_uuid_t result{}; result.len=ESP_UUID_LEN_128;
  const uint8_t bytes[16]={0xa0,0xc6,0x21,0x50,0x9f,0x3d,0xc4,0xa8,
                          0x2e,0x4b,0x7d,0x6f,characteristic,0,0xe1,0xb7};
  std::memcpy(result.uuid.uuid128,bytes,16); return result;
}
}
bool Gatt::begin(uint16_t appId, const uint8_t address[6], uint8_t type, uint32_t now,const uint8_t psk[32]) {
  if (!idle() || !address || !psk || type>3 || appId>0x7fff) return false;
  if (!queue_) queue_=xQueueCreateStatic(kQueueSize,sizeof(uint8_t),indexBytes_,&queueControl_);
  if (!queue_) return false;
  xQueueReset(queue_); slabsReset(); overflow_=false;
  std::memcpy(address_,address,6); type_=type; appId_=appId;
  conn_=0xffff; mtu_=23; serviceStart_=serviceEnd_=rx_=tx_=cccd_=0; services_=0;
  opened_=openPending_=cancelPending_=closePending_=unregisterPending_=writePending_=congested_=false;
  autoDiscoveryDone_=false;
  pipe.down();channel_.reset();std::memcpy(psk_,psk,32);replyLength_=0;
  descriptorPending_=authenticationFailed_=rejectPending_=serviceChanged_=false;
  if (++generation_==0) ++generation_;
  std::memset(entered_,0,sizeof(entered_));
  end_={}; armed_=concluded_=discoveryFailed_=mtuRefused_=false;
  enter(Phase::Registering,now); registering_=true;
  if (esp_ble_gattc_app_register(appId)!=ESP_OK) {
    registering_=false;channel_.reset();sodium_memzero(psk_,sizeof(psk_));enter(Phase::Idle,now);return false;
  }
  armed_=true;
  return true;
}

bool Gatt::post(esp_gattc_cb_event_t kind, esp_gatt_if_t interface,
                const esp_ble_gattc_cb_param_t* p) {
  if (kind==ESP_GATTC_REG_EVT) {
    if (!p || !registering_.load() || p->reg.app_id!=appId_.load()) return false;
    // Publish routing before the callback returns: later callbacks can arrive
    // before the worker has consumed REG. The object/queue has static lifetime.
    if (p->reg.status==ESP_GATT_OK) interface_.store(interface);
  } else {
    const auto owned=interface_.load();
    if (owned==ESP_GATT_IF_NONE || (interface!=ESP_GATT_IF_NONE && interface!=owned)) return false;
    if (interface==ESP_GATT_IF_NONE) {
      // Some host events are broadcast. Consume only the phone's address.
      if (!p) return false;
      const uint8_t* peer=nullptr;
      if (kind==ESP_GATTC_CONNECT_EVT) peer=p->connect.remote_bda;
      else if (kind==ESP_GATTC_SRVC_CHG_EVT) peer=p->srvc_chg.remote_bda;
      else if (kind==ESP_GATTC_DISCONNECT_EVT) peer=p->disconnect.remote_bda;
      if (!peer || std::memcmp(peer,address_,6)) return false;
      interface=owned;
    }
  }
  // UNREG has no parameter in this SDK. Never dereference it.
  if (kind!=ESP_GATTC_UNREG_EVT && !p) { overflow_=true; return true; }
  const int slot=slabReserve();
  if (slot<0) { overflow_=true; return true; }
  // Every header field is written: the slab is reused and its storage is
  // NOLOAD PSRAM (no initializer ever ran).
  Event& event=slabs_[slot];
  event.kind=kind; event.interface=interface; event.status=ESP_GATT_OK;
  event.conn=0xffff; event.handle=0; event.last=0; event.length=0; event.flag=false;
  bool keep=true, result=true;
  switch (kind) {
    case ESP_GATTC_REG_EVT: event.status=p->reg.status; break;
    case ESP_GATTC_OPEN_EVT:
      event.status=p->open.status; event.conn=p->open.conn_id;
      event.length=p->open.mtu; std::memcpy(event.data,p->open.remote_bda,6); break;
    case ESP_GATTC_CONNECT_EVT: keep=false; break; // OPEN owns MTU/discovery; no SMP request
    case ESP_GATTC_CFG_MTU_EVT:
      event.status=p->cfg_mtu.status; event.conn=p->cfg_mtu.conn_id; event.length=p->cfg_mtu.mtu; break;
    case ESP_GATTC_DIS_SRVC_CMPL_EVT:
      event.status=p->dis_srvc_cmpl.status; event.conn=p->dis_srvc_cmpl.conn_id; break;
    case ESP_GATTC_SEARCH_RES_EVT:
      event.conn=p->search_res.conn_id; event.handle=p->search_res.start_handle;
      event.last=p->search_res.end_handle;
      { const auto expected=uuid(0);
        event.flag=p->search_res.is_primary && p->search_res.srvc_id.uuid.len==ESP_UUID_LEN_128 &&
          !std::memcmp(p->search_res.srvc_id.uuid.uuid.uuid128,expected.uuid.uuid128,16); }
      break;
    case ESP_GATTC_SEARCH_CMPL_EVT:
      event.status=p->search_cmpl.status; event.conn=p->search_cmpl.conn_id; break;
    case ESP_GATTC_REG_FOR_NOTIFY_EVT:
      event.status=p->reg_for_notify.status; event.handle=p->reg_for_notify.handle; break;
    case ESP_GATTC_WRITE_DESCR_EVT: case ESP_GATTC_WRITE_CHAR_EVT:
      event.status=p->write.status; event.conn=p->write.conn_id; event.handle=p->write.handle; break;
    case ESP_GATTC_NOTIFY_EVT:
      event.conn=p->notify.conn_id; event.handle=p->notify.handle;
      event.length=p->notify.value_len; event.flag=p->notify.is_notify;
      if (event.length>sizeof(event.data) || !p->notify.value ||
          uxQueueMessagesWaiting(queue_)>=kQueueSize-kControlSlots) { overflow_=true; keep=false; break; }
      std::memcpy(event.data,p->notify.value,event.length); break;
    case ESP_GATTC_CLOSE_EVT:
      event.status=p->close.status; event.conn=p->close.conn_id; break;
    case ESP_GATTC_DISCONNECT_EVT:
      // Every registered app gets a copy of every link's DISCONNECT: keep the
      // address so consume() takes only the phone's own reason.
      event.conn=p->disconnect.conn_id; event.handle=uint16_t(p->disconnect.reason);
      std::memcpy(event.data,p->disconnect.remote_bda,6); break;
    case ESP_GATTC_CANCEL_OPEN_EVT:
      event.status=p->cancel_open.status; std::memcpy(event.data,p->cancel_open.remote_bda,6); break;
    case ESP_GATTC_SRVC_CHG_EVT:
      if (std::memcmp(p->srvc_chg.remote_bda,address_,6)) { keep=false; result=false; }
      break;
    case ESP_GATTC_CONGEST_EVT:
      event.conn=p->congest.conn_id; event.flag=p->congest.congested; break;
    case ESP_GATTC_QUEUE_FULL_EVT:
      event.conn=p->queue_full.conn_id; event.flag=p->queue_full.is_full; break;
    case ESP_GATTC_UNREG_EVT: break;
    default: keep=false; break;
  }
  if (!keep) { slabRelease(unsigned(slot)); return result; }
  const uint8_t index=uint8_t(slot);
  slabState_[slot].store(kSlabQueued,std::memory_order_release);
  if (!queue_ || xQueueSend(queue_,&index,0)!=pdTRUE) { slabRelease(unsigned(slot)); overflow_=true; }
  return true;
}

int Gatt::slabReserve() {
  for (unsigned i=0; i<kSlabCount; ++i) {
    uint8_t expected=kSlabFree;
    if (slabState_[i].compare_exchange_strong(expected,kSlabWriting,std::memory_order_acq_rel)) return int(i);
  }
  return -1;
}

hw1coex::ConnectStage Gatt::stageNow() const {
  using S=hw1coex::ConnectStage;
  const bool discovered=autoDiscoveryDone_ && !discoveryFailed_;
  switch (phase_) {
    case Phase::Registering: return S::RegisterEvent;
    case Phase::Opening: return S::Open;
    case Phase::Mtu: return mtuRefused_ || discovered ? S::Mtu : S::Discovery;
    case Phase::Discovering: return discovered ? S::Search : S::Discovery;
    case Phase::Subscribe: case Phase::Cccd: return S::Subscribe;
    // Secure begins when the CCCD write is submitted; until it is answered
    // the phone has not seen a subscription.
    case Phase::Secure: return descriptorPending_ ? S::Cccd : S::Secure;
    case Phase::Pipe: return S::Hello;
    default: return S::None;
  }
}
void Gatt::noteStatus(esp_gatt_status_t status) {
  if (armed_ && status!=ESP_GATT_OK && end_.gatt<0) end_.gatt=int16_t(status);
}
void Gatt::conclude(uint32_t now, hw1pipe::Error why) {
  if (!armed_) return;  // not registered yet, or already concluded
  armed_=false; concluded_=true;
  end_.at=now; end_.stage=stageNow(); end_.error=why; end_.serviceChanged=serviceChanged_;
}
void Gatt::stop(uint32_t now, hw1pipe::Error reason) {
  conclude(now,reason);  // the first stop names the attempt's end
  // Transport failure/cancellation is not evidence of a bad key. Only the
  // channel parser or the peer handshake deadline may impose auth cooldown.
  pipe.down(reason,now);channel_.reset();sodium_memzero(psk_,sizeof(psk_));
  sodium_memzero(reply_,sizeof(reply_));replyLength_=0;
  if (idle() || phase_==Phase::Retiring || phase_==Phase::Quarantined) return;
  enter(Phase::Retiring,now);
}
void Gatt::retire(uint32_t now) {
  const auto interface=interface_.load();
  if (registering_.load()) return; // late REG still owns its registration result
  if (interface==ESP_GATT_IF_NONE) { enter(Phase::Idle,now); return; }
  if (openPending_) {
    if (!cancelPending_) {
      esp_ble_gattc_cancel_open_params_t cancel{}; cancel.gattc_if=interface;
      std::memcpy(cancel.remote_bda,address_,6); cancelPending_=true;
      if (esp_ble_gattc_cancel_open(&cancel)!=ESP_OK) enter(Phase::Quarantined,now);
    }
    return;
  }
  if (opened_) {
    if (!closePending_) {
      closePending_=true;
      if (esp_ble_gattc_close(interface,conn_)!=ESP_OK) enter(Phase::Quarantined,now);
    }
    return;
  }
  if (!unregisterPending_) {
    unregisterPending_=true;
    if (esp_ble_gattc_app_unregister(interface)!=ESP_OK) enter(Phase::Quarantined,now);
  }
}

bool Gatt::discover(uint32_t now) {
  // Cache refresh is requested before OPEN. SEARCH must finish within one
  // discovery budget; no Arduino 30-second blocking getServices() call.
  auto service=uuid(0); discoveryAt_=now; enter(Phase::Discovering,now);
  return esp_ble_gattc_search_service(interface_.load(),conn_,&service)==ESP_OK;
}
bool Gatt::subscribe(uint32_t now) {
  const auto interface=interface_.load();
  esp_gattc_char_elem_t chars[2]{}; uint16_t count=2;
  if (esp_ble_gattc_get_char_by_uuid(interface,conn_,serviceStart_,serviceEnd_,uuid(1),chars,&count)!=ESP_GATT_OK ||
      count!=1 || !(chars[0].properties & ESP_GATT_CHAR_PROP_BIT_WRITE_NR)) return false;
  rx_=chars[0].char_handle; count=2;
  if (esp_ble_gattc_get_char_by_uuid(interface,conn_,serviceStart_,serviceEnd_,uuid(2),chars,&count)!=ESP_GATT_OK ||
      count!=1 || !(chars[0].properties & ESP_GATT_CHAR_PROP_BIT_NOTIFY)) return false;
  tx_=chars[0].char_handle;
  if (rx_==tx_ || rx_<=serviceStart_ || tx_<=serviceStart_ || rx_>serviceEnd_ || tx_>serviceEnd_) return false;
  esp_bt_uuid_t ccc{}; ccc.len=ESP_UUID_LEN_16; ccc.uuid.uuid16=ESP_GATT_UUID_CHAR_CLIENT_CONFIG;
  esp_gattc_descr_elem_t desc[2]{}; count=2;
  if (esp_ble_gattc_get_descr_by_char_handle(interface,conn_,tx_,ccc,desc,&count)!=ESP_GATT_OK ||
      count!=1 || desc[0].handle<=tx_ || desc[0].handle>serviceEnd_) return false;
  cccd_=desc[0].handle; enter(Phase::Subscribe,now);
  return esp_ble_gattc_register_for_notify(interface,address_,tx_)==ESP_OK;
}

void Gatt::consume(const Event& e, uint32_t now) {
  const bool retiring=phase_==Phase::Retiring || phase_==Phase::Quarantined;
  if (e.kind==ESP_GATTC_REG_EVT) {
    registering_=false;
    if (e.status!=ESP_GATT_OK) {
      noteStatus(e.status); conclude(now,hw1pipe::Error::Connect);
      interface_=ESP_GATT_IF_NONE; enter(Phase::Idle,now); return;
    }
    if (retiring) return;
    if (phase_!=Phase::Registering) { stop(now,hw1pipe::Error::Protocol); return; }
    // Always refresh handles; a previous Android service incarnation can have
    // a different attribute table under the same service UUID.
    if (esp_ble_gattc_cache_refresh(address_)!=ESP_OK) { stop(now,hw1pipe::Error::Connect); return; }
    enter(Phase::Opening,now); openPending_=true;
    if (esp_ble_gattc_open(e.interface,address_,esp_ble_addr_type_t(type_),true)!=ESP_OK) {
      openPending_=false; stop(now,hw1pipe::Error::Connect);  // stage open, no reason: a local refusal
    }
    return;
  }
  if (e.kind==ESP_GATTC_UNREG_EVT) {
    if (!unregisterPending_) conclude(now,hw1pipe::Error::Protocol);
    pipe.down(unregisterPending_ ? pipe.error : hw1pipe::Error::Protocol);
    channel_.reset();sodium_memzero(psk_,sizeof(psk_));
    interface_=ESP_GATT_IF_NONE; opened_=openPending_=false;
    enter(Phase::Idle,now); return;
  }
  if (e.kind==ESP_GATTC_OPEN_EVT) {
    if (!openPending_ && !retiring) { stop(now,hw1pipe::Error::Protocol); return; }
    noteStatus(e.status);
    openPending_=false;
    if (e.status==ESP_GATT_OK) {
      conn_=e.conn; mtu_=e.length>=23?e.length:23; opened_=true;
      if (std::memcmp(e.data,address_,6)) { stop(now,hw1pipe::Error::Protocol); return; }
    }
    if (retiring) return;
    if (e.status!=ESP_GATT_OK) { stop(now,hw1pipe::Error::Connect); return; }
    enter(Phase::Mtu,now);
    if(esp_ble_gattc_send_mtu_req(interface_.load(),conn_)!=ESP_OK){mtuRefused_=true;stop(now,hw1pipe::Error::Connect);}
    return;
  }
  if (e.kind==ESP_GATTC_CANCEL_OPEN_EVT) {
    if (std::memcmp(e.data,address_,6)) return;
    if (e.status==ESP_GATT_OK) openPending_=false;
    else if (openPending_) enter(Phase::Quarantined,now);
    return;
  }
  if (e.kind==ESP_GATTC_DISCONNECT_EVT || e.kind==ESP_GATTC_CLOSE_EVT) {
    // A failed open's DISCONNECT carries the HCI reason and no usable conn_id;
    // a live link's carries ours. Other links' copies (G2 temples, ring) carry
    // their own addresses. The first reason of the attempt is kept.
    if (e.kind==ESP_GATTC_DISCONNECT_EVT && armed_ && !end_.reason && !std::memcmp(e.data,address_,6) &&
        ((openPending_ && !opened_) || e.conn==conn_)) end_.reason=e.handle;
    if (e.conn!=conn_) return;
    if (e.kind==ESP_GATTC_CLOSE_EVT && e.status!=ESP_GATT_OK) {
      conclude(now,hw1pipe::Error::Connect); enter(Phase::Quarantined,now); return;
    }
    opened_=false; openPending_=false;
    if (!retiring) stop(now,hw1pipe::Error::Connect);
    return;
  }
  if (retiring || idle()) return;
  if (e.conn!=0xffff && e.conn!=conn_) return;
  switch (e.kind) {
    case ESP_GATTC_CFG_MTU_EVT:
      if (phase_!=Phase::Mtu) break;
      // A completed unsupported MTU negotiation keeps the existing MTU. On a
      // live link IDF 5.5.5 never reports BUSY here: a CFG_MTU issued during
      // discovery is queued, and a synchronous failure is dropped unreported.
      noteStatus(e.status);
      // Our CFG_MTU is queued behind Bluedroid's discovery, so running before
      // DIS_SRVC_CMPL means that discovery ended without one: it failed, and
      // the search below reads a partial cache (for the record only).
      if (!autoDiscoveryDone_) discoveryFailed_=true;
      if (e.status==ESP_GATT_OK && e.length>=23) mtu_=e.length;
      if(mtu_<kMinimumMtu || !discover(now))stop(now,hw1pipe::Error::Connect);
      break;
    case ESP_GATTC_DIS_SRVC_CMPL_EVT:
      // Bluedroid's automatic discovery ended; a CFG_MTU queued behind it runs now.
      if (phase_==Phase::Mtu && !autoDiscoveryDone_) {
        autoDiscoveryDone_=true; autoDiscoveryAt_=now;
        if (e.status!=ESP_GATT_OK) { noteStatus(e.status); discoveryFailed_=true; }
      }
      break;
    case ESP_GATTC_SEARCH_RES_EVT:
      if (phase_!=Phase::Discovering || !e.flag || !e.handle || e.last<=e.handle) {
        stop(now,hw1pipe::Error::Protocol); break;
      }
      ++services_; serviceStart_=e.handle; serviceEnd_=e.last; break;
    case ESP_GATTC_SEARCH_CMPL_EVT:
      noteStatus(e.status);
      if (phase_!=Phase::Discovering || e.status!=ESP_GATT_OK || services_!=1 || !subscribe(now))
        stop(now,hw1pipe::Error::Protocol);
      break;
    case ESP_GATTC_REG_FOR_NOTIFY_EVT:
      noteStatus(e.status);
      if (phase_!=Phase::Subscribe || e.handle!=tx_ || e.status!=ESP_GATT_OK) {
        stop(now,hw1pipe::Error::Protocol); break;
      }
      enter(Phase::Cccd,now); break; // submission is paced/arbitrated in poll()
    case ESP_GATTC_WRITE_DESCR_EVT:
      noteStatus(e.status);
      if (phase_!=Phase::Secure || !writePending_ || !descriptorPending_ || e.handle!=cccd_ || e.status!=ESP_GATT_OK) {
        stop(now,hw1pipe::Error::Protocol);break;
      }
      descriptorPending_=writePending_=false;break;
    case ESP_GATTC_WRITE_CHAR_EVT:
      if ((phase_!=Phase::Secure && phase_!=Phase::Pipe) || !writePending_ ||
          descriptorPending_ || e.handle!=rx_ ||
          (e.status!=ESP_GATT_OK && e.status!=ESP_GATT_CONGESTED)) {
        stop(now,hw1pipe::Error::Protocol);break;
      }
      // 0x8f means L2CAP accepted this NO_RSP PDU before becoming congested.
      // Its bytes/counter were already committed; never replay them. The
      // separate CONGEST event owns pacing until the channel clears.
      writePending_=false;break;
    case ESP_GATTC_NOTIFY_EVT: {
      if(rejectPending_)break;
      if((phase_!=Phase::Secure && phase_!=Phase::Pipe) || e.handle!=tx_ || !e.flag || e.length>hw1pipe::valueLimit(mtu_)) {
        stop(now,hw1pipe::Error::Protocol);break;
      }
      uint8_t plain[hw1pipe::kValue];size_t length=0;
      const auto result=channel_.receive(e.data,e.length,psk_,plain,sizeof(plain),length);
      if(result==SecureChannel::Result::Reply && phase_==Phase::Secure && !replyLength_ && length<=sizeof(reply_)) {
        std::memcpy(reply_,plain,length);replyLength_=length;
      } else if(result!=SecureChannel::Result::Data || phase_!=Phase::Pipe || !pipe.receive(plain,length,now)) {
        authenticationFailed_=result==SecureChannel::Result::Error || phase_==Phase::Secure;
        if(phase_==Phase::Secure && result==SecureChannel::Result::Error) {
          // Best-effort rejection is paced like every other write. Keys are
          // already erased. A congested/pending local write gets at most 250 ms.
          sodium_memzero(psk_,sizeof(psk_));reply_[0]=5;reply_[1]=2;replyLength_=2;
          rejectPending_=true;rejectAt_=now;
        } else if(result==SecureChannel::Result::Data && phase_==Phase::Pipe) {
          // An authenticated frame the pipe refused: keep the core's own cause
          // (a phone RESET's code, Flow, Protocol). Unauthorized is reserved
          // for frames that fail authentication.
          stop(now,pipe.error!=hw1pipe::Error::None?pipe.error:hw1pipe::Error::Protocol);
        } else stop(now,hw1pipe::Error::Unauthorized);
      }
      sodium_memzero(plain,sizeof(plain));break;
    }
    case ESP_GATTC_CONGEST_EVT: congested_=e.flag; break;
    case ESP_GATTC_QUEUE_FULL_EVT:
      if (e.flag) stop(now,hw1pipe::Error::Resources);
      break;
    case ESP_GATTC_SRVC_CHG_EVT: serviceChanged_=true; stop(now,hw1pipe::Error::Protocol); break;
    default: break;
  }
}

void Gatt::poll(uint32_t now, bool relayPressure, bool consoleDisplayBusy) {
  if (!queue_) return;
  if (overflow_.exchange(false)) stop(now,hw1pipe::Error::Resources);
  uint8_t index=0;
  for (unsigned i=0; i<kQueueSize && xQueueReceive(queue_,&index,0)==pdTRUE; ++i) {
    if (index>=kSlabCount) continue;
    slabState_[index].store(kSlabProcessing,std::memory_order_relaxed);
    consume(slabs_[index],now);
    slabRelease(index);
  }
  if (idle()) return;
  if (phase_==Phase::Retiring || phase_==Phase::Quarantined) {
    retire(now);
    if (phase_==Phase::Retiring && hw1pipe::expired(now,phaseAt_,3000)) enter(Phase::Quarantined,now);
    return;
  }
  const bool discovery=phase_==Phase::Discovering || phase_==Phase::Subscribe || phase_==Phase::Cccd;
  if ((phase_==Phase::Registering && hw1pipe::expired(now,phaseAt_,2000)) ||
      (phase_==Phase::Opening && hw1pipe::expired(now,phaseAt_,10000)) ||
      (phase_==Phase::Mtu && (autoDiscoveryDone_ ? hw1pipe::expired(now,autoDiscoveryAt_,2000)
                                                 : hw1pipe::expired(now,phaseAt_,10000))) ||
      (discovery && hw1pipe::expired(now,discoveryAt_,3000)) ||
      (writePending_ && hw1pipe::expired(now,writeAt_,2000))) {
    stop(now,hw1pipe::Error::Deadline); return;
  }
  if(rejectPending_ && hw1pipe::expired(now,rejectAt_,250)) {stop(now,hw1pipe::Error::Unauthorized);return;}
  if(phase_==Phase::Secure) {
    const bool owed=writePending_ || replyLength_;
    if(owed && !localOwed_)localSince_=now;
    if(!owed && localOwed_)localMs_+=uint32_t(now-localSince_);
    localOwed_=owed;
  }
  if(phase_==Phase::Secure && hw1pipe::expired(now,secureAt_,5000)) {
    // A-5: the 5 s cap is absolute, but it is charged as a failed
    // authentication only when the phone owed the next step and nothing local
    // (queued reply, unfinished write, more than 1 s of local delay) held it up.
    const uint32_t local=localMs_+(localOwed_?uint32_t(now-localSince_):0);
    authenticationFailed_=!localOwed_ && local<1000;
    stop(now,authenticationFailed_?hw1pipe::Error::Unauthorized:hw1pipe::Error::Deadline);return;
  }
  if (phase_==Phase::Pipe && !pipe.tick(now,relayPressure,consoleDisplayBusy)) { stop(now,pipe.error); return; }
  if ((phase_!=Phase::Pipe && phase_!=Phase::Cccd && phase_!=Phase::Secure) || writePending_ || congested_ ||
      !hw1pipe::expired(now,lastSend_,30) || !bleCentralTxTryPhone(true)) return;
  uint8_t bytes[hw1pipe::kValue],plain[hw1pipe::kValue];size_t length=0;esp_err_t status=ESP_OK;
  const Phase submitting=phase_;
  if(submitting==Phase::Cccd) {
    uint8_t enabled[2]={1,0};length=2;
    status=esp_ble_gattc_write_char_descr(interface_.load(),conn_,cccd_,2,enabled,
                                        ESP_GATT_WRITE_TYPE_RSP,ESP_GATT_AUTH_REQ_NONE);
  } else {
    if(submitting==Phase::Secure) {
      length=replyLength_;if(length)std::memcpy(bytes,reply_,length);
    } else {
      const size_t count=pipe.peek(plain);
      if(count)length=channel_.encrypt(plain,count,bytes,hw1pipe::valueLimit(mtu_));
      if(count && !length)status=ESP_FAIL;
    }
    if(length)status=esp_ble_gattc_write_char(interface_.load(),conn_,rx_,uint16_t(length),bytes,
                                            ESP_GATT_WRITE_TYPE_NO_RSP,ESP_GATT_AUTH_REQ_NONE);
  }
  // Local completion is bounded; NO_RSP never waits for a peer ATT response.
  bleCentralTxGive();sodium_memzero(plain,sizeof(plain));sodium_memzero(bytes,sizeof(bytes));
  if(status!=ESP_OK){stop(now,hw1pipe::Error::Connect);return;}
  if(!length)return;
  if(submitting==Phase::Cccd) {
    descriptorPending_=true;secureAt_=now;localMs_=0;localOwed_=false;enter(Phase::Secure,now);
  } else if(submitting==Phase::Secure) {
    if(rejectPending_){stop(now,hw1pipe::Error::Unauthorized);return;}
    replyLength_=0;sodium_memzero(reply_,sizeof(reply_));
    if(!channel_.replySubmitted()){stop(now,hw1pipe::Error::Unauthorized);return;}
    if(channel_.ready()) {
      sodium_memzero(psk_,sizeof(psk_));uint8_t nonce[8];
      do {esp_fill_random(nonce,sizeof(nonce));}while(!hw1pipe::get32(nonce)&&!hw1pipe::get32(nonce+4));
      if(!pipe.start(mtu_,nonce,now,kSecureOverhead)){stop(now,hw1pipe::Error::Protocol);return;}
      enter(Phase::Pipe,now);
    }
  } else if(!pipe.accept(now)){stop(now,hw1pipe::Error::Protocol);return;}
  writePending_=true;writeAt_=lastSend_=now;
}

void Gatt::hostStopped() {
  // No callback can remain after the checked HAL shutdown. Clearing quarantine
  // without that proof could route an old OPEN/UNREG into a new incarnation.
  interface_=ESP_GATT_IF_NONE; registering_=false; overflow_=false;
  if (queue_) xQueueReset(queue_);
  slabsReset();
  pipe.down();channel_.reset();sodium_memzero(psk_,sizeof(psk_));
  sodium_memzero(reply_,sizeof(reply_));replyLength_=0;phase_=Phase::Idle;
  armed_=false;  // a host reset mid-attempt leaves no record
}
}
#endif
