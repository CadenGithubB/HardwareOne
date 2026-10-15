// Header storage/event loop and connect entry are SDK boundaries. The actual
// SDK first-line/header loops/open/write/close bodies are inserted unchanged.
constexpr int ESP_FAIL=-1,ESP_ERR_HTTP_WRITE_DATA=51,ESP_ERR_HTTP_CONNECTING=52,ESP_ERR_HTTP_EAGAIN=53;
constexpr int HTTP_STATE_INIT=1,HTTP_STATE_CONNECTED=2,HTTP_STATE_REQ_COMPLETE_HEADER=3;
constexpr int HTTP_METHOD_GET=0,HTTP_METHOD_HEAD=1,HTTP_METHOD_POST=2,HTTP_METHOD_DELETE=3;
constexpr int HTTP_EVENT_HEADERS_SENT=1,HTTP_EVENT_ERROR=2,HTTP_EVENT_DISCONNECTED=3;
static const char* HTTP_METHOD_MAPPING[]={"GET","HEAD","POST","DELETE"};
static const char* DEFAULT_HTTP_PROTOCOL="HTTP/1.1";
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
struct Buffer {char storage[256]{};char* data=storage;};
struct Headers{Buffer bytes;Buffer* buffer=&bytes;void* headers=nullptr;};
struct HttpClient {
 int post_len=0,timeout_ms=2000,buffer_size_tx=255,state=HTTP_STATE_INIT;
 bool is_async=false,first_line_prepared=false;
 int header_index=0,data_written_index=0,data_write_left=0;
 struct {int method=HTTP_METHOD_POST;const char* path="/fixture";const char* query=nullptr;}connection_info;
 Headers storage;Headers* request=&storage;Transport* transport=nullptr;
};
static void http_header_delete(void*,const char*){}
static void http_header_set_format(void*,const char*,const char*,int){}
static int esp_http_client_set_header(HttpClient*,const char*,const char*){return 0;}
static int http_header_generate_string(void*,int index,char* out,int* len){
 if(index)return 0;
 const char headers[]="Host: relay.example\r\nAuthorization: Bearer fixture-token\r\n\r\n";
 assert(*len>=int(sizeof(headers)-1));std::memcpy(out,headers,sizeof(headers));*len=sizeof(headers)-1;return 1;
}
static void http_dispatch_event(HttpClient*,int,void*,int){}
static void http_dispatch_event_to_event_loop(int,void*,size_t){}
static void* esp_transport_get_error_handle(Transport*){return nullptr;}
static int esp_transport_write(Transport* t,const char* p,int len,int timeout){return t->write(t,p,len,timeout);}
static int esp_transport_close(Transport* t){return t->close(t);}
static int esp_http_client_connect(HttpClient* client){
 int result=client->transport->connect(client->transport,"relay.example",443,2000);
 if(!result)client->state=HTTP_STATE_CONNECTED;
 return result;
}
// INSERT_SDK
int main(){
 for(int partial=0;partial<2;++partial){reset();auto* io=make();HttpClient client;client.transport=io->transport;
  if(partial)directWriteResults={3,-1};else directWriteResults={-1};
  assert(esp_http_client_open(&client,100)!=ESP_OK);
  assert(io->attempt.mayHaveSent && io->attempt.finished && !opens && directCreates==1);
  assert(directWrites==unsigned(partial?2:1));
  assert(esp_http_client_open(&client,100)!=ESP_OK); // no new child/alternate route
  assert(directCreates==1 && !opens);release(io);
 }
 {reset();auto* io=make();HttpClient client;client.transport=io->transport;
  assert(!esp_http_client_open(&client,4));assert(io->attempt.mayHaveSent && directWrites==1);
  directWriteResults={2,-1};assert(esp_http_client_write(&client,"body",4)<0);
  assert(esp_http_client_open(&client,4)!=ESP_OK && directCreates==1 && !opens);release(io);
 }
 {reset();directOutcome=-1;auto* io=make();HttpClient client;client.transport=io->transport;
  assert(!esp_http_client_open(&client,4));
  assert(opens==1 && directCreates==1 && io->attempt.path==Path::Phone && io->attempt.mayHaveSent);
  assert(pipes.at(io->pipe.id).output.find("POST /fixture HTTP/1.1\r\nHost:")==0);
  assert(esp_http_client_write(&client,"body",4)==4);
  assert(opens==1 && directCreates==1);release(io);
 }
 {reset();directOutcome=-1;directFlags=1;auto* io=make();HttpClient client;client.transport=io->transport;
  assert(esp_http_client_open(&client,4)!=ESP_OK && !io->attempt.mayHaveSent && !opens && directWrites==0);release(io);
 }
 puts("Pinned IDF open/header/body writes: partial-header failure latches uncertainty; no connection replay; only pre-HTTP network fallback passed");
}
