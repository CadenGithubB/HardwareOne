#!/usr/bin/env python3
"""Exercise the production heap walker, including P4's shifted stack base."""
from pathlib import Path
import subprocess
import tempfile
from test_web_batch_handlers import extract_block

root = Path(__file__).resolve().parents[2]
source = (root / 'System_TaskUtils.cpp').read_text()
unit = r'''
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
struct TaskStatus_t {void* pxStackBase; void* xHandle;};
struct TaskHeapMeasure {uint32_t stackBytes=0,tcbBytes=0,regBytes=0; uintptr_t stackHeapSpan=UINTPTR_MAX;};
struct walker_heap_into_t {intptr_t start=0,end=0x100000;};
struct walker_block_info_t {void* ptr; size_t size; bool used;};
'''
unit += extract_block(source, 'struct TaskHeapWalkCtx') + ';\n'
unit += extract_block(source, 'bool taskHeapWalker(')
unit += r'''
int main(){
 TaskStatus_t tasks[]={{(void*)0x1000,(void*)0x5000},
                      {(void*)0x2180,(void*)0x6000},
                      {(void*)0x3000,(void*)0x7000},
                      {(void*)0x48001000,(void*)0x8000}};
 TaskHeapMeasure out[4]{};TaskHeapWalkCtx ctx{tasks,out,4};
 taskHeapWalker({}, {(void*)0x1000,4096,true}, &ctx);
 assert(out[0].stackBytes==4096 && out[1].stackBytes==0);
 taskHeapWalker({}, {(void*)0x2000,4096,true}, &ctx);
 assert(out[1].stackBytes==4096 && out[2].stackBytes==0); // exclusive end
 taskHeapWalker({}, {(void*)0x3000,4096,false}, &ctx);
 assert(out[2].stackBytes==0); // freed stack is not a live allocation
 taskHeapWalker({}, {(void*)0x5000,376,true}, &ctx);
 assert(out[0].tcbBytes==376 && out[3].stackBytes==0); // external remains unknown
 tasks[0].pxStackBase=(void*)(UINTPTR_MAX-4);out[0].stackBytes=0;
 taskHeapWalker({}, {(void*)(UINTPTR_MAX-8),16,true}, &ctx);
 assert(out[0].stackBytes==16); // no wrapped end-address arithmetic
 // A reserved DMA heap is nested inside a used parent block. Its children,
 // including freed stacks, own the attribution in either traversal order.
 for (bool parentFirst : {false,true}) {
   TaskStatus_t nested[]={{(void*)0x11180,(void*)0x20000},
                         {(void*)0x13180,(void*)0x21000},
                         {(void*)0x15180,(void*)0x22000}};
   TaskHeapMeasure measured[3]{}; TaskHeapWalkCtx nestedCtx{nested,measured,3};
   auto parent=[&] {taskHeapWalker({0,0x80000}, {(void*)0x10000,32768,true}, &nestedCtx);};
   auto child=[&] {
     taskHeapWalker({0x10000,0x17fff}, {(void*)0x11000,8192,true}, &nestedCtx);
     taskHeapWalker({0x10000,0x17fff}, {(void*)0x13000,4096,true}, &nestedCtx);
     taskHeapWalker({0x10000,0x17fff}, {(void*)0x15000,4096,false}, &nestedCtx);
   };
   if(parentFirst){parent();child();}else{child();parent();}
   assert(measured[0].stackBytes==8192);
   assert(measured[1].stackBytes==4096);
   assert(measured[2].stackBytes==0);
 }
}
'''
with tempfile.TemporaryDirectory(prefix='hw1-task-heap-') as d:
    cpp, exe = Path(d)/'test.cpp', Path(d)/'test'
    cpp.write_text(unit)
    subprocess.run(['clang++','-std=c++17','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined',str(cpp),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print('Task heap measurement tests passed')
