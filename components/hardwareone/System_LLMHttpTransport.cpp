#include "System_LLMHttpTransport.h"
#if ENABLE_LLM_BACKEND && ENABLE_LLM_SOURCE_HTTP
#include "System_LLMHttpTransportCore.h"
#include "System_MemUtil.h"
#include "System_Clock.h"
#include "BLE_PhonePipe.h"
#include <WiFi.h>
#include <esp_transport.h>
#include <esp_tls.h>
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
#include <cstring>
#include <new>
#include <sys/socket.h>

#if ENABLE_BLE_PHONE_PIPE && !CONFIG_ESP_HTTP_CLIENT_ENABLE_CUSTOM_TRANSPORT
#error "Phone HTTP routing requires CONFIG_ESP_HTTP_CLIENT_ENABLE_CUSTOM_TRANSPORT=y in the saved SDK choices"
#endif

using hw1httptransport::Attempt;
using hw1httptransport::Path;
using hw1httptransport::Failure;
namespace {
#if ENABLE_BLE_PHONE_PIPE
constexpr uint32_t kDirectMs=2000,kOpenMs=3000,kHandshakeMs=8000,kIoMs=2000;
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
  int socket=-1; // published after connect, protected by the caller request lock
#if ENABLE_BLE_PHONE_PIPE
  Attempt attempt;
  esp_transport_handle_t transport=nullptr;
  esp_tls_t* direct=nullptr;
  PhoneTls* tls=nullptr;
  PhonePipeStream pipe;
  Failure failure=Failure::Resources;
  uint32_t epoch=0;
  bool begun=false,connected=false,networkError=false;
  char host[254]{};
#endif
};
namespace {
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
struct DnsSlot {DnsState state=DnsState::Free;char name[254]{};ip_addr_t address{};bool ok=false;};
DnsSlot dnsSlots[4];
portMUX_TYPE dnsMux=portMUX_INITIALIZER_UNLOCKED;
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
  const err_t result=dns_gethostbyname_addrtype(slot.name,&address,dnsDone,&slot,type);
  if(result==ERR_OK)dnsDone(nullptr,&address,&slot);
  else if(result!=ERR_INPROGRESS)dnsDone(nullptr,nullptr,&slot);
}
bool resolve(LlmHttpIo& io,const char* host,char* out,size_t cap,uint32_t start,uint32_t duration) {
  ip_addr_t address;
  if(ipaddr_aton(host,&address))return ipaddr_ntoa_r(&address,out,cap)!=nullptr;
  DnsSlot* slot=nullptr;
  portENTER_CRITICAL(&dnsMux);
  for(auto& candidate:dnsSlots)if(candidate.state==DnsState::Free) {
    slot=&candidate;strlcpy(slot->name,host,sizeof(slot->name));slot->state=DnsState::Pending;break;
  }
  portEXIT_CRITICAL(&dnsMux);
  if(!slot){io.failure=Failure::Resources;return false;}
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

hw1httptransport::Health health;
uint32_t healthEpoch=0,healthRevision=0;
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
bool preferDirect(LlmHttpIo& io,bool phone) {
  io.epoch=wifiEpoch.load();
  portENTER_CRITICAL(&healthMux);
  health.context(io.epoch,io.revision);healthEpoch=io.epoch;healthRevision=io.revision;
  const bool result=!phone || health.directEligible() || health.claimProbe(millis());
  portEXIT_CRITICAL(&healthMux);
  return result;
}
void directResult(const LlmHttpIo& io,bool success) {
  portENTER_CRITICAL(&healthMux);
  if(healthEpoch==io.epoch && healthRevision==io.revision && wifiEpoch.load()==io.epoch) {
    if(success || io.failure!=Failure::Network)health.succeeded();
    else health.networkFailed(millis());
  }
  portEXIT_CRITICAL(&healthMux);
}

void closeChild(LlmHttpIo& io) {
  io.connected=false;
  if(io.direct){esp_tls_conn_destroy(io.direct);io.direct=nullptr;}
  if(io.pipe.id){phonePipeClose(io.pipe,true);phonePipeRelease(io.pipe);io.pipe={};}
  if(io.tls){io.tls->~PhoneTls();ps_free(io.tls);io.tls=nullptr;}
}
Failure directFailure(esp_tls_t* child) {
  int code=0,flags=0;esp_tls_error_handle_t errors=nullptr;
  if(esp_tls_get_error_handle(child,&errors)!=ESP_OK || !errors)return Failure::Resources;
  const int error=esp_tls_get_and_clear_last_error(errors,&code,&flags);
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
bool connectDirect(LlmHttpIo& io,int port) {
  const uint32_t start=millis(),duration=budget(io,kDirectMs);char address[48];
  if(!duration || !resolve(io,io.host,address,sizeof(address),start,duration))return false;
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
    const int result=esp_tls_conn_new_async(address,strlen(address),port,&config,io.direct);
    if(result==1){io.connected=true;return true;}
    if(result<0){io.failure=directFailure(io.direct);return false;}
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
  auto* memory=ps_alloc(sizeof(PhoneTls),AllocPref::PreferPSRAM,"llm.phone.tls");
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
#if defined(MBEDTLS_SSL_SESSION_TICKETS) && defined(MBEDTLS_SSL_CLI_C)
  mbedtls_ssl_conf_session_tickets(&t.config,MBEDTLS_SSL_SESSION_TICKETS_DISABLED);
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
bool connectPhone(LlmHttpIo& io,int port) {
  io.failure=Failure::Network;
  if(!phonePipeOpen(io.pipe,io.host,uint16_t(port),budget(io,kOpenMs),*io.cancelled,&io.interrupted))return false;
  if(cancelled(io)){io.failure=Failure::Cancelled;return false;}
  io.failure=Failure::Resources;
  if(!configurePhoneTls(io))return false;
  const uint32_t start=millis(),duration=budget(io,kHandshakeMs);
  while(!cancelled(io) && !expired(start,duration)) {
    const int result=mbedtls_ssl_handshake(&io.tls->ssl);
    const auto flags=mbedtls_ssl_get_verify_result(&io.tls->ssl);
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
  const bool phone=io.secure && phonePipeReady();
  if(WiFi.isConnected() && preferDirect(io,phone)) {
    if(!io.attempt.select(Path::Wifi))return -1;
    const bool success=connectDirect(io,port);directResult(io,success);
    if(success && !cancelled(io))return 0;
    if(cancelled(io))io.failure=Failure::Cancelled;
    closeChild(io);
    if(!io.attempt.fallback(io.failure,io.secure && phonePipeReady()))return -1;
  }
  if(!io.secure || !phonePipeReady() || cancelled(io) || !io.attempt.select(Path::Phone))return -1;
  if(connectPhone(io,port) && !cancelled(io))return 0;
  closeChild(io);return -1;
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
#endif
}
LlmHttpIo* llmHttpIoCreate(const char* ca,bool secure,const std::atomic<bool>& cancel,
                         uint32_t started,uint32_t total,uint32_t revision) {
  if(!ca || !total)return nullptr;
  auto* memory=ps_alloc(sizeof(LlmHttpIo),AllocPref::PreferPSRAM,"llm.http.io");
  if(!memory)return nullptr;
  auto* io=new(memory)LlmHttpIo;
  io->ca=ca;io->secure=secure;io->cancelled=&cancel;io->started=started;io->total=total;io->revision=revision;
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
  // Same clock gate for both paths; an offline boot must not bypass time checks.
  if(io->secure && !Clock::isPlausibleEpoch(Clock::epochSeconds()))return false;
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
void llmHttpIoDestroy(LlmHttpIo* io) {
  if(!io)return;
#if ENABLE_BLE_PHONE_PIPE
  esp_transport_destroy(io->transport);
#else
  io->~LlmHttpIo();ps_free(io);
#endif
}
bool llmHttpPathAvailable(bool secure) {
  return WiFi.isConnected() || (secure && phonePipeReady());
}
#endif
