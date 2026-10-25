#!/usr/bin/env python3
"""Run production direct connect against the pinned ESP-TLS state machine.

Only DNS, sockets and the cryptographic handshake are controlled boundaries.
select clears its fd_sets on timeout, exactly as the target socket API does.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile
from test_web_batch_handlers import extract_block

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]

SDK_BOUNDARIES = r'''
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_INT_EVENT_TRACKER_CAPTURE(h,type,value) ((h)->error=(value))
constexpr int ESP_ERR_ESP_TLS_SOCKET_SETOPT_FAILED=1001;
static unsigned sdkHandshakes=0;
static void _esp_tls_net_init(esp_tls_t*){}
static int tcp_connect(const char*,int,int,const esp_tls_cfg_t* cfg,ErrorHandle*,int* fd){
 assert(cfg->non_block);*fd=88;return ESP_OK;
}
static int tcp_read(esp_tls_t*,void*,size_t){return MBEDTLS_ERR_SSL_WANT_READ;}
static int tcp_write(esp_tls_t*,const void*,size_t n){return int(n);}
#define _esp_tls_read tcp_read
#define _esp_tls_write tcp_write
static void ms_to_timeval(int ms,timeval* tv){tv->tv_sec=ms/1000;tv->tv_usec=(ms%1000)*1000;}
static int create_ssl_handle(const char*,int,const esp_tls_cfg_t* cfg,esp_tls_t*){
 assert(cfg->cacert_buf || cfg->crt_bundle_attach);return ESP_OK;
}
static int esp_tls_handshake(esp_tls_t* t,const esp_tls_cfg_t*){
 ++sdkHandshakes;t->conn_state=ESP_TLS_DONE;return 1;
}
// INSERT_STATE_MACHINE
static int esp_tls_conn_new_async(const char* h,size_t n,int port,const esp_tls_cfg_t* cfg,esp_tls_t* t){
 ++directCalls;return esp_tls_low_level_conn(h,int(n),port,cfg,t);
}
'''

CASES = r'''
int main(){
 for(bool secure:{true,false}){
  reset();phoneReady=false;tcpReadyAt=now+120;sdkHandshakes=0;
  auto* io=make(secure);const auto start=now;
  assert(dial(io)==0 && now-start>=120 && now-start<2000);
  assert(io->connected && !opens && !io->attempt.mayHaveSent);
  assert(sdkHandshakes==unsigned(secure));assert(selectCalls>2);release(io);
 }
 for(bool secure:{true,false}){
  reset();phoneReady=false;tcpReadyAt=now+3000;
  auto* io=make(secure);const auto start=now;
  assert(dial(io)<0 && now-start==2000 && io->failure==Failure::Network);
  assert(!opens && !io->connected && !io->attempt.mayHaveSent);release(io);
 }
 {reset();tcpReadyAt=now+3000;auto* io=make();const auto start=now;
  assert(dial(io)==0 && now-start==2000 && opens==1 && io->attempt.path==Path::Phone);release(io);}
 {reset();phoneReady=false;tcpReadyAt=now+120;socketError=ECONNREFUSED;auto* io=make();
  assert(dial(io)<0 && io->failure==Failure::Network && !io->connected);release(io);}
 {reset();tcpReadyAt=now+500;auto* io=make();const auto start=now;
  delayHook=[&]{if(now-start>=50)llmHttpIoCancel(io);};
  assert(dial(io)<0 && now-start==50 && !opens && io->failure==Failure::Cancelled);
  delayHook={};release(io);}
 {reset();tcpReadyAt=now+500;auto* io=make(true,"fixture CA",55);const auto start=now;
  assert(dial(io)<0 && now-start==55 && !opens);release(io);}
 {reset();phoneReady=false;tcpReadyAt=now+120;auto* io=make();bool injected=false;
  delayHook=[&]{if(!injected){selectErrors=1;selectErrno=EINTR;injected=true;}};
  assert(dial(io)==0 && injected);delayHook={};release(io);}
 {reset();phoneReady=false;tcpReadyAt=now+120;auto* io=make();bool injected=false;
  delayHook=[&]{if(!injected){selectErrors=1;selectErrno=EBADF;injected=true;}};
  assert(dial(io)<0 && injected && io->failure==Failure::Network);delayHook={};release(io);}
 for(int exhausted:{ENOMEM,ENOBUFS}) {
  reset();tcpReadyAt=now+120;socketError=exhausted;auto* io=make();
  assert(dial(io)<0 && io->failure==Failure::Resources && !opens);release(io);
 }
 puts("Pinned ESP-TLS: delayed HTTPS/plain TCP, cleared select sets, deadlines, cancellation, socket errors and pre-HTTP fallback passed");
}
'''

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--idf', type=Path, required=True)
    p.add_argument('--source-root', type=Path, default=ROOT)
    p.add_argument('--cxx', default=os.environ.get('CXX', 'clang++'))
    p.add_argument('--sanitize', action='store_true')
    args = p.parse_args()
    sdk = (args.idf/'components/esp-tls/esp_tls.c').read_text()
    state_machine = extract_block(sdk, 'static int esp_tls_low_level_conn(')
    boundaries = SDK_BOUNDARIES.replace('// INSERT_STATE_MACHINE', state_machine)
    adapter = re.sub(r'^#include.*\n', '', (args.source_root/'System_LLMHttpTransport.cpp').read_text(), flags=re.M)
    base = (HERE/'llm_http_transport_harness.cpp').read_text().split('int main(){', 1)[0]
    unit = '#define HW1_TEST_REAL_TLS_CONNECT 1\n' + base.replace('// INSERT_PINNED_ESP_TLS', boundaries).replace('// INSERT_PRODUCTION', adapter) + CASES
    with tempfile.TemporaryDirectory(prefix='hw1-direct-connect-') as tmp:
        cpp, binary = Path(tmp)/'connect.cpp', Path(tmp)/'connect'
        cpp.write_text(unit)
        command = [args.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                   '-Wno-unused-function', '-Wno-unused-parameter', '-I', str(ROOT), str(cpp), '-o', str(binary)]
        if args.sanitize:
            command[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g']
        subprocess.run(command, check=True)
        subprocess.run([str(binary)], check=True)

if __name__ == '__main__':
    main()
