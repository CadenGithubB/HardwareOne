#include "System_LLMHttpTransportCore.h"
#include "BLE_PhonePipeTypes.h"
#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/pk.h>
#include <psa/crypto.h>
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <iterator>
#include <new>
#include <string>
#ifdef NDEBUG
#error "Host checks require assertions"
#endif
#define ENABLE_BLE_PHONE_PIPE 1
#undef CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#define CONFIG_MBEDTLS_CERTIFICATE_BUNDLE 0
using hw1httptransport::Attempt;using hw1httptransport::Path;using hw1httptransport::Failure;
constexpr int ERR_TCP_TRANSPORT_CONNECTION_FAILED=-2,ERR_TCP_TRANSPORT_CONNECTION_CLOSED_BY_FIN=-1,ERR_TCP_TRANSPORT_CONNECTION_TIMEOUT=0;
// INSERT_LIMITS
struct LlmHttpIo;using esp_transport_handle_t=LlmHttpIo*;
struct esp_tls_t{};
static void esp_tls_conn_destroy(esp_tls_t*){assert(false);}
static int esp_tls_conn_read(esp_tls_t*,void*,size_t){assert(false);return -1;}
static int esp_tls_conn_write(esp_tls_t*,const void*,size_t){assert(false);return -1;}
enum class AllocPref{PreferPSRAM,RequireInternal};
static void* ps_alloc(size_t n,AllocPref pref,const char*){assert(pref==AllocPref::RequireInternal);return std::malloc(n);}
static void ps_free(void* p){std::free(p);}
static uint32_t clockMs=0;
static uint32_t millis(){return clockMs;}
static size_t fragment=8;
static std::deque<unsigned char> toServer,toClient;
static std::string observedWire,receivedHttp;
static bool serverReady=false,serverFailed=false,responseSent=false;
static mbedtls_ssl_context server;
static const char response[]="HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nOK";
static int push(std::deque<unsigned char>& queue,const unsigned char* p,size_t n){
 n=std::min({n,fragment,size_t(4096-queue.size())});if(!n)return MBEDTLS_ERR_SSL_WANT_WRITE;
 queue.insert(queue.end(),p,p+n);observedWire.append(reinterpret_cast<const char*>(p),n);return int(n);
}
static int pop(std::deque<unsigned char>& queue,unsigned char* p,size_t n){
 n=std::min({n,fragment,queue.size()});if(!n)return MBEDTLS_ERR_SSL_WANT_READ;
 for(size_t i=0;i<n;++i){p[i]=queue.front();queue.pop_front();}return int(n);
}
static int serverSend(void*,const unsigned char* p,size_t n){return push(toClient,p,n);}
static int serverRead(void*,unsigned char* p,size_t n){return pop(toServer,p,n);}
static bool want(int result){return result==MBEDTLS_ERR_SSL_WANT_READ || result==MBEDTLS_ERR_SSL_WANT_WRITE;}
static void pause(){
 clockMs+=10;
 if(!serverReady && !serverFailed){int r=mbedtls_ssl_handshake(&server);if(!r)serverReady=true;else if(!want(r))serverFailed=true;}
 if(serverReady){
  unsigned char buffer[256];int r=mbedtls_ssl_read(&server,buffer,sizeof(buffer));
  if(r>0)receivedHttp.append(reinterpret_cast<const char*>(buffer),size_t(r));
  if(!responseSent && receivedHttp.find("\r\n\r\n")!=std::string::npos){
   r=mbedtls_ssl_write(&server,reinterpret_cast<const unsigned char*>(response),sizeof(response)-1);
   if(r>0){assert(size_t(r)==sizeof(response)-1);responseSent=true;}else assert(want(r));
  }
 }
}
static PhonePipeOpenResult phonePipeOpen(PhonePipeStream& h,const char*,uint16_t,uint32_t,const std::atomic<bool>& cancel,const std::atomic<bool>* interrupted){
 if(cancel.load() || interrupted->load())return {PhonePipeOpenStatus::Cancelled};
 h.generation=1;h.id=1;h.openedAt=millis();return {PhonePipeOpenStatus::Opened};}
static int phonePipeRead(PhonePipeStream,unsigned char* p,size_t n){int r=pop(toClient,p,n);return r==MBEDTLS_ERR_SSL_WANT_READ?0:r;}
static int phonePipeWrite(PhonePipeStream,const unsigned char* p,size_t n){int r=push(toServer,p,n);return r==MBEDTLS_ERR_SSL_WANT_WRITE?0:r;}
static void phonePipeClose(PhonePipeStream,bool){}
static void phonePipeRelease(PhonePipeStream){}
enum class LlmHttpRequestKind : uint8_t { Chat, Terminal };
struct RequestSlot {std::atomic<bool> claimed{false};PhonePipeStream retiring;};
RequestSlot requestSlots[2];
// INSERT_TYPES
static LlmHttpIo& context(esp_transport_handle_t io){return *io;}
// INSERT_PRODUCTION
static std::string contents(const char* file){std::ifstream in(file);assert(in);return std::string(std::istreambuf_iterator<char>(in),{});}
int main(int argc,char** argv){
 assert(argc==7);fragment=std::strtoul(argv[6],nullptr,10);const bool expected=std::atoi(argv[5]);
 const auto ca=contents(argv[1]),certificate=contents(argv[2]),key=contents(argv[3]);
 mbedtls_x509_crt cert;mbedtls_x509_crt_init(&cert);
 mbedtls_pk_context pk;mbedtls_pk_init(&pk);
 mbedtls_entropy_context entropy;mbedtls_entropy_init(&entropy);
 mbedtls_ctr_drbg_context rng;mbedtls_ctr_drbg_init(&rng);
 assert(!mbedtls_ctr_drbg_seed(&rng,mbedtls_entropy_func,&entropy,nullptr,0));
 assert(!mbedtls_x509_crt_parse(&cert,reinterpret_cast<const unsigned char*>(certificate.c_str()),certificate.size()+1));
 assert(!mbedtls_pk_parse_key(&pk,reinterpret_cast<const unsigned char*>(key.c_str()),key.size()+1,nullptr,0,mbedtls_ctr_drbg_random,&rng));
 mbedtls_ssl_config config;mbedtls_ssl_config_init(&config);
 assert(!mbedtls_ssl_config_defaults(&config,MBEDTLS_SSL_IS_SERVER,MBEDTLS_SSL_TRANSPORT_STREAM,MBEDTLS_SSL_PRESET_DEFAULT));
 mbedtls_ssl_conf_rng(&config,mbedtls_ctr_drbg_random,&rng);
 mbedtls_ssl_conf_max_tls_version(&config,MBEDTLS_SSL_VERSION_TLS1_2);
 assert(!mbedtls_ssl_conf_own_cert(&config,&cert,&pk));
 mbedtls_ssl_init(&server);assert(!mbedtls_ssl_setup(&server,&config));mbedtls_ssl_set_bio(&server,nullptr,serverSend,serverRead,nullptr);
 LlmHttpIo io;std::atomic<bool> cancel{false};io.ca=ca.c_str();io.cancelled=&cancel;io.total=15000;
 assert(std::strlen(argv[4])<sizeof(io.host));std::strcpy(io.host,argv[4]);
 const bool connected=connectPhone(io,443);
 if(connected!=expected){std::fprintf(stderr,"unexpected certificate acceptance=%d flags=%08x failure=%u time=%u\n",connected,io.tls?mbedtls_ssl_get_verify_result(&io.tls->ssl):0,unsigned(io.failure),clockMs);return 1;}
 const char request[]="POST /fixture HTTP/1.1\r\nAuthorization: Bearer fixture-not-a-real-credential\r\nContent-Length: 0\r\n\r\n";
 if(connected){
  assert(mbedtls_ssl_get_verify_result(&io.tls->ssl)==0);
  assert(write(&io,request,sizeof(request)-1,2000)==int(sizeof(request)-1));
  std::string reply;char bytes[20];
  while(reply.size()<sizeof(response)-1){int n=read(&io,bytes,sizeof(bytes),2000);assert(n>0);reply.append(bytes,size_t(n));}
  assert(reply==response && receivedHttp==request && io.attempt.mayHaveSent);
  assert(observedWire.find("fixture-not-a-real-credential")==std::string::npos);
  assert(observedWire.find("POST /fixture")==std::string::npos);
 }else{assert(receivedHttp.empty() && !io.attempt.mayHaveSent && io.failure==Failure::Trust);}
 closeChild(io);mbedtls_ssl_free(&server);mbedtls_ssl_config_free(&config);mbedtls_x509_crt_free(&cert);
 mbedtls_pk_free(&pk);mbedtls_ctr_drbg_free(&rng);mbedtls_entropy_free(&entropy);
 std::puts(connected?"accepted; HTTP sealed end to end":"rejected before HTTP");
}
