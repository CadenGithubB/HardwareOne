#include "System_LLMHttpTransport.h"
#if ENABLE_LLM_BACKEND && ENABLE_LLM_SOURCE_HTTP
#include "System_LLMHttpTransportCore.h"
#include "System_MemUtil.h"
#include "BLE_PhonePipe.h"
#include <WiFi.h>
#include <esp_transport.h>
#include <esp_tls.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_event.h>
#include <esp_wifi.h>
#include <esp_netif.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <lwip/dns.h>
#include <lwip/tcpip.h>
#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/platform_util.h>
#include <mbedtls/net_sockets.h>
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include <esp_crt_bundle.h>
#endif
#if CONFIG_MBEDTLS_SSL_PROTO_TLS1_3
#include <psa/crypto.h>
#endif
#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstring>
#include <new>
#include <sys/socket.h>
#include <sys/select.h>

#if ENABLE_BLE_PHONE_PIPE && !CONFIG_ESP_HTTP_CLIENT_ENABLE_CUSTOM_TRANSPORT
#error "Phone HTTP routing requires CONFIG_ESP_HTTP_CLIENT_ENABLE_CUSTOM_TRANSPORT=y in the saved SDK choices"
#endif

using hw1httptransport::Attempt;
using hw1httptransport::Path;
using hw1httptransport::Failure;
namespace {
#if ENABLE_BLE_PHONE_PIPE
constexpr uint32_t kDirectMs=2000,kHandshakeMs=4500,kIoMs=2000;
struct PhoneTls {
  mbedtls_ssl_context ssl;
  mbedtls_ssl_config config;
  mbedtls_x509_crt ca;
  mbedtls_ctr_drbg_context rng;
  mbedtls_entropy_context entropy;
  PhoneTls() {
    mbedtls_ssl_init(&ssl);mbedtls_ssl_config_init(&config);mbedtls_x509_crt_init(&ca);
    mbedtls_ctr_drbg_init(&rng);mbedtls_entropy_init(&entropy);
  }
  ~PhoneTls() {
    mbedtls_ssl_free(&ssl);mbedtls_ssl_config_free(&config);mbedtls_x509_crt_free(&ca);
    mbedtls_ctr_drbg_free(&rng);mbedtls_entropy_free(&entropy);
  }
};
#endif
}
struct LlmHttpIo {
  const char* ca=nullptr;
  const std::atomic<bool>* cancelled=nullptr;
  std::atomic<bool> interrupted{false};
  uint32_t started=0,total=0,revision=0;
  bool secure=true;
  int8_t outcome=-1; // F3: -1 not reported, 0 failed, 1 completed (llmHttpIoFinish)
  LlmHttpRequestKind kind=LlmHttpRequestKind::Chat;
  int socket=-1; // published after connect, protected by the caller request lock
#if ENABLE_BLE_PHONE_PIPE
  Attempt attempt;
  esp_transport_handle_t transport=nullptr;
  esp_tls_t* direct=nullptr;
  PhoneTls* tls=nullptr;
  PhonePipeStream pipe;
  Failure failure=Failure::Resources;
  const char* directStage="resolve";
  int directSocketError=0;
  uint32_t directStarted=0,directBudget=0;
  uint32_t epoch=0,phoneGeneration=0;
  // F3: what each path did for this request.
  struct PathOutcome {bool tried=false,connected=false;Failure failure=Failure::Resources;};
  PathOutcome wifiOutcome,phoneOutcome;
  PhonePipeOpenResult phoneOpen{};
  uint64_t phoneCorr=0;uint16_t phoneStream=0; // kept for F3 after close() drops the handle
  bool begun=false,connected=false,networkError=false;
  char host[254]{};
#endif
};
namespace {
struct RequestSlot {
  std::atomic<bool> claimed{false};
#if ENABLE_BLE_PHONE_PIPE
  PhonePipeStream retiring;
#endif
};
RequestSlot requestSlots[2];
bool cancelled(const LlmHttpIo& io) {
  return io.interrupted.load() || io.cancelled->load() || uint32_t(millis()-io.started)>=io.total;
}
#if ENABLE_BLE_PHONE_PIPE
bool expired(uint32_t start,uint32_t duration){return uint32_t(millis()-start)>=duration;}
uint32_t budget(const LlmHttpIo& io,uint32_t cap) {
  const uint32_t elapsed=uint32_t(millis()-io.started);
  return elapsed>=io.total?0:std::min(cap,io.total-elapsed);
}
void pause(){vTaskDelay(pdMS_TO_TICKS(10));}
LlmHttpIo& context(esp_transport_handle_t t){return *static_cast<LlmHttpIo*>(esp_transport_get_context_data(t));}

// DNS slots outlive abandoned requests. Only the TCP/IP completion returns an
// abandoned slot to Free; a timed-out request never frees/reuses callback data.
enum class DnsState:uint8_t {Free,Pending,Done,Abandoned};
// The names live in PSRAM: a slot is claimed (Free -> Pending) under dnsMux,
// its name is written after the unlock by the claimant alone, and nothing reads
// it until dnsStart runs on the TCP/IP thread.
struct DnsSlot {ip_addr_t address{};DnsState state=DnsState::Free;bool ok=false;};
DnsSlot dnsSlots[4];
EXT_RAM_BSS_ATTR char dnsNames[4][254];
portMUX_TYPE dnsMux=portMUX_INITIALIZER_UNLOCKED;
char* dnsName(const DnsSlot& slot){return dnsNames[&slot-dnsSlots];}
void dnsDone(const char*,const ip_addr_t* address,void* opaque) {
  auto& slot=*static_cast<DnsSlot*>(opaque);
  portENTER_CRITICAL(&dnsMux);
  if(slot.state==DnsState::Abandoned)slot.state=DnsState::Free;
  else if(slot.state==DnsState::Pending) {
    slot.ok=address!=nullptr;if(address)slot.address=*address;slot.state=DnsState::Done;
  }
  portEXIT_CRITICAL(&dnsMux);
}
void dnsStart(void* opaque) {
  auto& slot=*static_cast<DnsSlot*>(opaque);ip_addr_t address;
#if LWIP_IPV6
  constexpr uint8_t type=LWIP_DNS_ADDRTYPE_IPV4_IPV6;
#else
  constexpr uint8_t type=LWIP_DNS_ADDRTYPE_IPV4;
#endif
  const err_t result=dns_gethostbyname_addrtype(dnsName(slot),&address,dnsDone,&slot,type);
  if(result==ERR_OK)dnsDone(nullptr,&address,&slot);
  else if(result!=ERR_INPROGRESS)dnsDone(nullptr,nullptr,&slot);
}
bool resolve(LlmHttpIo& io,const char* host,char* out,size_t cap,uint32_t start,uint32_t duration) {
  ip_addr_t address;
  if(ipaddr_aton(host,&address))return ipaddr_ntoa_r(&address,out,cap)!=nullptr;
  DnsSlot* slot=nullptr;
  portENTER_CRITICAL(&dnsMux);
  for(auto& candidate:dnsSlots)if(candidate.state==DnsState::Free) {
    slot=&candidate;slot->state=DnsState::Pending;break;
  }
  portEXIT_CRITICAL(&dnsMux);
  if(!slot){io.failure=Failure::Resources;return false;}
  strlcpy(dnsName(*slot),host,sizeof(dnsNames[0]));  // outside the spinlock: PSRAM
  if(tcpip_try_callback(dnsStart,slot)!=ERR_OK) {
    portENTER_CRITICAL(&dnsMux);slot->state=DnsState::Free;portEXIT_CRITICAL(&dnsMux);
    io.failure=Failure::Resources;return false;
  }
  for(;;) {
    bool done=false,ok=false;
    const bool stop=cancelled(io) || expired(start,duration);
    portENTER_CRITICAL(&dnsMux);
    if(slot->state==DnsState::Done) {
      done=true;ok=slot->ok;if(ok)address=slot->address;slot->state=DnsState::Free;
    } else if(stop)slot->state=DnsState::Abandoned;
    portEXIT_CRITICAL(&dnsMux);
    if(stop){io.failure=cancelled(io)?Failure::Cancelled:Failure::Network;return false;}
    if(done){io.failure=Failure::Network;return ok && ipaddr_ntoa_r(&address,out,cap);}
    pause();
  }
}

hw1httptransport::Health health,phoneHealth;
uint32_t healthEpoch=0,healthRevision=0,phoneHealthGeneration=0,phoneHealthRevision=0;
std::atomic<uint32_t> wifiEpoch{1};
portMUX_TYPE healthMux=portMUX_INITIALIZER_UNLOCKED,initMux=portMUX_INITIALIZER_UNLOCKED;
StaticSemaphore_t initStorage;
SemaphoreHandle_t initMutex=nullptr;
bool eventsReady=false;
esp_event_handler_instance_t wifiEvents=nullptr,ipEvents=nullptr;
void networkEvent(void*,esp_event_base_t base,int32_t id,void*) {
  if((base==WIFI_EVENT && (id==WIFI_EVENT_STA_CONNECTED || id==WIFI_EVENT_STA_DISCONNECTED)) ||
     (base==IP_EVENT && (id==IP_EVENT_STA_GOT_IP || id==IP_EVENT_STA_LOST_IP)))++wifiEpoch;
}
bool watchNetwork() {
  portENTER_CRITICAL(&initMux);
  if(!initMutex)initMutex=xSemaphoreCreateMutexStatic(&initStorage);
  portEXIT_CRITICAL(&initMux);
  if(!initMutex || xSemaphoreTake(initMutex,pdMS_TO_TICKS(100))!=pdTRUE)return false;
  if(!eventsReady) {
    if(esp_event_handler_instance_register(WIFI_EVENT,ESP_EVENT_ANY_ID,networkEvent,nullptr,&wifiEvents)==ESP_OK) {
      if(esp_event_handler_instance_register(IP_EVENT,ESP_EVENT_ANY_ID,networkEvent,nullptr,&ipEvents)==ESP_OK)eventsReady=true;
      else {esp_event_handler_instance_unregister(WIFI_EVENT,ESP_EVENT_ANY_ID,wifiEvents);wifiEvents=nullptr;}
    }
  }
  const bool result=eventsReady;xSemaphoreGive(initMutex);return result;
}
// F4/F5 path order. Wi-Fi comes first unless its reachability hint is negative
// (and no probe is due) while the phone's is not. A negative hint never makes a
// path unusable: the only available path is always tried, and connect() still
// gives the other path one attempt after a Network failure before any write.
Path firstPath(LlmHttpIo& io,bool wifi,bool phone) {
  io.epoch=wifiEpoch.load();io.phoneGeneration=phonePipeGeneration();
  const uint32_t now=millis();
  Path first=wifi?Path::Wifi:(phone?Path::Phone:Path::None);
  portENTER_CRITICAL(&healthMux);
  health.context(io.epoch,io.revision);healthEpoch=io.epoch;healthRevision=io.revision;
  phoneHealth.context(io.phoneGeneration,io.revision);
  phoneHealthGeneration=io.phoneGeneration;phoneHealthRevision=io.revision;
  if(wifi && phone && !health.eligible() && !health.claimProbe(now) &&
     (phoneHealth.eligible() || phoneHealth.claimProbe(now)))first=Path::Phone;
  portEXIT_CRITICAL(&healthMux);
  return first;
}
const char* failureName(Failure failure) {
  switch(failure) {
    case Failure::Network:return "network";
    case Failure::Trust:return "tls-trust";
    case Failure::Cancelled:return "cancelled";
    case Failure::Protocol:return "protocol";
    case Failure::Resources:return "resources";
  }
  return "unknown";
}
void directResult(const LlmHttpIo& io,bool success) {
  if(!success) ESP_LOGW("LLM_TRANSPORT",
      "direct connect failed: kind=%u stage=%s category=%s socket_error=%d elapsed_ms=%lu deadline=%u internal_free=%u internal_largest=%u psram_free=%u",
      unsigned(io.kind),io.directStage,expired(io.directStarted,io.directBudget)?"timeout":failureName(io.failure),io.directSocketError,
      (unsigned long)(millis()-io.directStarted),unsigned(expired(io.directStarted,io.directBudget)),
      unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)),
      unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)),
      unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
  portENTER_CRITICAL(&healthMux);
  if(healthEpoch==io.epoch && healthRevision==io.revision && wifiEpoch.load()==io.epoch) {
    if(success || io.failure!=Failure::Network)health.succeeded();
    else health.networkFailed(millis());
  }
  portEXIT_CRITICAL(&healthMux);
}
// Only a completed phone TLS handshake clears the phone's negative hint; idle
// READY or a non-network failure leaves it unchanged. A new phone session or
// configuration revision starts from a clean hint.
void phoneResult(const LlmHttpIo& io,bool success) {
  portENTER_CRITICAL(&healthMux);
  if(phoneHealthGeneration==io.phoneGeneration && phoneHealthRevision==io.revision &&
     phonePipeGeneration()==io.phoneGeneration) {
    if(success)phoneHealth.succeeded();
    else if(io.failure==Failure::Network)phoneHealth.networkFailed(millis());
  }
  portEXIT_CRITICAL(&healthMux);
}

void closeChild(LlmHttpIo& io) {
  io.connected=false;
  if(io.direct){esp_tls_conn_destroy(io.direct);io.direct=nullptr;}
  if(io.pipe.id){phonePipeClose(io.pipe,true);phonePipeRelease(io.pipe);
    requestSlots[unsigned(io.kind)].retiring=io.pipe;io.pipe={};}
  if(io.tls){io.tls->~PhoneTls();ps_free(io.tls);io.tls=nullptr;}
}
Failure directFailure(esp_tls_t* child) {
  int code=0,flags=0;esp_tls_error_handle_t errors=nullptr;
  if(esp_tls_get_error_handle(child,&errors)!=ESP_OK || !errors)return Failure::Resources;
  const int error=esp_tls_get_and_clear_last_error(errors,&code,&flags);
  ESP_LOGW("LLM_TRANSPORT","direct TLS error: esp=%d tls=%d verify_flags=%d",error,code,flags);
  if(flags || error==ESP_ERR_MBEDTLS_SSL_SET_HOSTNAME_FAILED ||
     error==ESP_ERR_MBEDTLS_X509_CRT_PARSE_FAILED || error==ESP_ERR_MBEDTLS_CERT_PARTLY_OK ||
     code==MBEDTLS_ERR_X509_CERT_VERIFY_FAILED || code==-MBEDTLS_ERR_X509_CERT_VERIFY_FAILED)return Failure::Trust;
  const int tlsCode=code>0?-code:code;
  if(tlsCode==MBEDTLS_ERR_NET_CONNECT_FAILED || tlsCode==MBEDTLS_ERR_NET_CONN_RESET ||
     tlsCode==MBEDTLS_ERR_NET_SEND_FAILED || tlsCode==MBEDTLS_ERR_NET_RECV_FAILED)return Failure::Network;
  switch(error) {
    case ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME:
    case ESP_ERR_ESP_TLS_FAILED_CONNECT_TO_HOST:
    case ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT:return Failure::Network;
    case ESP_ERR_NO_MEM:case ESP_ERR_ESP_TLS_CANNOT_CREATE_SOCKET:
    case ESP_ERR_MBEDTLS_CTR_DRBG_SEED_FAILED:
    case ESP_ERR_MBEDTLS_SSL_CONFIG_DEFAULTS_FAILED:
    case ESP_ERR_MBEDTLS_SSL_SETUP_FAILED:return Failure::Resources;
    default:return Failure::Protocol;
  }
}
bool waitDirectConnect(LlmHttpIo& io,uint32_t start,uint32_t duration) {
  int socket=-1;
  if(esp_tls_get_conn_sockfd(io.direct,&socket)!=ESP_OK || socket<0) {
    io.failure=Failure::Resources;return false;
  }
  while(!cancelled(io) && !expired(start,duration)) {
    fd_set reads,writes;
    FD_ZERO(&reads);FD_SET(socket,&reads);writes=reads;
    const uint32_t elapsed=uint32_t(millis()-start);
    if(elapsed>=duration)break;
    const uint32_t left=duration-elapsed;
    timeval timeout{};timeout.tv_usec=std::min(uint32_t(10),left)*1000;
    const int result=select(socket+1,&reads,&writes,nullptr,&timeout);
    if(result<0) {
      if(errno==EINTR)continue;
      io.directSocketError=errno;
      io.failure=(errno==ENOMEM || errno==ENOBUFS)?Failure::Resources:Failure::Network;return false;
    }
    if(!result)continue;
    int error=0;socklen_t length=sizeof(error);
    if(getsockopt(socket,SOL_SOCKET,SO_ERROR,&error,&length)<0 || error) {
      io.directSocketError=error?error:errno;
      io.failure=(io.directSocketError==ENOMEM || io.directSocketError==ENOBUFS)?Failure::Resources:Failure::Network;return false;
    }
    if(cancelled(io) || expired(start,duration))break;
    return true;
  }
  io.failure=cancelled(io)?Failure::Cancelled:Failure::Network;return false;
}
bool connectDirect(LlmHttpIo& io,int port) {
  const uint32_t start=millis(),duration=budget(io,kDirectMs);char address[48];
  io.directStarted=start;io.directBudget=duration;
  if(!duration || !resolve(io,io.host,address,sizeof(address),start,duration))return false;
  io.directStage="tls-init";
  io.direct=esp_tls_init();
  if(!io.direct){io.failure=Failure::Resources;return false;}
  esp_tls_cfg_t config{};config.non_block=true;config.timeout_ms=10;
  config.is_plain_tcp=!io.secure;config.common_name=io.host;
  if(io.secure) {
    // The same public esp_tls backend used by esp_transport_ssl, with its real
    // asynchronous error handle. That stock wrapper does not copy async errors
    // into esp_transport_get_error_handle in the pinned SDK.
    if(io.ca[0]) {
      config.cacert_buf=reinterpret_cast<const unsigned char*>(io.ca);
      config.cacert_bytes=strlen(io.ca)+1;
    }
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    else config.crt_bundle_attach=esp_crt_bundle_attach;
#else
    else {io.failure=Failure::Trust;return false;}
#endif
  }
  while(!cancelled(io) && !expired(start,duration)) {
    io.directStage="tcp-or-tls";
    const uint32_t elapsed=uint32_t(millis()-start);
    if(elapsed>=duration)break;
    config.timeout_ms=std::min(uint32_t(10),duration-elapsed);
    const int result=esp_tls_conn_new_async(address,strlen(address),port,&config,io.direct);
    if(result<0){io.failure=directFailure(io.direct);return false;}
    if(cancelled(io) || expired(start,duration))break;
    if(result==1) {
      // IDF's plaintext async path returns before an EINPROGRESS connect ends.
      if(!io.secure && !waitDirectConnect(io,start,duration))return false;
      io.connected=true;return true;
    }
    esp_tls_conn_state_t state;
    if(esp_tls_get_conn_state(io.direct,&state)!=ESP_OK) {
      io.failure=Failure::Resources;return false;
    }
    if(state==ESP_TLS_CONNECTING) {
      io.directStage="tcp-connect";
      if(!waitDirectConnect(io,start,duration))return false;
      // IDF 5.5.5 reuses select's cleared fd_sets after a connect timeout.
      // TCP is now confirmed: skip that internal select on the next call.
      // This flag changes socket mode only in ESP_TLS_INIT; the existing
      // socket stays O_NONBLOCK throughout setup, handshake and later I/O.
      config.non_block=false;
      continue;
    }
    pause();
  }
  io.failure=cancelled(io)?Failure::Cancelled:Failure::Network;return false;
}
int bioSend(void* opaque,const unsigned char* bytes,size_t size) {
  auto& io=*static_cast<LlmHttpIo*>(opaque);
  if(cancelled(io))return MBEDTLS_ERR_NET_SEND_FAILED;
  const int n=phonePipeWrite(io.pipe,bytes,size);
  if(n<0){io.networkError=true;return MBEDTLS_ERR_NET_SEND_FAILED;}
  return n?n:MBEDTLS_ERR_SSL_WANT_WRITE;
}
int bioReceive(void* opaque,unsigned char* bytes,size_t size) {
  auto& io=*static_cast<LlmHttpIo*>(opaque);
  if(cancelled(io))return MBEDTLS_ERR_NET_RECV_FAILED;
  const int n=phonePipeRead(io.pipe,bytes,size);
  if(n==-2){io.networkError=true;return 0;}
  if(n<0){io.networkError=true;return MBEDTLS_ERR_NET_RECV_FAILED;}
  return n?n:MBEDTLS_ERR_SSL_WANT_READ;
}
bool retryTls(int result) {
  if(result==MBEDTLS_ERR_SSL_WANT_READ || result==MBEDTLS_ERR_SSL_WANT_WRITE)return true;
#ifdef MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET
  if(result==MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)return true;
#endif
  return false;
}
bool configurePhoneTls(LlmHttpIo& io) {
  auto* memory=ps_alloc(sizeof(PhoneTls),AllocPref::RequireInternal,"llm.phone.tls");
  if(!memory){io.failure=Failure::Resources;return false;}
  io.tls=new(memory)PhoneTls;auto& t=*io.tls;
#if CONFIG_MBEDTLS_SSL_PROTO_TLS1_3
  if(psa_crypto_init()!=PSA_SUCCESS)return false;
#endif
  // Same linked mbedTLS defaults, entropy/DRBG and name verification as
  // esp_tls_mbedtls.c. No private esp_tls fields or permissive callback.
  if(mbedtls_ctr_drbg_seed(&t.rng,mbedtls_entropy_func,&t.entropy,nullptr,0)!=0 ||
     mbedtls_ssl_config_defaults(&t.config,MBEDTLS_SSL_IS_CLIENT,MBEDTLS_SSL_TRANSPORT_STREAM,MBEDTLS_SSL_PRESET_DEFAULT)!=0)
    return false;
  mbedtls_ssl_conf_rng(&t.config,mbedtls_ctr_drbg_random,&t.rng);
  mbedtls_ssl_conf_authmode(&t.config,MBEDTLS_SSL_VERIFY_REQUIRED);
#if CONFIG_MBEDTLS_SSL_RENEGOTIATION
  mbedtls_ssl_conf_renegotiation(&t.config,MBEDTLS_SSL_RENEGOTIATION_ENABLED);
#endif
  io.failure=Failure::Trust;
  if(io.ca[0]) {
    if(mbedtls_x509_crt_parse(&t.ca,reinterpret_cast<const unsigned char*>(io.ca),strlen(io.ca)+1)<0)return false;
    mbedtls_ssl_conf_ca_chain(&t.config,&t.ca,nullptr);
  } else {
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    if(esp_crt_bundle_attach(&t.config)!=ESP_OK)return false;
#else
    return false;
#endif
  }
  io.failure=Failure::Resources;
  if(mbedtls_ssl_setup(&t.ssl,&t.config)!=0 || mbedtls_ssl_set_hostname(&t.ssl,io.host)!=0)return false;
  mbedtls_ssl_set_bio(&t.ssl,&io,bioSend,bioReceive,nullptr);
  return true;
}
// F5: the cause of a failed phone OPEN, as a routing class. Every one of these
// happens before OPEN_OK, so no HTTP byte can have gone through the phone.
Failure phoneOpenFailure(const PhonePipeOpenResult& result) {
  switch(result.status) {
    case PhonePipeOpenStatus::Refused:return hw1httptransport::openRefusal(result.code);
    case PhonePipeOpenStatus::Rejected:return Failure::Resources;
    case PhonePipeOpenStatus::NoBudget:case PhonePipeOpenStatus::Cancelled:return Failure::Cancelled;
    // Stale READY or silent loss: bounded by the A-1 phases, then one Wi-Fi try.
    case PhonePipeOpenStatus::NotReady:case PhonePipeOpenStatus::SubmitTimeout:
    case PhonePipeOpenStatus::ResponseTimeout:case PhonePipeOpenStatus::LinkLost:
    case PhonePipeOpenStatus::Opened:break;
  }
  return Failure::Network;
}
bool connectPhone(LlmHttpIo& io,int port) {
  io.phoneOpen=phonePipeOpen(io.pipe,io.host,uint16_t(port),budget(io,UINT32_MAX),*io.cancelled,&io.interrupted);
  io.phoneCorr=io.pipe.corr;io.phoneStream=io.pipe.id;
  if(cancelled(io)){io.failure=Failure::Cancelled;return false;}
  if(io.phoneOpen.status!=PhonePipeOpenStatus::Opened){io.failure=phoneOpenFailure(io.phoneOpen);return false;}
  io.failure=Failure::Network; // until TLS settles: a late or failed handshake is pre-write
  // OPEN_OK starts the relay's timeout; include worker wakeup and TLS setup.
  const uint32_t start=io.pipe.openedAt;
  const uint32_t elapsed=uint32_t(millis()-start);
  if(elapsed>=kHandshakeMs)return false;
  const uint32_t duration=elapsed+budget(io,kHandshakeMs-elapsed);
  io.failure=Failure::Resources;
  if(!configurePhoneTls(io))return false;
  while(!cancelled(io) && !expired(start,duration)) {
    const int result=mbedtls_ssl_handshake(&io.tls->ssl);
    const auto flags=mbedtls_ssl_get_verify_result(&io.tls->ssl);
    if(cancelled(io) || expired(start,duration))break;
    if(!result) {
      if(flags){io.failure=Failure::Trust;return false;}
      io.connected=true;return true;
    }
    if(!retryTls(result)) {
      io.failure=flags || result==MBEDTLS_ERR_X509_CERT_VERIFY_FAILED?Failure::Trust:
          (io.networkError?Failure::Network:Failure::Protocol);
      return false;
    }
    pause();
  }
  io.failure=cancelled(io)?Failure::Cancelled:Failure::Network;return false;
}
int connect(esp_transport_handle_t transport,const char* host,int port,int) {
  auto& io=context(transport);
  if(io.begun || cancelled(io) || !host || !*host || strlen(host)>253 || port<=0 || port>65535)return -1;
  io.begun=true;strlcpy(io.host,host,sizeof(io.host));
  // The phone is a path only while READY with its relay on (RELAY_ON).
  for(Path path=firstPath(io,WiFi.isConnected(),io.secure && phonePipeRelayAvailable());path!=Path::None;) {
    if(cancelled(io) || !io.attempt.select(path))return -1;
    const bool wifi=path==Path::Wifi;
    const bool success=wifi?connectDirect(io,port):connectPhone(io,port);
    if(wifi)directResult(io,success);else phoneResult(io,success);
    auto& outcome=wifi?io.wifiOutcome:io.phoneOutcome;
    outcome.tried=true;outcome.connected=success && !cancelled(io);
    if(outcome.connected)return 0;
    if(cancelled(io))io.failure=Failure::Cancelled;
    outcome.failure=io.failure;
    closeChild(io);
    // Never before an HTTP write, never back to a path already tried.
    const Path next=wifi?Path::Phone:Path::Wifi;
    const bool nextUp=wifi?(io.secure && phonePipeRelayAvailable()):WiFi.isConnected();
    if(!io.attempt.fallback(io.failure,next,nextUp))return -1;
    path=next;
  }
  return -1;
}
int read(esp_transport_handle_t transport,char* bytes,int length,int timeout) {
  auto& io=context(transport);
  if(!io.connected || cancelled(io) || length<=0)return ERR_TCP_TRANSPORT_CONNECTION_FAILED;
  const uint32_t start=millis(),duration=budget(io,std::min(kIoMs,uint32_t(std::max(timeout,0))));
  do {
    if(cancelled(io))return ERR_TCP_TRANSPORT_CONNECTION_FAILED;
    errno=0;
    const int result=io.direct?int(esp_tls_conn_read(io.direct,bytes,size_t(length))):
        mbedtls_ssl_read(&io.tls->ssl,reinterpret_cast<unsigned char*>(bytes),size_t(length));
    if(result>0)return result;
    if(result==0 || result==MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)return ERR_TCP_TRANSPORT_CONNECTION_CLOSED_BY_FIN;
    const bool plainAgain=io.direct && !io.secure && result<0 && (errno==EAGAIN || errno==EWOULDBLOCK);
    if(!plainAgain && !retryTls(result))return ERR_TCP_TRANSPORT_CONNECTION_FAILED;
    if(!duration || expired(start,duration))return ERR_TCP_TRANSPORT_CONNECTION_TIMEOUT;
    pause();
  } while(true);
}
int write(esp_transport_handle_t transport,const char* bytes,int length,int timeout) {
  auto& io=context(transport);
  if(length<=0)return length;
  // This boundary includes headers sent by esp_http_client_open. Even a local
  // failure is conservatively MAY_HAVE_SENT; only BIO handshake writes bypass it.
  io.attempt.beforeHttpWrite();
  if(!io.connected || cancelled(io))return -1;
  const uint32_t start=millis(),duration=budget(io,std::min(kIoMs,uint32_t(std::max(timeout,0))));
  do {
    if(cancelled(io))return -1;
    errno=0;
    const int result=io.direct?int(esp_tls_conn_write(io.direct,bytes,size_t(length))):
        mbedtls_ssl_write(&io.tls->ssl,reinterpret_cast<const unsigned char*>(bytes),size_t(length));
    if(result>0)return result;
    const bool plainAgain=io.direct && !io.secure && result<0 && (errno==EAGAIN || errno==EWOULDBLOCK);
    if(!plainAgain && !retryTls(result))return -1;
    // Same plaintext pointer/length retained on WANT_WRITE, as required by TLS.
    if(!duration || expired(start,duration))return -1;
    pause();
  } while(true);
}
int pollRead(esp_transport_handle_t transport,int) {
  auto& io=context(transport);
  // read owns the bounded wait and checks buffered TLS plaintext before BIO.
  return io.connected && !cancelled(io)?1:-1;
}
int pollWrite(esp_transport_handle_t transport,int){return pollRead(transport,0);}
int close(esp_transport_handle_t transport) {
  auto& io=context(transport);io.attempt.finish();closeChild(io);return 0;
}
int destroy(esp_transport_handle_t transport) {
  auto* io=&context(transport);close(transport);io->~LlmHttpIo();ps_free(io);return 0;
}

// F3: one non-sensitive record per logical request (no host, port, token or
// content), bounded counters and the last few records for `relay status`.
// "carried" is the path that may have carried HTTP bytes; None means no HTTP
// byte was written on any path, so the request was never delivered.
struct PathRecord {
  uint32_t seq=0,ms=0;uint64_t corr=0;uint16_t stream=0;
  uint8_t kind=0,first=0,carried=0,wifi=0,phone=0,open=0,code=0;int8_t outcome=-1;
};
struct PathStats {
  uint32_t requests=0,carried[3]{},toPhone=0,toWifi=0;
  uint32_t completed=0,failedBeforeSend=0,failedAfterSend=0,unreported=0;
  uint32_t open[9]{},refusal[hw1httptransport::kRefusalCodes]{};
};
constexpr unsigned kRecent=8;
PathStats pathStats;
PathRecord recent[kRecent];
uint32_t recordSeq=0;
// 0 = not tried, 1 = connected, 2 + Failure otherwise.
uint8_t pathCode(const LlmHttpIo::PathOutcome& o){return !o.tried?0:o.connected?1:uint8_t(2+unsigned(o.failure));}
const char* pathCodeName(uint8_t code) {
  static const char* names[]={"-","ok","network","tls-trust","cancelled","protocol","resources"};
  return code<7?names[code]:"?";
}
const char* pathName(uint8_t path){return path==uint8_t(Path::Wifi)?"wifi":path==uint8_t(Path::Phone)?"phone":"none";}
const char* openName(uint8_t status) {
  static const char* names[]={"opened","refused","not-ready","rejected","no-budget","submit-timeout",
    "response-timeout","link-lost","cancelled"};
  return status<9?names[status]:"?";
}
const char* outcomeName(const PathRecord& r) {
  if(r.outcome==1)return "completed";
  if(r.outcome<0)return "unreported";
  return r.carried?"failed-after-send":"failed-before-send";
}
size_t formatRecord(const PathRecord& r,char* out,size_t cap) {
  const int n=snprintf(out,cap,"#%lu %s first=%s carried=%s wifi=%s phone=%s open=%s:%u outcome=%s corr=%016llx stream=%u ms=%lu",
      (unsigned long)r.seq,r.kind?"terminal":"chat",pathName(r.first),pathName(r.carried),pathCodeName(r.wifi),
      pathCodeName(r.phone),r.phone?openName(r.open):"-",unsigned(r.code),outcomeName(r),
      (unsigned long long)r.corr,unsigned(r.stream),(unsigned long)r.ms);
  return n<0?0:std::min(size_t(n),cap?cap-1:0);
}
size_t appendf(char* out,size_t cap,size_t used,const char* format,...) __attribute__((format(printf,4,5)));
size_t appendf(char* out,size_t cap,size_t used,const char* format,...) {
  if(used+1>=cap)return used;
  va_list args;va_start(args,format);const int n=vsnprintf(out+used,cap-used,format,args);va_end(args);
  return n>0?used+std::min(size_t(n),cap-used-1):used;
}
void record(const LlmHttpIo& io) {
  if(!io.begun)return;
  PathRecord r;
  r.ms=uint32_t(millis()-io.started);r.kind=uint8_t(io.kind);r.first=uint8_t(io.attempt.first);
  r.carried=io.attempt.mayHaveSent?uint8_t(io.attempt.path):0;
  r.wifi=pathCode(io.wifiOutcome);r.phone=pathCode(io.phoneOutcome);
  r.open=uint8_t(io.phoneOpen.status);r.code=uint8_t(io.phoneOpen.code&0xff);r.outcome=io.outcome;
  if(io.phoneOutcome.tried){r.corr=io.phoneCorr;r.stream=io.phoneStream;}
  portENTER_CRITICAL(&healthMux);
  r.seq=++recordSeq;
  auto& st=pathStats;++st.requests;++st.carried[r.carried<3?r.carried:0];
  if(io.attempt.first==Path::Wifi && io.phoneOutcome.tried)++st.toPhone;
  if(io.attempt.first==Path::Phone && io.wifiOutcome.tried)++st.toWifi;
  if(r.outcome==1)++st.completed;
  else if(r.outcome<0)++st.unreported;
  else if(r.carried)++st.failedAfterSend;
  else ++st.failedBeforeSend;
  if(io.phoneOutcome.tried) {
    ++st.open[r.open<9?r.open:0];
    if(io.phoneOpen.status==PhonePipeOpenStatus::Refused)++st.refusal[hw1httptransport::refusalSlot(io.phoneOpen.code)];
  }
  recent[(r.seq-1)%kRecent]=r;
  portEXIT_CRITICAL(&healthMux);
  char line[200];formatRecord(r,line,sizeof(line));
  ESP_LOGI("LLM_PATH","%s",line);
}
#endif
}
LlmHttpIo* llmHttpIoCreate(const char* ca,bool secure,const std::atomic<bool>& cancel,
                         uint32_t started,uint32_t total,uint32_t revision,LlmHttpRequestKind kind) {
  if(!ca || !total)return nullptr;
  auto* memory=ps_alloc(sizeof(LlmHttpIo),AllocPref::PreferPSRAM,"llm.http.io");
  if(!memory)return nullptr;
  auto* io=new(memory)LlmHttpIo;
  io->kind=kind;io->ca=ca;io->secure=secure;io->cancelled=&cancel;io->started=started;io->total=total;io->revision=revision;
#if ENABLE_BLE_PHONE_PIPE
  if(!watchNetwork()){io->~LlmHttpIo();ps_free(io);return nullptr;}
  io->transport=esp_transport_init();
  if(!io->transport){io->~LlmHttpIo();ps_free(io);return nullptr;}
  esp_transport_set_context_data(io->transport,io);
  esp_transport_set_default_port(io->transport,secure?443:80);
  esp_transport_set_func(io->transport,connect,read,write,close,pollRead,pollWrite,destroy);
#endif
  return io;
}
bool llmHttpIoConfigure(LlmHttpIo* io,esp_http_client_config_t& config) {
  if(!io || cancelled(*io))return false;
#if ENABLE_BLE_PHONE_PIPE
  config.transport=io->transport;
#endif
  // Also configure the stock path for builds with the pipe disabled.
  if(io->secure) {
    if(io->ca[0])config.cert_pem=io->ca;
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    else config.crt_bundle_attach=esp_crt_bundle_attach;
#else
    else return false;
#endif
  }
  return true;
}
void llmHttpIoConnected(LlmHttpIo* io,esp_http_client_handle_t client) {
  if(!io)return;
#if ENABLE_BLE_PHONE_PIPE
  if(io->direct)(void)esp_tls_get_conn_sockfd(io->direct,&io->socket);
  (void)client;
#else
  io->socket=esp_http_client_get_socket(client);
#endif
  if(cancelled(*io) && io->socket>=0)(void)shutdown(io->socket,SHUT_RDWR);
}
void llmHttpIoCancel(LlmHttpIo* io) {
  if(!io)return;
  io->interrupted=true;
  if(io->socket>=0)(void)shutdown(io->socket,SHUT_RDWR);
}
void llmHttpIoFinish(LlmHttpIo* io,bool completed) {
  if(io)io->outcome=completed?1:0;
}
void llmHttpIoDestroy(LlmHttpIo* io) {
  if(!io)return;
#if ENABLE_BLE_PHONE_PIPE
  record(*io);
  esp_transport_destroy(io->transport);
#else
  io->~LlmHttpIo();ps_free(io);
#endif
}
LlmHttpAdmission llmHttpRequestAdmit(LlmHttpRequestKind kind,bool secure) {
  if(!llmHttpPathAvailable(secure))return LlmHttpAdmission::Unavailable;
  auto& slot=requestSlots[unsigned(kind)];
  if(slot.claimed.exchange(true,std::memory_order_acquire))return LlmHttpAdmission::Busy;
#if ENABLE_BLE_PHONE_PIPE
  if(!phonePipeRetired(slot.retiring)) {
    slot.claimed.store(false,std::memory_order_release);return LlmHttpAdmission::Busy;
  }
  slot.retiring={};
#endif
  return LlmHttpAdmission::Accepted;
}
void llmHttpRequestRelease(LlmHttpRequestKind kind) {
  requestSlots[unsigned(kind)].claimed.store(false,std::memory_order_release);
}
bool llmHttpIoUsedPhone(const LlmHttpIo* io) {
#if ENABLE_BLE_PHONE_PIPE
  return io && io->attempt.path==Path::Phone;
#else
  (void)io;return false;
#endif
}
bool llmHttpPathAvailable(bool secure) {
  // A READY link with the phone's relay off carries the console only.
  return WiFi.isConnected() || (secure && phonePipeRelayAvailable());
}
size_t llmHttpPathReport(char* out,size_t cap) {
  if(!out || !cap)return 0;
  out[0]=0;
#if ENABLE_BLE_PHONE_PIPE
  PathStats st;PathRecord rows[kRecent];uint32_t last=0;
  portENTER_CRITICAL(&healthMux);
  st=pathStats;last=recordSeq;for(unsigned i=0;i<kRecent;++i)rows[i]=recent[i];
  portEXIT_CRITICAL(&healthMux);
  size_t used=appendf(out,cap,0,"requests=%lu carried wifi=%lu phone=%lu none=%lu fallback wifi>phone=%lu phone>wifi=%lu\n",
      (unsigned long)st.requests,(unsigned long)st.carried[1],(unsigned long)st.carried[2],(unsigned long)st.carried[0],
      (unsigned long)st.toPhone,(unsigned long)st.toWifi);
  used=appendf(out,cap,used,"outcomes completed=%lu failed_before_send=%lu failed_after_send=%lu unreported=%lu\nphone_open:",
      (unsigned long)st.completed,(unsigned long)st.failedBeforeSend,(unsigned long)st.failedAfterSend,(unsigned long)st.unreported);
  for(unsigned i=0;i<9;++i)if(st.open[i])used=appendf(out,cap,used," %s=%lu",openName(uint8_t(i)),(unsigned long)st.open[i]);
  used=appendf(out,cap,used,"\nopen_err:");
  for(unsigned i=0;i<hw1httptransport::kRefusalCodes;++i)if(st.refusal[i])used=appendf(out,cap,used," %u=%lu",i,(unsigned long)st.refusal[i]);
  used=appendf(out,cap,used,"\nrecent:");
  const uint32_t count=last<kRecent?last:kRecent;
  for(uint32_t seq=last-count+1;count && seq<=last;++seq) {
    used=appendf(out,cap,used,"\n");
    if(used+1<cap)used+=formatRecord(rows[(seq-1)%kRecent],out+used,cap-used);
  }
  return used;
#else
  return size_t(snprintf(out,cap,"transport: Wi-Fi only (phone pipe not compiled)"));
#endif
}
#endif
