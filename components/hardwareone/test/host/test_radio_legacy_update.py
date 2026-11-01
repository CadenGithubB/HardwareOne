#!/usr/bin/env python3
"""Run production Hosted admission and OTA chunking against controlled RPCs."""
from pathlib import Path
import subprocess
import tempfile
from test_web_batch_handlers import extract_block
HERE = Path(__file__).resolve().parent
SOURCE = HERE.parents[1]

def main():
    source = (SOURCE / 'HAL_Radio.cpp').read_text()
    names = ['esp_err_t startOnce(', 'static bool updateTransportUp(',
             'bool radioHalTransportUp(', 'esp_err_t radioHalUpdateWrite(']
    production = '\n'.join(extract_block(source, name) for name in names)
    scaffold = r'''
#include <cassert>
#include <cstdint>
#include <cstddef>
#include <vector>
using esp_err_t = int;
constexpr int ESP_OK=0, ESP_FAIL=-1, ESP_ERR_WIFI_NOT_INIT=2,
  ESP_ERR_INVALID_ARG=3, ESP_ERR_INVALID_STATE=4, WIFI_IF_STA=0;
#define HW1_C6_HOSTED_VERSION_MAJOR 2
#define HW1_C6_HOSTED_VERSION_MINOR 12
#define HW1_C6_HOSTED_VERSION_PATCH 13
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define portENTER_CRITICAL(...) ((void)0)
#define portEXIT_CRITICAL(...) ((void)0)
bool sTransportUp=false, sTransportLost=false, sHeld=false, sUpdateOnly=false;
struct { bool versionKnown=false, versionMatches=false; int major=0,minor=0,patch=0; } sIdentity;
struct esp_hosted_coprocessor_fwver_t { int major1=2,minor1=12,patch1=13; };
bool initialized=true;
int versionResult=ESP_OK, probeResult=ESP_OK, probeCalls=0, writeFailure=0;
int remotePatch=13;
std::vector<unsigned> chunks;
bool hostedInitWiFi() { return initialized; }
bool hostedIsInitialized() { return initialized; }
int esp_hosted_get_coprocessor_fwversion(esp_hosted_coprocessor_fwver_t* v) {
  v->patch1=remotePatch; return versionResult;
}
int esp_wifi_get_mac(int, uint8_t*) { ++probeCalls; return probeResult; }
int esp_hosted_slave_ota_write(uint8_t*, uint32_t size) {
  chunks.push_back(size); return writeFailure;
}
'''
    checks = r'''
int main() {
  assert(startOnce(false)==ESP_OK && radioHalTransportUp());
  assert(sIdentity.versionKnown && sIdentity.versionMatches && probeCalls==0);
  remotePatch=99;
  assert(startOnce(false)==ESP_OK && !sIdentity.versionMatches);
  // Main's normal boot contract: no silent fallback on any version error.
  versionResult=17;
  assert(startOnce(false)==17 && probeCalls==0);
  sTransportUp=false;
  probeResult=18;
  assert(startOnce(true)==18 && !updateTransportUp());
  assert(probeCalls==1);
  for (int reply : {ESP_OK, ESP_ERR_WIFI_NOT_INIT}) {
    probeResult=reply;
    assert(startOnce(true)==ESP_OK);
    assert(updateTransportUp() && !radioHalTransportUp());
    assert(!sIdentity.versionKnown && !sIdentity.versionMatches);
    uint8_t data[2500] = {};
    chunks.clear();
    assert(radioHalUpdateWrite(data,sizeof(data))==ESP_OK);
    assert((chunks==std::vector<unsigned>{1024,1024,452}));
    chunks.clear(); writeFailure=19;
    assert(radioHalUpdateWrite(data,sizeof(data))==19 && chunks.size()==1);
    writeFailure=0;
    sTransportLost=true;
    assert(radioHalUpdateWrite(data,sizeof(data))==ESP_ERR_INVALID_STATE);
    assert(!updateTransportUp() && !radioHalTransportUp());
    sTransportLost=false;
    assert(radioHalUpdateWrite(nullptr,1)==ESP_ERR_INVALID_ARG);
    assert(radioHalUpdateWrite(data,0)==ESP_ERR_INVALID_ARG);
  }
  versionResult=ESP_OK; remotePatch=13;
  assert(startOnce(false)==ESP_OK && radioHalTransportUp() && !sUpdateOnly);
  uint8_t data[4096] = {}; chunks.clear();
  assert(radioHalUpdateWrite(data,sizeof(data))==ESP_OK);
  assert((chunks==std::vector<unsigned>{4096}));
  initialized=false;
  assert(startOnce(true)==ESP_FAIL && !updateTransportUp());
}
'''
    with tempfile.TemporaryDirectory(prefix='hw1-radio-legacy-') as directory:
        path = Path(directory)
        (path/'test.cpp').write_text(scaffold + production + checks)
        subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', str(path/'test.cpp'),
                        '-o',str(path/'test')],check=True)
        subprocess.run([str(path/'test')],check=True)
    print('Hosted legacy update admission and chunking passed')
if __name__ == '__main__': main()
