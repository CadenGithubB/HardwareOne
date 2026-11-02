#!/usr/bin/env python3
"""Run the actual SDK allocator/destructor and project descriptor relocation."""
import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path
from test_web_batch_handlers import extract_block

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]

HARNESS = r'''
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <set>
#include <sys/queue.h>
#define ENABLE_CAMERA_SENSOR 1
#define CONFIG_IDF_TARGET_ESP32P4 1
#define CONFIG_CAM_CTLR_MIPI_CSI_ISR_CACHE_SAFE 1
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_SPIRAM 4
#define V4L2_MEMORY_MMAP 1
#define ESP_VIDEO_ALIGN(n,a) (((n)+(a)-1)&~((a)-1))
#define ESP_LOGE(...) do {} while(0)
#define ELEMENT_SET_FREE(e) ((e)->free=true)
using esp_err_t=int;
constexpr int ESP_OK=0;
static std::set<void*> live,internalPointers;
static bool failInternal=false,failAll=false;
void* allocate(size_t bytes,unsigned caps) {
  if(failAll || (failInternal && (caps&MALLOC_CAP_INTERNAL)))return nullptr;
  void* p=std::calloc(1,bytes);assert(p);live.insert(p);
  if(caps&MALLOC_CAP_INTERNAL)internalPointers.insert(p);
  return p;
}
void* heap_caps_calloc(size_t n,size_t bytes,unsigned caps){return allocate(n*bytes,caps);}
void* heap_caps_malloc(size_t bytes,unsigned caps){return allocate(bytes,caps);}
void* heap_caps_aligned_alloc(size_t,size_t bytes,unsigned caps){return allocate(bytes,caps);}
bool esp_ptr_internal(void* p){return internalPointers.count(p)!=0;}
void hostFree(void* p){assert(live.erase(p)==1);internalPointers.erase(p);std::free(p);}
void heap_caps_free(void* p){if(p)hostFree(p);}
#define free hostFree
struct esp_video_buffer_element;
typedef TAILQ_ENTRY(esp_video_buffer_element) esp_video_buffer_node_t;
// INSERT_TYPES
// INSERT_SDK
// INSERT_PRODUCTION
int main() {
  esp_video_buffer_info info{2,32,8,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT,V4L2_MEMORY_MMAP};
  auto* b=__wrap_esp_video_buffer_create(&info);
  assert(b&&esp_ptr_internal(b)&&b->info.count==2&&b->info.caps==info.caps);
  assert(live.size()==3&&internalPointers.size()==1);
  for(unsigned i=0;i<2;++i) {
    assert(b->element[i].video_buffer==b&&b->element[i].index==i);
    assert(b->element[i].free&&!b->element[i].node.tqe_next&&!b->element[i].node.tqe_prev);
    assert(b->element[i].buffer&&!esp_ptr_internal(b->element[i].buffer));
    b->element[i].buffer[31]=0x56;
  }
  esp_video_buffer_destroy(b);assert(live.empty());
  failInternal=true;assert(!__wrap_esp_video_buffer_create(&info)&&live.empty());failInternal=false;
  failAll=true;assert(!__wrap_esp_video_buffer_create(&info)&&live.empty());failAll=false;
  info.caps=MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT;
  b=__wrap_esp_video_buffer_create(&info);assert(b&&live.size()==3&&internalPointers.size()==3);
  esp_video_buffer_destroy(b);assert(live.empty());
  std::puts("Actual SDK video buffers: internal descriptors, unchanged pixel ownership and failed-allocation cleanup passed");
}
'''

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--video-component', type=Path,
                        default=COMPONENT.parents[1]/'managed_components/espressif__esp_video')
    args = parser.parse_args()
    header = args.video_component/'private_include/esp_video_buffer.h'
    if not header.is_file():
        print('SKIP: resolve a P4 camera build or pass --video-component')
        return 77
    types = header.read_text()
    sdk = (args.video_component/'src/esp_video_buffer.c').read_text()
    structs = '\n'.join(extract_block(types[types.index(marker):], marker[:-1].rstrip())+';' for marker in (
        'struct esp_video_buffer_info {','struct esp_video_buffer_element {','struct esp_video_buffer {'))
    functions = extract_block(sdk,'struct esp_video_buffer *esp_video_buffer_create(')
    functions = functions.replace('esp_video_buffer_create(', '__real_esp_video_buffer_create(', 1)
    functions += '\n'+extract_block(sdk,'esp_err_t esp_video_buffer_destroy(')
    # The SDK source is C. Add only its two implicit void* conversions for the
    # C++ harness; allocation/destruction logic remains the production code.
    functions = functions.replace('buffer = heap_caps_calloc(', 'buffer = (esp_video_buffer*)heap_caps_calloc(')
    functions = functions.replace('element->buffer = heap_caps_aligned_alloc(', 'element->buffer = (uint8_t*)heap_caps_aligned_alloc(')
    functions = 'extern "C" {\n'+functions+'\n}'
    production = re.sub(r'^#include.*\n','',(COMPONENT/'HAL_Camera_VideoBuffers.cpp').read_text(),flags=re.M)
    unit = HARNESS.replace('// INSERT_TYPES',structs).replace('// INSERT_SDK',functions).replace('// INSERT_PRODUCTION',production)
    with tempfile.TemporaryDirectory(prefix='hw1-camera-buffers-') as directory:
        source=Path(directory)/'test.cpp';binary=Path(directory)/'test';source.write_text(unit)
        flags=['clang++','-std=c++17','-Wall','-Wextra','-Werror','-Wno-sign-compare']
        if args.sanitize:flags+=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
        subprocess.run([*flags,str(source),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)

if __name__=='__main__':sys.exit(main())
