#include "System_LLMHttpTransportCore.h"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <new>
#include <string>
#include <vector>
#define ENABLE_LLM_BACKEND 1
#define ENABLE_LLM_SOURCE_HTTP 1
#define ENABLE_BLE_PHONE_PIPE 1
#define CONFIG_ESP_HTTP_CLIENT_ENABLE_CUSTOM_TRANSPORT 1
#define CONFIG_MBEDTLS_CERTIFICATE_BUNDLE 1
#define CONFIG_MBEDTLS_SSL_PROTO_TLS1_3 0
#define CONFIG_MBEDTLS_SSL_RENEGOTIATION 1
#define MBEDTLS_SSL_SESSION_TICKETS 1
#define MBEDTLS_SSL_CLI_C 1
#define LWIP_IPV6 1
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
#define pdMS_TO_TICKS(x) (x)
constexpr int pdTRUE=1,portMUX_INITIALIZER_UNLOCKED=0;
using portMUX_TYPE=int;using StaticSemaphore_t=int;using SemaphoreHandle_t=int*;
static SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t* p){return p;}
static int xSemaphoreTake(SemaphoreHandle_t,unsigned){return pdTRUE;}
static void xSemaphoreGive(SemaphoreHandle_t){}
static uint32_t now=100;
static std::function<void()> delayHook;
static uint32_t millis(){return now;}
static void vTaskDelay(unsigned amount){now+=amount;if(delayHook)delayHook();}
static size_t hw1Strlcpy(char* out,const char* in,size_t cap){size_t n=std::strlen(in);if(cap){size_t take=std::min(n,cap-1);std::memcpy(out,in,take);out[take]=0;}return n;}
#define strlcpy hw1Strlcpy
enum class AllocPref{PreferPSRAM};
static unsigned allocations=0;
static void* ps_alloc(size_t n,AllocPref,const char*){++allocations;return std::malloc(n);}
static void ps_free(void* p){if(p){--allocations;std::free(p);}}
static int shutdown(int,int){return 0;}
constexpr int SHUT_RDWR=2;
struct Wifi {bool up=true;bool isConnected(){return up;}}WiFi;
using esp_err_t=int;
constexpr int ESP_OK=0,ESP_ERR_NO_MEM=11;
constexpr int ESP_ERR_MBEDTLS_SSL_SET_HOSTNAME_FAILED=12,ESP_ERR_MBEDTLS_X509_CRT_PARSE_FAILED=13,
 ESP_ERR_MBEDTLS_CERT_PARTLY_OK=14,ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME=15,
 ESP_ERR_ESP_TLS_FAILED_CONNECT_TO_HOST=16,ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT=17,
 ESP_ERR_ESP_TLS_CANNOT_CREATE_SOCKET=18,ESP_ERR_MBEDTLS_CTR_DRBG_SEED_FAILED=19,
 ESP_ERR_MBEDTLS_SSL_CONFIG_DEFAULTS_FAILED=20,ESP_ERR_MBEDTLS_SSL_SETUP_FAILED=21;
constexpr int MBEDTLS_ERR_X509_CERT_VERIFY_FAILED=-100,MBEDTLS_ERR_NET_CONNECT_FAILED=-101,
 MBEDTLS_ERR_NET_CONN_RESET=-102,MBEDTLS_ERR_NET_SEND_FAILED=-103,MBEDTLS_ERR_NET_RECV_FAILED=-104,
 MBEDTLS_ERR_SSL_WANT_READ=-105,MBEDTLS_ERR_SSL_WANT_WRITE=-106,MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY=-107;
constexpr int MBEDTLS_SSL_IS_CLIENT=0,MBEDTLS_SSL_TRANSPORT_STREAM=0,MBEDTLS_SSL_PRESET_DEFAULT=0,
 MBEDTLS_SSL_VERIFY_REQUIRED=2,MBEDTLS_SSL_RENEGOTIATION_ENABLED=1,MBEDTLS_SSL_SESSION_TICKETS_DISABLED=0;
constexpr int ERR_TCP_TRANSPORT_CONNECTION_FAILED=-2,ERR_TCP_TRANSPORT_CONNECTION_CLOSED_BY_FIN=-1,
 ERR_TCP_TRANSPORT_CONNECTION_TIMEOUT=0;
struct mbedtls_ssl_config {int auth=0;bool ca=false,bundle=false;};
struct mbedtls_x509_crt{};struct mbedtls_ctr_drbg_context{};struct mbedtls_entropy_context{};
struct mbedtls_ssl_context {
 mbedtls_ssl_config* config=nullptr;std::string host;void* bio=nullptr;
 int(*send)(void*,const unsigned char*,size_t)=nullptr;
 int(*recv)(void*,unsigned char*,size_t)=nullptr;
 const unsigned char* pending=nullptr;size_t length=0;unsigned writes=0;
};
static int tlsHandshake=0,tlsFlags=0,parseResult=0;
static bool tlsWriteWant=false;
static unsigned handshakes=0;
static void mbedtls_ssl_init(mbedtls_ssl_context*){}
static void mbedtls_ssl_config_init(mbedtls_ssl_config*){}
static void mbedtls_x509_crt_init(mbedtls_x509_crt*){}
static void mbedtls_ctr_drbg_init(mbedtls_ctr_drbg_context*){}
static void mbedtls_entropy_init(mbedtls_entropy_context*){}
static void mbedtls_ssl_free(mbedtls_ssl_context*){}
static void mbedtls_ssl_config_free(mbedtls_ssl_config*){}
static void mbedtls_x509_crt_free(mbedtls_x509_crt*){}
static void mbedtls_ctr_drbg_free(mbedtls_ctr_drbg_context*){}
static void mbedtls_entropy_free(mbedtls_entropy_context*){}
static int mbedtls_entropy_func(void*,unsigned char*,size_t){return 0;}
static int mbedtls_ctr_drbg_random(void*,unsigned char*,size_t){return 0;}
static int mbedtls_ctr_drbg_seed(mbedtls_ctr_drbg_context*,int(*)(void*,unsigned char*,size_t),void*,const unsigned char*,size_t){return 0;}
static int mbedtls_ssl_config_defaults(mbedtls_ssl_config*,int,int,int){return 0;}
static void mbedtls_ssl_conf_rng(mbedtls_ssl_config*,int(*)(void*,unsigned char*,size_t),void*){}
static void mbedtls_ssl_conf_authmode(mbedtls_ssl_config* c,int value){c->auth=value;}
static void mbedtls_ssl_conf_renegotiation(mbedtls_ssl_config*,int){}
static void mbedtls_ssl_conf_session_tickets(mbedtls_ssl_config*,int value){assert(value==0);}
static int mbedtls_x509_crt_parse(mbedtls_x509_crt*,const unsigned char* ca,size_t n){assert(ca[n-1]==0);return parseResult;}
static void mbedtls_ssl_conf_ca_chain(mbedtls_ssl_config* c,mbedtls_x509_crt*,void*){c->ca=true;}
static int esp_crt_bundle_attach(void* config){static_cast<mbedtls_ssl_config*>(config)->bundle=true;return 0;}
static int mbedtls_ssl_setup(mbedtls_ssl_context* s,mbedtls_ssl_config* c){s->config=c;return 0;}
static int mbedtls_ssl_set_hostname(mbedtls_ssl_context* s,const char* h){s->host=h;return 0;}
static void mbedtls_ssl_set_bio(mbedtls_ssl_context* s,void* ctx,int(*send)(void*,const unsigned char*,size_t),int(*recv)(void*,unsigned char*,size_t),void*){s->bio=ctx;s->send=send;s->recv=recv;}
static int mbedtls_ssl_handshake(mbedtls_ssl_context* s){++handshakes;assert(s->config->auth==2 && (s->config->ca!=s->config->bundle));return tlsHandshake;}
static uint32_t mbedtls_ssl_get_verify_result(mbedtls_ssl_context*){return uint32_t(tlsFlags);}
static int mbedtls_ssl_read(mbedtls_ssl_context* s,unsigned char* out,size_t n){return s->recv(s->bio,out,n);}
static int mbedtls_ssl_write(mbedtls_ssl_context* s,const unsigned char* bytes,size_t n){
 if(tlsWriteWant && s->writes++==0){s->pending=bytes;s->length=n;return MBEDTLS_ERR_SSL_WANT_WRITE;}
 if(s->pending){assert(s->pending==bytes && s->length==n);s->pending=nullptr;}
 return s->send(s->bio,bytes,n);
}
struct ErrorHandle{int error=0,code=0,flags=0;};using esp_tls_error_handle_t=ErrorHandle*;
struct esp_tls_t{ErrorHandle error;};
struct esp_tls_cfg_t{bool non_block=false,is_plain_tcp=false;int timeout_ms=0;const char* common_name=nullptr;
 const unsigned char* cacert_buf=nullptr;size_t cacert_bytes=0;int(*crt_bundle_attach)(void*)=nullptr;};
static int directOutcome=1,directError=ESP_ERR_ESP_TLS_FAILED_CONNECT_TO_HOST,directCode=0,directFlags=0;
static unsigned directCalls=0,directCreates=0,directDestroys=0,directWrites=0;
static std::string directHost,directName,directCa;
static std::vector<int> directWriteResults;
static esp_tls_t* esp_tls_init(){++directCreates;return new esp_tls_t;}
static void esp_tls_conn_destroy(esp_tls_t* p){++directDestroys;delete p;}
static int esp_tls_conn_new_async(const char* host,size_t,int,const esp_tls_cfg_t* cfg,esp_tls_t* t){
 ++directCalls;assert(cfg->non_block && cfg->timeout_ms==10);
 directHost=host;directName=cfg->common_name;directCa=cfg->cacert_buf?reinterpret_cast<const char*>(cfg->cacert_buf):"";
 if(!cfg->is_plain_tcp)assert(cfg->cacert_buf || cfg->crt_bundle_attach);
 t->error={directError,directCode,directFlags};return directOutcome;
}
static int esp_tls_get_error_handle(esp_tls_t* t,esp_tls_error_handle_t* out){*out=&t->error;return 0;}
static int esp_tls_get_and_clear_last_error(esp_tls_error_handle_t e,int* code,int* flags){*code=e->code;*flags=e->flags;return e->error;}
static int esp_tls_get_conn_sockfd(esp_tls_t*,int* out){*out=88;return 0;}
static int esp_tls_conn_read(esp_tls_t*,void*,size_t){return MBEDTLS_ERR_SSL_WANT_READ;}
static int esp_tls_conn_write(esp_tls_t*,const void*,size_t n){++directWrites;if(directWriteResults.empty())return int(n);const int result=directWriteResults.front();directWriteResults.erase(directWriteResults.begin());return result;}
using esp_event_base_t=int;
constexpr int WIFI_EVENT=1,IP_EVENT=2,WIFI_EVENT_STA_CONNECTED=1,WIFI_EVENT_STA_DISCONNECTED=2,
 IP_EVENT_STA_GOT_IP=3,IP_EVENT_STA_LOST_IP=4,ESP_EVENT_ANY_ID=-1;
using esp_event_handler_instance_t=void*;
static int esp_event_handler_instance_register(int,int,void(*)(void*,int,int32_t,void*),void*,void** out){*out=reinterpret_cast<void*>(1);return 0;}
static int esp_event_handler_instance_unregister(int,int,void*){return 0;}
struct ip_addr_t{char text[48]{};};
static int ipaddr_aton(const char* text,ip_addr_t* out){if(text[0]>='0'&&text[0]<='9'){strlcpy(out->text,text,sizeof(out->text));return 1;}return 0;}
static char* ipaddr_ntoa_r(const ip_addr_t* p,char* out,size_t n){strlcpy(out,p->text,n);return out;}
using err_t=int;constexpr int ERR_OK=0,ERR_INPROGRESS=-1,LWIP_DNS_ADDRTYPE_IPV4_IPV6=0;
#define LWIP_DNS_ADDRTYPE_IPV4 1
struct DnsPending{void(*fn)(const char*,const ip_addr_t*,void*);void* ctx;};
static bool dnsDelay=false,dnsFailure=false;
static std::vector<DnsPending> pendingDns;
static int tcpip_try_callback(void(*fn)(void*),void* p){fn(p);return 0;}
static int dns_gethostbyname_addrtype(const char*,ip_addr_t* out,void(*fn)(const char*,const ip_addr_t*,void*),void* p,uint8_t){
 if(dnsDelay){pendingDns.push_back({fn,p});return ERR_INPROGRESS;}
 if(dnsFailure)return -2;strlcpy(out->text,"192.0.2.10",sizeof(out->text));return 0;
}
struct PhonePipeStream{uint32_t generation=1;uint16_t id=0;};
struct Pipe{std::string input,output;bool eof=false;};
static std::map<uint16_t,Pipe> pipes;
static bool phoneReady=true,phoneOpenResult=true;
static unsigned opens=0,closes=0,releases=0,nextStream=0;
static bool phonePipeReady(){return phoneReady;}
static bool phonePipeOpen(PhonePipeStream& h,const char*,uint16_t,uint32_t duration,const std::atomic<bool>& cancel,const std::atomic<bool>* interrupted){
 assert(interrupted);assert(duration<=3000);++opens;if(!phoneOpenResult || cancel.load() || interrupted->load() || !duration)return false;
 h.id=++nextStream;pipes[h.id]={};return true;
}
static int phonePipeRead(PhonePipeStream h,uint8_t* out,size_t n){auto& p=pipes.at(h.id);
 n=std::min(n,p.input.size());std::memcpy(out,p.input.data(),n);p.input.erase(0,n);return n?int(n):(p.eof?-2:0);}
static int phonePipeWrite(PhonePipeStream h,const uint8_t* p,size_t n){pipes.at(h.id).output.append(reinterpret_cast<const char*>(p),n);return int(n);}
static void phonePipeClose(PhonePipeStream h,bool){assert(pipes.count(h.id));++closes;}
static void phonePipeRelease(PhonePipeStream h){assert(pipes.erase(h.id));++releases;}
struct Transport;
using esp_transport_handle_t=Transport*;
struct Transport{void* data=nullptr;int(*connect)(Transport*,const char*,int,int)=nullptr;
 int(*read)(Transport*,char*,int,int)=nullptr;int(*write)(Transport*,const char*,int,int)=nullptr;
 int(*close)(Transport*)=nullptr;int(*pollRead)(Transport*,int)=nullptr;int(*pollWrite)(Transport*,int)=nullptr;int(*destroy)(Transport*)=nullptr;};
static Transport* esp_transport_init(){return new Transport;}
static void* esp_transport_get_context_data(Transport* t){return t->data;}
static int esp_transport_set_context_data(Transport* t,void* p){t->data=p;return 0;}
static int esp_transport_set_default_port(Transport*,int){return 0;}
static int esp_transport_set_func(Transport* t,int(*connect)(Transport*,const char*,int,int),int(*read)(Transport*,char*,int,int),int(*write)(Transport*,const char*,int,int),int(*close)(Transport*),int(*pr)(Transport*,int),int(*pw)(Transport*,int),int(*destroy)(Transport*)){
 t->connect=connect;t->read=read;t->write=write;t->close=close;t->pollRead=pr;t->pollWrite=pw;t->destroy=destroy;return 0;}
static void esp_transport_destroy(Transport* t){t->destroy(t);delete t;}
struct esp_http_client_config_t {Transport* transport=nullptr;const char* cert_pem=nullptr;int(*crt_bundle_attach)(void*)=nullptr;};
struct HttpClient;using esp_http_client_handle_t=HttpClient*;
namespace Clock {
static bool valid=true;
static long epochSeconds(){return 0;}
static bool isPlausibleEpoch(long){return valid;}
}
// INSERT_PRODUCTION
static std::atomic<bool> cancelFlag{false};
static LlmHttpIo* make(bool secure=true,const char* ca="fixture CA",uint32_t total=15000){
 auto* io=llmHttpIoCreate(ca,secure,cancelFlag,now,total,1);assert(io);
 esp_http_client_config_t cfg{};assert(llmHttpIoConfigure(io,cfg) && cfg.transport==io->transport);return io;
}
static int dial(LlmHttpIo* io){return io->transport->connect(io->transport,"relay.example",443,2000);}
static void release(LlmHttpIo* io){llmHttpIoDestroy(io);assert(allocations==0);assert(pipes.empty());}
static void reset(){
 assert(!allocations && pipes.empty() && pendingDns.empty());
 Clock::valid=true;now=100;delayHook={};WiFi.up=true;phoneReady=phoneOpenResult=true;cancelFlag=false;
 tlsHandshake=tlsFlags=parseResult=0;tlsWriteWant=false;handshakes=0;
 directOutcome=1;directError=ESP_ERR_ESP_TLS_FAILED_CONNECT_TO_HOST;directCode=directFlags=0;
 directCalls=directCreates=directDestroys=directWrites=opens=closes=releases=0;directWriteResults.clear();
 dnsDelay=dnsFailure=false;health={};healthEpoch=healthRevision=0;++wifiEpoch;
}
int main(){
 for(bool wifi:{false,true}) {reset();WiFi.up=wifi;Clock::valid=false;
  auto* io=llmHttpIoCreate("fixture CA",true,cancelFlag,now,15000,1);assert(io);
  esp_http_client_config_t cfg{};assert(!llmHttpIoConfigure(io,cfg));
  assert(!opens && !directCreates && !io->attempt.mayHaveSent);release(io);
 }
 {reset();Clock::valid=false;auto* io=make(false);assert(!dial(io) && directCalls==1 && !opens);release(io);}

 {reset();WiFi.up=false;auto* io=make();assert(dial(io)==0 && opens==1 && directCalls==0);
  assert(io->attempt.path==Path::Phone && !io->attempt.mayHaveSent);
  assert(io->tls->ssl.host=="relay.example" && io->tls->config.auth==MBEDTLS_SSL_VERIFY_REQUIRED);
  tlsWriteWant=true;const char* msg="POST /";assert(io->transport->write(io->transport,msg,6,2000)==6);
  assert(io->attempt.mayHaveSent && pipes.at(io->pipe.id).output==msg);
  assert(dial(io)<0 && opens==1);char out[5];
  assert(io->transport->read(io->transport,out,5,20)==0); // empty is timeout, not EOF
  pipes.at(io->pipe.id).input="abc";assert(io->transport->read(io->transport,out,5,20)==3);
  pipes.at(io->pipe.id).eof=true;assert(io->transport->read(io->transport,out,5,20)==-1);
  io->transport->close(io->transport);io->transport->close(io->transport);release(io);assert(releases==1);
 }
 {reset();auto* io=make();assert(dial(io)==0 && directCalls==1 && !opens);
  assert(directHost=="192.0.2.10" && directName=="relay.example" && directCa=="fixture CA");
  assert(!io->attempt.mayHaveSent);cancelFlag=true;
  assert(io->transport->write(io->transport,"H",1,2000)<0 && io->attempt.mayHaveSent && directWrites==0);
  assert(dial(io)<0 && !opens);release(io);
 }
 {reset();directOutcome=-1;auto* io=make();assert(dial(io)==0 && opens==1 && directDestroys==1);release(io);
  io=make();assert(dial(io)==0 && directCalls==1);release(io); // cooldown chooses ready phone
  now+=30000;directOutcome=1;io=make();assert(dial(io)==0 && directCalls==2 && io->attempt.path==Path::Wifi);release(io);
 }
 for(int fault=0;fault<4;++fault){reset();directOutcome=-1;
  if(fault==0)directFlags=1;
  if(fault==1)directError=ESP_ERR_MBEDTLS_X509_CRT_PARSE_FAILED;
  if(fault==2)directError=999; // unclassified TLS protocol error
  if(fault==3)directError=ESP_ERR_NO_MEM;
  auto* io=make();assert(dial(io)<0 && !opens && !io->attempt.mayHaveSent);release(io);
 }
 {reset();directOutcome=0;auto* io=make();uint32_t start=now;assert(dial(io)==0 && now-start==2000 && opens==1);release(io);}
 {reset();dnsDelay=true;auto* io=make();uint32_t start=now;assert(dial(io)==0 && now-start==2000 && !directCalls);release(io);
  assert(pendingDns.size()==1);ip_addr_t answer;strlcpy(answer.text,"192.0.2.11",sizeof(answer.text));
  pendingDns[0].fn(nullptr,&answer,pendingDns[0].ctx);pendingDns.clear();
  for(const auto& slot:dnsSlots)assert(slot.state==DnsState::Free); // late callback did not touch destroyed request
 }
 {reset();dnsDelay=true;auto* io=make();delayHook=[&]{llmHttpIoCancel(io);};assert(dial(io)<0 && !opens && now<2000);delayHook={};release(io);
  pendingDns[0].fn(nullptr,nullptr,pendingDns[0].ctx);pendingDns.clear();
 }
 {reset();WiFi.up=false;tlsHandshake=MBEDTLS_ERR_SSL_WANT_READ;auto* io=make();delayHook=[&]{llmHttpIoCancel(io);};assert(dial(io)<0 && now<2000);delayHook={};release(io);}
 {reset();WiFi.up=false;tlsFlags=1;auto* io=make();assert(dial(io)<0 && io->failure==Failure::Trust);release(io);}
 {reset();WiFi.up=false;parseResult=-1;auto* io=make();assert(dial(io)<0 && !handshakes);release(io);}
 {reset();WiFi.up=false;auto* io=make(true,"");assert(dial(io)==0 && io->tls->config.bundle && !io->tls->config.ca);release(io);}
 {reset();WiFi.up=false;auto* io=make(false);assert(dial(io)<0 && !opens);release(io);}
 {reset();WiFi.up=false;auto* first=make();auto* second=make();assert(dial(first)==0 && dial(second)==0 && first->pipe.id!=second->pipe.id);
  auto id=second->pipe.id;llmHttpIoCancel(first);llmHttpIoDestroy(first);assert(pipes.size()==1 && pipes.count(id));
  assert(second->transport->write(second->transport,"body",4,2000)==4);release(second);
 }
 puts("HTTP transport: Wi-Fi first, strict failure routing, pre-header replay latch, DNS retirement, cancellation, TLS BIO waits and concurrent streams passed");
}
