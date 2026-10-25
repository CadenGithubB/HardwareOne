#include "System_LLMHttpTransportCore.h"
// The DNS name table is PSRAM on the device; here it is plain storage.
#ifndef EXT_RAM_BSS_ATTR
#define EXT_RAM_BSS_ATTR
#endif
#include "BLE_PhonePipeTypes.h"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <functional>
#include <map>
#include <new>
#include <string>
#include <vector>
#include <sys/select.h>
static std::vector<std::string> transportLogs;
static void testLog(const char*,const char* format,...) __attribute__((format(printf,2,3)));
static void testLog(const char*,const char* format,...) {
 char line[512];va_list args;va_start(args,format);vsnprintf(line,sizeof(line),format,args);va_end(args);
 transportLogs.emplace_back(line);
}
#define ESP_LOGW testLog
static std::vector<std::string> pathLogs;
static void pathLog(const char*,const char* format,...) __attribute__((format(printf,2,3)));
static void pathLog(const char*,const char* format,...) {
 char line[512];va_list args;va_start(args,format);vsnprintf(line,sizeof(line),format,args);va_end(args);
 pathLogs.emplace_back(line);
}
#define ESP_LOGI pathLog
constexpr unsigned MALLOC_CAP_INTERNAL=1,MALLOC_CAP_8BIT=2,MALLOC_CAP_SPIRAM=4;
static size_t heap_caps_get_free_size(unsigned){return 123456;}
static size_t heap_caps_get_largest_free_block(unsigned){return 65536;}
#ifdef NDEBUG
#error "Host checks require assertions"
#endif
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
enum class AllocPref{PreferPSRAM,RequireInternal};
static bool refuseInternal=false;
static unsigned internalAllocations=0;
static unsigned allocations=0;
static void* ps_alloc(size_t n,AllocPref pref,const char*){
 if(pref==AllocPref::RequireInternal){if(refuseInternal)return nullptr;++internalAllocations;}
 ++allocations;return std::malloc(n);
}
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
 MBEDTLS_SSL_VERIFY_REQUIRED=2,MBEDTLS_SSL_RENEGOTIATION_ENABLED=1;
#define MBEDTLS_SSL_SESSION_TICKETS_DISABLED 0
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
static uint32_t setupDelay=0,handshakeDelay=0,openWakeDelay=0;
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
static void mbedtls_ssl_conf_session_tickets(mbedtls_ssl_config*,int){assert(false && "Keep SDK ticket defaults");}
static int mbedtls_x509_crt_parse(mbedtls_x509_crt*,const unsigned char* ca,size_t n){assert(ca[n-1]==0);return parseResult;}
static void mbedtls_ssl_conf_ca_chain(mbedtls_ssl_config* c,mbedtls_x509_crt*,void*){c->ca=true;}
static int esp_crt_bundle_attach(void* config){static_cast<mbedtls_ssl_config*>(config)->bundle=true;return 0;}
static int mbedtls_ssl_setup(mbedtls_ssl_context* s,mbedtls_ssl_config* c){now+=setupDelay;s->config=c;return 0;}
static int mbedtls_ssl_set_hostname(mbedtls_ssl_context* s,const char* h){s->host=h;return 0;}
static void mbedtls_ssl_set_bio(mbedtls_ssl_context* s,void* ctx,int(*send)(void*,const unsigned char*,size_t),int(*recv)(void*,unsigned char*,size_t),void*){s->bio=ctx;s->send=send;s->recv=recv;}
static int mbedtls_ssl_handshake(mbedtls_ssl_context* s){++handshakes;now+=handshakeDelay;assert(s->config->auth==2 && (s->config->ca!=s->config->bundle));return tlsHandshake;}
static uint32_t mbedtls_ssl_get_verify_result(mbedtls_ssl_context*){return uint32_t(tlsFlags);}
static int mbedtls_ssl_read(mbedtls_ssl_context* s,unsigned char* out,size_t n){return s->recv(s->bio,out,n);}
static int mbedtls_ssl_write(mbedtls_ssl_context* s,const unsigned char* bytes,size_t n){
 if(tlsWriteWant && s->writes++==0){s->pending=bytes;s->length=n;return MBEDTLS_ERR_SSL_WANT_WRITE;}
 if(s->pending){assert(s->pending==bytes && s->length==n);s->pending=nullptr;}
 return s->send(s->bio,bytes,n);
}
struct ErrorHandle{int error=0,code=0,flags=0;};using esp_tls_error_handle_t=ErrorHandle*;
enum esp_tls_conn_state_t{ESP_TLS_INIT,ESP_TLS_CONNECTING,ESP_TLS_HANDSHAKE,ESP_TLS_FAIL,ESP_TLS_DONE};
struct esp_tls_t{ErrorHandle error;ErrorHandle* error_handle=&error;esp_tls_conn_state_t conn_state=ESP_TLS_INIT;
 bool is_tls=false;int sockfd=-1;fd_set rset{},wset{};
 int(*read)(esp_tls_t*,void*,size_t)=nullptr;int(*write)(esp_tls_t*,const void*,size_t)=nullptr;};
static int esp_tls_get_conn_state(esp_tls_t* t,esp_tls_conn_state_t* out){*out=t->conn_state;return ESP_OK;}
static uint32_t tcpReadyAt=0;
static int socketError=0,selectErrors=0,selectErrno=EINTR;
static unsigned selectCalls=0;
constexpr int SOL_SOCKET=1,SO_ERROR=2;
using socklen_t=unsigned;
static int getsockopt(int,int,int,void* value,socklen_t*){*static_cast<int*>(value)=socketError;return 0;}
int select(int,fd_set* reads,fd_set* writes,fd_set*,timeval* tv){
 ++selectCalls;
 if(selectErrors){--selectErrors;errno=selectErrno;return -1;}
 const bool requested=FD_ISSET(88,reads)||FD_ISSET(88,writes);
 if(requested && now>=tcpReadyAt)return 1;
 now+=unsigned(tv->tv_sec*1000+tv->tv_usec/1000);
 if(delayHook)delayHook();
 FD_ZERO(reads);FD_ZERO(writes);return 0;
}
struct esp_tls_cfg_t{bool non_block=false,is_plain_tcp=false;int timeout_ms=0;const char* common_name=nullptr;
 const unsigned char* cacert_buf=nullptr;size_t cacert_bytes=0;int(*crt_bundle_attach)(void*)=nullptr;};
static int directOutcome=1,directError=ESP_ERR_ESP_TLS_FAILED_CONNECT_TO_HOST,directCode=0,directFlags=0;
static unsigned directCalls=0,directCreates=0,directDestroys=0,directWrites=0;
static std::string directHost,directName,directCa;
static std::vector<int> directWriteResults;
static esp_tls_t* esp_tls_init(){++directCreates;return new esp_tls_t;}
static void esp_tls_conn_destroy(esp_tls_t* p){++directDestroys;delete p;}
#ifndef HW1_TEST_REAL_TLS_CONNECT
static int esp_tls_conn_new_async(const char* host,size_t,int,const esp_tls_cfg_t* cfg,esp_tls_t* t){
 ++directCalls;assert(cfg->non_block && cfg->timeout_ms==10);
 directHost=host;directName=cfg->common_name;directCa=cfg->cacert_buf?reinterpret_cast<const char*>(cfg->cacert_buf):"";
 if(!cfg->is_plain_tcp)assert(cfg->cacert_buf || cfg->crt_bundle_attach);
 t->conn_state=ESP_TLS_HANDSHAKE;t->error={directError,directCode,directFlags};return directOutcome;
}
#else
// INSERT_PINNED_ESP_TLS
#endif
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
struct Pipe{std::string input,output;bool eof=false;};
static std::map<uint16_t,Pipe> pipes;
static bool phoneReady=true, phoneRelay=true, delayRetirement=false, failAfterOpen=false;
static unsigned opens=0,closes=0,releases=0,nextStream=0;
// Fake pipe owner. phoneOpen chooses the OPEN outcome; statuses after commit
// allocate a stream the caller must release, as the real owner does.
static PhonePipeOpenStatus phoneOpen=PhonePipeOpenStatus::Opened;
static uint16_t phoneOpenCode=0;
static uint32_t phoneOpenCost=0,phoneGeneration=1,lastOpenBudget=0;
static bool newSessionDuringOpen=false;
static bool phonePipeReady(){return phoneReady;}
// Phone link v2: READY and the phone's RELAY_ON; relay users route on this alone.
static bool phonePipeRelayAvailable(){return phoneReady && phoneRelay;}
static uint32_t phonePipeGeneration(){return phoneGeneration;}
static PhonePipeOpenResult phonePipeOpen(PhonePipeStream& h,const char*,uint16_t,uint32_t budget,const std::atomic<bool>& cancel,const std::atomic<bool>* interrupted){
 assert(interrupted);++opens;lastOpenBudget=budget;h={};
 if(cancel.load() || interrupted->load())return {PhonePipeOpenStatus::Cancelled};
 if(budget<kPhonePipeOpenResponseMs)return {PhonePipeOpenStatus::NoBudget};
 if(phoneOpen==PhonePipeOpenStatus::NotReady || phoneOpen==PhonePipeOpenStatus::Rejected ||
    phoneOpen==PhonePipeOpenStatus::NoBudget || phoneOpen==PhonePipeOpenStatus::Cancelled)return {phoneOpen,phoneOpenCode};
 h.generation=phoneGeneration;h.corr=0x0102030405060708ull;
 h.openedAt=millis();now+=openWakeDelay;h.id=++nextStream;pipes[h.id]={};
 if(failAfterOpen)return {PhonePipeOpenStatus::LinkLost};
 now+=phoneOpenCost;if(newSessionDuringOpen)++phoneGeneration;return {phoneOpen,phoneOpenCode};
}
static int phonePipeRead(PhonePipeStream h,uint8_t* out,size_t n){auto& p=pipes.at(h.id);
 n=std::min(n,p.input.size());std::memcpy(out,p.input.data(),n);p.input.erase(0,n);return n?int(n):(p.eof?-2:0);}
static int phonePipeWrite(PhonePipeStream h,const uint8_t* p,size_t n){pipes.at(h.id).output.append(reinterpret_cast<const char*>(p),n);return int(n);}
static void phonePipeClose(PhonePipeStream h,bool){assert(pipes.count(h.id));++closes;}
static void phonePipeRelease(PhonePipeStream h){if(!delayRetirement)assert(pipes.erase(h.id));++releases;}
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
enum class LlmHttpRequestKind : uint8_t { Chat, Terminal };
enum class LlmHttpAdmission : uint8_t { Accepted, Busy, Unavailable };
bool llmHttpPathAvailable(bool);
bool phonePipeRetired(PhonePipeStream h){return !h.id || !pipes.count(h.id);}
// INSERT_PRODUCTION
static std::atomic<bool> cancelFlag{false};
static LlmHttpIo* make(bool secure=true,const char* ca="fixture CA",uint32_t total=15000){
 auto* io=llmHttpIoCreate(ca,secure,cancelFlag,now,total,1,LlmHttpRequestKind::Chat);assert(io);
 esp_http_client_config_t cfg{};assert(llmHttpIoConfigure(io,cfg) && cfg.transport==io->transport);return io;
}
static int dial(LlmHttpIo* io){return io->transport->connect(io->transport,"relay.example",443,2000);}
static void release(LlmHttpIo* io){llmHttpIoDestroy(io);assert(allocations==0);assert(pipes.empty());}
// Mark the direct path unreachable in the context the next request will use.
static void directNegative(){health.context(wifiEpoch.load(),1);health.networkFailed(now);}
static void reset(){
 assert(!allocations && pipes.empty() && pendingDns.empty());
 delayRetirement=failAfterOpen=false;phoneOpen=PhonePipeOpenStatus::Opened;phoneOpenCode=0;phoneOpenCost=0;++phoneGeneration;lastOpenBudget=0;pathLogs.clear();
 refuseInternal=false;internalAllocations=0;tcpReadyAt=0;socketError=selectErrors=0;selectErrno=EINTR;selectCalls=0;
 Clock::valid=true;now=100;delayHook={};WiFi.up=true;phoneReady=true;phoneRelay=true;cancelFlag=false;
 setupDelay=handshakeDelay=openWakeDelay=0;tlsHandshake=tlsFlags=parseResult=0;tlsWriteWant=false;handshakes=0;
 directOutcome=1;directError=ESP_ERR_ESP_TLS_FAILED_CONNECT_TO_HOST;directCode=directFlags=0;
 directCalls=directCreates=directDestroys=directWrites=opens=closes=releases=0;directWriteResults.clear();
 dnsDelay=dnsFailure=false;health={};healthEpoch=healthRevision=0;++wifiEpoch;
 phoneHealth={};phoneHealthGeneration=phoneHealthRevision=0;newSessionDuringOpen=false;
 pathStats={};recordSeq=0;for(auto& r:recent)r={};
}
int main(){
 // Deadline begins at OPEN_OK, including delayed worker wake and TLS setup.
 for(uint32_t clock:{uint32_t(100),UINT32_MAX-2000}) {
  reset();now=clock;WiFi.up=false;openWakeDelay=500;setupDelay=1000;
  tlsHandshake=MBEDTLS_ERR_SSL_WANT_READ;auto* io=make();
  assert(dial(io)<0 && io->failure==Failure::Network && uint32_t(now-clock)==4500);
  assert(handshakes>0);release(io);
 }
 {reset();WiFi.up=false;setupDelay=4500;auto* io=make();
  assert(dial(io)<0&&!handshakes&&io->failure==Failure::Network);release(io);}
 {reset();WiFi.up=false;openWakeDelay=4500;auto* io=make();
  assert(dial(io)<0&&!handshakes&&io->failure==Failure::Network);release(io);}
 for(uint32_t delay:{uint32_t(4499),uint32_t(4500)}) {
  reset();WiFi.up=false;handshakeDelay=delay;auto* io=make();
  assert((dial(io)==0)==(delay<4500));release(io);
 }
 for(bool wifi:{false,true}) {reset();WiFi.up=wifi;Clock::valid=false;
  auto* io=llmHttpIoCreate("fixture CA",true,cancelFlag,now,15000,1,LlmHttpRequestKind::Chat);assert(io);
  esp_http_client_config_t cfg{};assert(llmHttpIoConfigure(io,cfg));
  assert(dial(io)==0 && !io->attempt.mayHaveSent);release(io);
 }
 {reset();Clock::valid=false;auto* io=make(false);assert(!dial(io) && directCalls==1 && !opens);release(io);}

 {reset();WiFi.up=false;refuseInternal=true;auto* io=make();assert(dial(io)<0 && io->failure==Failure::Resources && !handshakes);release(io);}
 {reset();WiFi.up=false;auto* io=make();assert(dial(io)==0 && opens==1 && directCalls==0);
  assert(io->attempt.path==Path::Phone && !io->attempt.mayHaveSent);
  assert(internalAllocations==1);
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
  const size_t logStart=transportLogs.size();
  auto* io=make();assert(dial(io)<0 && !opens && !io->attempt.mayHaveSent);
  assert(transportLogs.size()==logStart+2);
  assert(transportLogs[logStart].find("direct TLS error: esp=")==0);
  assert(transportLogs.back().find("internal_free=123456 internal_largest=65536")!=std::string::npos);
  const char* category=fault<2?"category=tls-trust":fault==2?"category=protocol":"category=resources";
  assert(transportLogs.back().find(category)!=std::string::npos);
  for(size_t i=logStart;i<transportLogs.size();++i) {
   assert(transportLogs[i].find("relay.example")==std::string::npos);
   assert(transportLogs[i].find("fixture CA")==std::string::npos);
  }
  release(io);
 }
 {reset();directOutcome=0;auto* io=make();uint32_t start=now;assert(dial(io)==0 && now-start==2000 && opens==1);
  assert(transportLogs.back().find("category=timeout")!=std::string::npos);
  assert(transportLogs.back().find("elapsed_ms=2000 deadline=1")!=std::string::npos);release(io);}
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
 // Reservations are per consumer, survive transport destruction, and gate
 // admission synchronously without any new OPEN. Exercise normal and failed OPEN.
 for(bool failed:{false,true}) {reset();WiFi.up=false;delayRetirement=true;
  assert(llmHttpRequestAdmit(LlmHttpRequestKind::Chat,true)==LlmHttpAdmission::Accepted);
  auto* chat=make();assert(dial(chat)==0);
  assert(llmHttpRequestAdmit(LlmHttpRequestKind::Terminal,true)==LlmHttpAdmission::Accepted);
  auto* terminal=llmHttpIoCreate("fixture CA",true,cancelFlag,now,15000,1,LlmHttpRequestKind::Terminal);
  assert(terminal);failAfterOpen=failed;assert((dial(terminal)==0)==!failed);
  const auto terminalId=requestSlots[1].retiring.id?requestSlots[1].retiring.id:terminal->pipe.id;
  assert(terminalId);llmHttpIoDestroy(terminal);llmHttpRequestRelease(LlmHttpRequestKind::Terminal);
  const auto before=opens;
  assert(llmHttpRequestAdmit(LlmHttpRequestKind::Terminal,true)==LlmHttpAdmission::Busy && opens==before);
  assert(llmHttpRequestAdmit(LlmHttpRequestKind::Chat,true)==LlmHttpAdmission::Busy);
  assert(pipes.erase(terminalId)); // CLOSE_ACK/RESET retirement boundary
  assert(llmHttpRequestAdmit(LlmHttpRequestKind::Terminal,true)==LlmHttpAdmission::Accepted);
  assert(llmHttpRequestAdmit(LlmHttpRequestKind::Terminal,true)==LlmHttpAdmission::Busy); // worker still owns it
  llmHttpRequestRelease(LlmHttpRequestKind::Terminal);delayRetirement=false;
  release(chat);llmHttpRequestRelease(LlmHttpRequestKind::Chat);
 }
 using Status=PhonePipeOpenStatus;
 const auto npos=std::string::npos;
 // F5: only the direct hint is negative, so the phone goes first. An OPEN_ERR
 // in the network class lets the request use Wi-Fi once despite its hint.
 // 16 SERVICE_OFF: the phone's relay is off or has no destination (phone link v2).
 for(uint16_t code:{5,6,7,8,11,16}) {reset();directNegative();phoneOpen=Status::Refused;phoneOpenCode=code;
  auto* io=make();assert(dial(io)==0 && opens==1 && directCalls==1);
  assert(io->attempt.first==Path::Phone && io->attempt.path==Path::Wifi && !io->attempt.mayHaveSent);
  assert(io->phoneOutcome.tried && !io->phoneOutcome.connected && io->phoneOutcome.failure==Failure::Network);
  assert(!phoneHealth.eligible() && health.eligible());release(io);}
 // Busy/resources, policy, cancellation and protocol faults never bypass to
 // Wi-Fi and never mark the phone unreachable.
 for(uint16_t code:{1,4,9,10,14,17,18}) {reset();directNegative();phoneOpen=Status::Refused;phoneOpenCode=code;
  auto* io=make();assert(dial(io)<0 && opens==1 && !directCalls);
  assert(io->failure==hw1httptransport::openRefusal(code) && phoneHealth.eligible());release(io);}
 // Stale READY or silent loss: the bounded OPEN phases end, then Wi-Fi once.
 for(auto status:{Status::SubmitTimeout,Status::ResponseTimeout,Status::LinkLost,Status::NotReady}) {
  reset();directNegative();phoneOpen=status;phoneOpenCost=status==Status::NotReady?0:6000;auto* io=make();
  assert(dial(io)==0 && io->attempt.first==Path::Phone && io->attempt.path==Path::Wifi);
  assert(!phoneHealth.eligible());release(io);}
 // Each path at most once: no Wi-Fi -> phone -> Wi-Fi, and no reverse loop.
 {reset();directOutcome=-1;phoneOpen=Status::ResponseTimeout;auto* io=make();
  assert(dial(io)<0 && directCalls==1 && opens==1 && io->attempt.first==Path::Wifi);release(io);}
 {reset();directNegative();directOutcome=-1;phoneOpen=Status::Refused;phoneOpenCode=7;auto* io=make();
  assert(dial(io)<0 && directCalls==1 && opens==1 && io->attempt.first==Path::Phone);release(io);}
 // No fallback once an HTTP byte may have been written.
 {reset();directNegative();auto* io=make();assert(dial(io)==0 && io->attempt.path==Path::Phone);
  assert(io->transport->write(io->transport,"POST /",6,2000)==6);
  assert(!io->attempt.fallback(Failure::Network,Path::Wifi,true));release(io);}
 // The phone hint is a preference, not a ban: with Wi-Fi down it is the only
 // path, and a completed phone handshake clears it.
 {reset();WiFi.up=false;phoneHealth.context(phoneGeneration,1);phoneHealth.networkFailed(now);
  auto* io=make();assert(dial(io)==0 && io->attempt.path==Path::Phone && phoneHealth.eligible());release(io);}
 // Both hints negative: Wi-Fi first, and a negative phone still gets the
 // request after Wi-Fi fails before any write.
 {reset();phoneHealth.context(phoneGeneration,1);phoneHealth.networkFailed(now);directNegative();directOutcome=-1;
  auto* io=make();assert(dial(io)==0 && io->attempt.first==Path::Wifi && io->attempt.path==Path::Phone);release(io);}
 // One phone probe after 30 s while the direct hint is still fresh.
 for(uint32_t wait:{uint32_t(29999),uint32_t(30000)}) {
  reset();phoneHealth.context(phoneGeneration,1);phoneHealth.networkFailed(now);now+=wait;directNegative();
  auto* io=make();assert(dial(io)==0);
  assert(io->attempt.first==(wait<30000?Path::Wifi:Path::Phone));release(io);}
 // Scope: a new phone session starts clean, and a result from a session that
 // ended during the attempt cannot mark its replacement.
 {reset();phoneHealth.context(phoneGeneration,1);phoneHealth.networkFailed(now);++phoneGeneration;directNegative();
  auto* io=make();assert(dial(io)==0 && io->attempt.first==Path::Phone);release(io);}
 {reset();directNegative();phoneOpen=Status::ResponseTimeout;newSessionDuringOpen=true;auto* io=make();
  assert(dial(io)==0 && io->attempt.path==Path::Wifi && phoneHealth.eligible());release(io);}
 // A-1: the OPEN gets the caller's whole remaining budget, and is not started
 // when less than the 4 s answer window remains.
 {reset();WiFi.up=false;auto* io=make();assert(dial(io)==0 && lastOpenBudget==15000);release(io);}
 {reset();WiFi.up=false;auto* io=make(true,"fixture CA",3999);
  assert(dial(io)<0 && io->failure==Failure::Cancelled && io->phoneOpen.status==Status::NoBudget && pipes.empty());release(io);}
 // F3: one record per request, with the path that carried HTTP bytes and the
 // worker's result. No host, port, credential or content.
 {reset();directOutcome=-1;auto* io=make();assert(dial(io)==0);
  assert(io->transport->write(io->transport,"POST /",6,2000)==6);llmHttpIoFinish(io,true);
  const auto stream=io->pipe.id;assert(stream);
  io->transport->close(io->transport);  // the workers close the client before destroy
  release(io);
  assert(pathLogs.size()==1);
  assert(pathLogs[0].find("#1 chat first=wifi carried=phone wifi=network phone=ok open=opened:0 outcome=completed corr=0102030405060708 stream="+std::to_string(stream)+" ")==0);
  for(const char* secret:{"relay.example","fixture CA","POST","443"})assert(pathLogs[0].find(secret)==npos);
  char report[2048];assert(llmHttpPathReport(report,sizeof(report))==strlen(report));
  assert(strstr(report,"requests=1 carried wifi=0 phone=1 none=0 fallback wifi>phone=1 phone>wifi=0\n"));
  assert(strstr(report,"outcomes completed=1 failed_before_send=0 failed_after_send=0 unreported=0\n"));
  assert(strstr(report,"phone_open: opened=1\nopen_err:\nrecent:\n#1 chat first=wifi"));}
 {reset();WiFi.up=false;phoneOpen=Status::Refused;phoneOpenCode=5;auto* io=make();assert(dial(io)<0);
  llmHttpIoFinish(io,false);io->transport->close(io->transport);release(io);
  assert(pathLogs.back().find("first=phone carried=none wifi=- phone=network open=refused:5 outcome=failed-before-send corr=0102030405060708 stream=")!=npos);
  assert(pathLogs.back().find("stream=0 ")==npos);
  char report[2048];llmHttpPathReport(report,sizeof(report));
  assert(strstr(report,"failed_before_send=1") && strstr(report,"phone_open: refused=1") && strstr(report,"open_err: 5=1"));}
 {reset();directNegative();phoneOpen=Status::Refused;phoneOpenCode=6;auto* io=make();assert(dial(io)==0);
  assert(io->transport->write(io->transport,"x",1,2000)==1);llmHttpIoFinish(io,false);release(io);
  assert(pathLogs.back().find("first=phone carried=wifi wifi=ok phone=network open=refused:6 outcome=failed-after-send")!=npos);
  char report[2048];llmHttpPathReport(report,sizeof(report));assert(strstr(report,"phone>wifi=1"));}
 {reset();auto* io=make();assert(dial(io)==0);release(io);
  assert(pathLogs.back().find("carried=none wifi=ok phone=- open=-:0 outcome=unreported corr=0000000000000000 stream=0")!=npos);}
 {reset();auto* io=make();release(io);assert(pathLogs.empty());} // never dialed: no record
 {reset();WiFi.up=false;for(unsigned i=0;i<10;++i){auto* io=make();assert(dial(io)==0);llmHttpIoFinish(io,true);release(io);}
  char report[4096];llmHttpPathReport(report,sizeof(report));
  assert(strstr(report,"requests=10 ") && !strstr(report,"\n#2 ") && strstr(report,"\n#3 ") && strstr(report,"\n#10 "));
  char tiny[24];assert(llmHttpPathReport(tiny,sizeof(tiny))==strlen(tiny) && strlen(tiny)<sizeof(tiny));}
 // The class of every pipe code: 16 is network-class (Wi-Fi once), 17
 // UNSUPPORTED never bypasses to another path.
 for(unsigned code=0;code<=20;++code){const Failure f=hw1httptransport::openRefusal(uint16_t(code));
  assert((f==Failure::Network)==(code==5||code==6||code==7||code==8||code==11||code==16));
  assert((f==Failure::Resources)==(code==9||code==10) && (f==Failure::Cancelled)==(code==14));}
 // relay status counts each refusal under its own code: 16..18 never fold onto
 // 0..2, and a code the pipe does not define counts as 0.
 {reset();WiFi.up=false;
  for(uint16_t code:{16,17,18,2,300}){phoneOpen=Status::Refused;phoneOpenCode=code;auto* io=make();assert(dial(io)<0);
   llmHttpIoFinish(io,false);release(io);}
  char report[2048];llmHttpPathReport(report,sizeof(report));assert(strstr(report,"\nopen_err: 0=1 2=1 16=1 17=1 18=1\n"));}
 // Phone link v2: the phone is a relay path only while READY with RELAY_ON.
 // With relay off the link carries the console only: never chosen, never a
 // fallback, and no path for admission when Wi-Fi is down.
 {reset();WiFi.up=false;phoneRelay=false;
  assert(!llmHttpPathAvailable(true) && llmHttpRequestAdmit(LlmHttpRequestKind::Chat,true)==LlmHttpAdmission::Unavailable);
  auto* io=make();assert(dial(io)<0 && !opens && !directCalls && io->attempt.first==Path::None);release(io);
  phoneRelay=true;assert(llmHttpPathAvailable(true));}
 {reset();directOutcome=-1;phoneRelay=false;auto* io=make();   // Wi-Fi fails before a write: no phone fallback
  assert(dial(io)<0 && directCalls==1 && !opens && io->attempt.first==Path::Wifi && !io->phoneOutcome.tried);release(io);}
 {reset();directNegative();phoneRelay=false;auto* io=make();   // a negative Wi-Fi hint never picks a relay-off phone
  assert(dial(io)==0 && io->attempt.first==Path::Wifi && !opens);release(io);}
 {reset();directNegative();auto* io=make();assert(dial(io)==0 && io->attempt.first==Path::Phone && opens==1);release(io);}  // relay on
 puts("HTTP transport: two-way pre-write fallback by OPEN class, phone hint scope and probes, A-1 budget, F3 records;");
 puts("HTTP transport: relay availability (READY and RELAY_ON) for path choice, fallback and admission; refusal 16 network, 17 protocol, counts per code 0..18;");
 puts("HTTP transport: Wi-Fi first, strict failure routing, pre-header replay latch, DNS retirement, cancellation, TLS BIO waits and concurrent streams passed");
}
