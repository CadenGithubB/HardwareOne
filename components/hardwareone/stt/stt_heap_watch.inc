// Diagnostic only: included by stt_internal_memory.cpp when HW1_STT_HEAP_WATCH is 1 or 2.
//
// Answers "who held the memory at its low point": HW1_STT_HEAP_WATCH=1 watches
// the internal heap (every block), =2 the PSRAM heap (blocks of 4 KiB and more).
// Between heapWatchBegin() and a heapWatchReport(), every task's allocations in
// the watched heap are entered in a PSRAM table (pointer, bytes, task, and up to
// ten flash-text return addresses read off the allocating stack: a word counts
// when the instruction before it is a call, so no frame pointers are needed).
// Each time the watched heap reaches a new low the whole table is copied; the
// report groups that copy by task and call path. Resolve the addresses with
// riscv32-esp-elf-addr2line. Mode 1 costs ~430 KB of PSRAM .bss, mode 2 ~110 KB,
// and each allocation a short critical section; allocations and frees from
// interrupts are not seen. Never for a release build.
#include <cstring>
#include "esp_attr.h"
#include "esp_private/freertos_debug.h"
extern "C" { extern char _text_start, _text_end; }
namespace {
#if HW1_STT_HEAP_WATCH==2
constexpr size_t kWatchBlocks=1024, kWatchMinBytes=4096, kWatchStep=16384;
constexpr uint32_t kWatchCaps=MALLOC_CAP_SPIRAM;
inline bool watchHeap(const void* p) { return esp_ptr_external_ram(p); }
#else
constexpr size_t kWatchBlocks=4096, kWatchMinBytes=0, kWatchStep=2048;
constexpr uint32_t kWatchCaps=MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT;
inline bool watchHeap(const void* p) { return esp_ptr_internal(p); }
#endif
constexpr size_t kWatchTasks=32, kWatchProbe=160, kWatchCallers=10, kWatchGroups=384, kWatchKey=5;
struct WatchBlock { void* p; uint32_t bytes; uint32_t task; uint32_t ra[kWatchCallers]; };
struct WatchGroup { uint32_t key, bytes, blocks, task, largest; uint32_t ra[kWatchCallers]; };
EXT_RAM_BSS_ATTR WatchBlock gWatch[kWatchBlocks];
EXT_RAM_BSS_ATTR WatchBlock gWatchLow[kWatchBlocks];      // the table when the internal heap was lowest
EXT_RAM_BSS_ATTR WatchGroup gWatchGroups[kWatchGroups];
struct WatchState {
    std::atomic<bool> active{false};
    bool frozen=false;
    uint32_t overflow=0, allocs=0, liveBytes=0, liveBlocks=0;
    uint32_t startFree=0, startMin=0, startMs=0, minFree=UINT32_MAX, minMs=0, copiedFree=UINT32_MAX, copiedBytes=0, copiedBlocks=0;
    uint32_t lowTask=0, lowBytes=0;
    TaskHandle_t tasks[kWatchTasks]={};
    char names[kWatchTasks][configMAX_TASK_NAME_LEN]={};
} gW;
portMUX_TYPE gWatchMux=portMUX_INITIALIZER_UNLOCKED;

uint32_t watchTask() {   // under gWatchMux
    const TaskHandle_t t=xTaskGetCurrentTaskHandle();
    const char* name=pcTaskGetName(t);
    for(size_t i=0;i<kWatchTasks;++i) {
        if(gW.tasks[i]==t && !strncmp(gW.names[i],name,configMAX_TASK_NAME_LEN))return i;
        if(!gW.tasks[i]) { gW.tasks[i]=t; strlcpy(gW.names[i],name,sizeof(gW.names[i])); return i; }
    }
    return kWatchTasks-1;
}
inline bool watchReturnAddress(uint32_t w) {
    const uintptr_t a=w;
    if((a&1) || a<uintptr_t(&_text_start)+4 || a>=uintptr_t(&_text_end))return false;
    uint32_t i4; uint16_t i2;
    memcpy(&i4,reinterpret_cast<const void*>(a-4),4);
    memcpy(&i2,reinterpret_cast<const void*>(a-2),2);
    return (i4&0xFFF)==0x0EF || (i4&0x7FFF)==0x00E7 || (i2&0xF07F)==0x9002 || (i2&0xE003)==0x2001;   // jal ra, jalr ra, c.jalr, c.jal
}
inline void watchCallers(uint32_t* out) {
    uintptr_t sp;
    __asm__ volatile("mv %0, sp" : "=r"(sp));
    uintptr_t end=sp+4096;
    TaskSnapshot_t snap{};
    if(vTaskGetSnapshot(xTaskGetCurrentTaskHandle(),&snap)==pdTRUE) {
        const uintptr_t top=reinterpret_cast<uintptr_t>(snap.pxEndOfStack);
        if(top>sp && top<end)end=top;
    }
    size_t n=0;
    for(uintptr_t a=(sp+3)&~uintptr_t(3);a+4<=end && n<kWatchCallers;a+=4) {
        const uint32_t w=*reinterpret_cast<const uint32_t*>(a);
        if(watchReturnAddress(w) && (!n || out[n-1]!=w))out[n++]=w;
    }
    while(n<kWatchCallers)out[n++]=0;
}
inline void heapWatchAlloc(void* p,size_t bytes) {
    if(!gW.active.load(std::memory_order_relaxed) || !p || xPortInIsrContext() || !watchHeap(p))return;
    const uint32_t freeNow=heap_caps_get_free_size(kWatchCaps);
    if(bytes<kWatchMinBytes && freeNow>=gW.minFree)return;   // small blocks only move the low mark
    uint32_t ra[kWatchCallers];
    watchCallers(ra);
    portENTER_CRITICAL(&gWatchMux);
    const uint32_t task=watchTask();
    const size_t h=(uintptr_t(p)>>3)&(kWatchBlocks-1);
    WatchBlock* slot=nullptr;
    for(size_t i=0;i<kWatchProbe;++i) {
        WatchBlock& b=gWatch[(h+i)&(kWatchBlocks-1)];
        if(b.p==p) { slot=&b; gW.liveBytes-=b.bytes; --gW.liveBlocks; break; }   // in-place realloc, or a free we did not see
        if(!b.p && !slot)slot=&b;
    }
    if(bytes<kWatchMinBytes) {
        if(slot && slot->p==p) { slot->p=nullptr; }   // a tracked block shrank below the threshold
        slot=nullptr;
    } else if(slot) {
        slot->p=p; slot->bytes=bytes; slot->task=task; memcpy(slot->ra,ra,sizeof(ra));
        gW.liveBytes+=bytes; ++gW.liveBlocks;
    } else ++gW.overflow;
    ++gW.allocs;
    if(freeNow<gW.minFree) {
        gW.minFree=freeNow; gW.minMs=esp_log_timestamp(); gW.lowTask=task; gW.lowBytes=bytes;
        if(!gW.frozen && freeNow+kWatchStep<=gW.copiedFree) {
            memcpy(gWatchLow,gWatch,sizeof(gWatch));
            gW.copiedFree=freeNow; gW.copiedBytes=gW.liveBytes; gW.copiedBlocks=gW.liveBlocks;
        }
    }
    portEXIT_CRITICAL(&gWatchMux);
}
inline void heapWatchFree(void* p) {
    if(!gW.active.load(std::memory_order_relaxed) || !p || xPortInIsrContext() || !watchHeap(p))return;
    portENTER_CRITICAL(&gWatchMux);
    const size_t h=(uintptr_t(p)>>3)&(kWatchBlocks-1);
    for(size_t i=0;i<kWatchProbe;++i) {
        WatchBlock& b=gWatch[(h+i)&(kWatchBlocks-1)];
        if(b.p==p) { gW.liveBytes-=b.bytes; --gW.liveBlocks; b.p=nullptr; break; }
    }
    portEXIT_CRITICAL(&gWatchMux);
}
}
namespace hw1::stt {
void heapWatchBegin() {
    portENTER_CRITICAL(&gWatchMux);
    memset(gWatch,0,sizeof(gWatch));
    gW.frozen=false; gW.overflow=gW.allocs=gW.liveBytes=gW.liveBlocks=0;
    gW.minFree=gW.copiedFree=UINT32_MAX; gW.minMs=gW.copiedBytes=gW.copiedBlocks=gW.lowTask=gW.lowBytes=0;
    memset(gW.tasks,0,sizeof(gW.tasks));
    gW.startFree=heap_caps_get_free_size(kWatchCaps);
    gW.startMin=heap_caps_get_minimum_free_size(kWatchCaps);
    gW.startMs=esp_log_timestamp();
    portEXIT_CRITICAL(&gWatchMux);
    gW.active.store(true,std::memory_order_release);
}
void heapWatchReport(const char* when) {
    if(!gW.active.load(std::memory_order_acquire))return;
    portENTER_CRITICAL(&gWatchMux);
    gW.frozen=true;   // the copy stays as it is while it is read
    const struct {
        uint32_t overflow,allocs,liveBytes,liveBlocks,startFree,startMin,startMs,minFree,minMs,copiedFree,copiedBytes,copiedBlocks,lowTask,lowBytes;
    } s={gW.overflow,gW.allocs,gW.liveBytes,gW.liveBlocks,gW.startFree,gW.startMin,gW.startMs,
         gW.minFree,gW.minMs,gW.copiedFree,gW.copiedBytes,gW.copiedBlocks,gW.lowTask,gW.lowBytes};
    portEXIT_CRITICAL(&gWatchMux);
    ESP_LOGI("STT","HW %s start_free=%u start_min=%u start_ms=%u min_free=%u min_ms=%u copy_free=%u copy_bytes=%u copy_blocks=%u "
                   "now_free=%u now_min=%u now_bytes=%u now_blocks=%u allocs=%u overflow=%u low_alloc=%u by=%s",
             when,unsigned(s.startFree),unsigned(s.startMin),unsigned(s.startMs),unsigned(s.minFree),unsigned(s.minMs),
             unsigned(s.copiedFree),unsigned(s.copiedBytes),unsigned(s.copiedBlocks),
             unsigned(heap_caps_get_free_size(kWatchCaps)),
             unsigned(heap_caps_get_minimum_free_size(kWatchCaps)),
             unsigned(s.liveBytes),unsigned(s.liveBlocks),unsigned(s.allocs),unsigned(s.overflow),unsigned(s.lowBytes),gW.names[s.lowTask]);
#if HW1_STT_HEAP_WATCH==2
    // Every block of 48 KiB and more, largest first (bit 31 of task marks a listed block).
    for(int rank=0;rank<64;++rank) {
        WatchBlock* best=nullptr;
        for(WatchBlock& b:gWatchLow)
            if(b.p && b.bytes>=48*1024 && !(b.task&0x80000000u) && (!best || b.bytes>best->bytes))best=&b;
        if(!best)break;
        ESP_LOGI("STT","HW block %s bytes=%u ra=%08x %08x %08x %08x %08x %08x %08x %08x",gW.names[best->task],unsigned(best->bytes),
                 unsigned(best->ra[0]),unsigned(best->ra[1]),unsigned(best->ra[2]),unsigned(best->ra[3]),unsigned(best->ra[4]),
                 unsigned(best->ra[5]),unsigned(best->ra[6]),unsigned(best->ra[7]));
        best->task|=0x80000000u;
    }
    for(WatchBlock& b:gWatchLow)b.task&=0x7FFFFFFFu;
#endif
    // Per task: bytes, blocks, and bytes by block size (<=128, <=1 K, <=4 K, <=16 K, larger).
    for(size_t t=0;t<kWatchTasks;++t) {
        if(!gW.tasks[t])continue;
        uint32_t bytes=0,blocks=0,bins[5]={};
        for(const WatchBlock& b:gWatchLow) {
            if(!b.p || b.task!=t)continue;
            bytes+=b.bytes; ++blocks;
            bins[b.bytes<=128?0:b.bytes<=1024?1:b.bytes<=4096?2:b.bytes<=16384?3:4]+=b.bytes;
        }
        if(bytes)ESP_LOGI("STT","HW task %-12s bytes=%u blocks=%u by_size=%u/%u/%u/%u/%u",gW.names[t],unsigned(bytes),unsigned(blocks),
                          unsigned(bins[0]),unsigned(bins[1]),unsigned(bins[2]),unsigned(bins[3]),unsigned(bins[4]));
    }
    // Groups: same task and same first kWatchKey return addresses.
    memset(gWatchGroups,0,sizeof(gWatchGroups));
    uint32_t dropped=0;
    for(const WatchBlock& b:gWatchLow) {
        if(!b.p)continue;
        uint32_t key=2166136261u^b.task;
        for(size_t i=0;i<kWatchKey;++i)key=(key^b.ra[i])*16777619u;
        if(!key)key=1;
        WatchGroup* g=nullptr;
        for(size_t i=0;i<kWatchGroups;++i) {
            WatchGroup& c=gWatchGroups[(key+i)%kWatchGroups];
            if(c.key==key || !c.key) { g=&c; break; }
        }
        if(!g) { dropped+=b.bytes; continue; }
        if(!g->key) { g->key=key; g->task=b.task; memcpy(g->ra,b.ra,sizeof(b.ra)); }
        g->bytes+=b.bytes; ++g->blocks;
        if(b.bytes>g->largest)g->largest=b.bytes;
    }
    for(int rank=0;rank<28;++rank) {
        WatchGroup* best=nullptr;
        for(WatchGroup& g:gWatchGroups)if(g.key && g.bytes && (!best || g.bytes>best->bytes))best=&g;
        if(!best || best->bytes<512)break;
        ESP_LOGI("STT","HW group %d %s bytes=%u blocks=%u largest=%u ra=%08x %08x %08x %08x %08x %08x %08x %08x %08x %08x",rank,
                 gW.names[best->task],unsigned(best->bytes),unsigned(best->blocks),unsigned(best->largest),
                 unsigned(best->ra[0]),unsigned(best->ra[1]),unsigned(best->ra[2]),unsigned(best->ra[3]),unsigned(best->ra[4]),
                 unsigned(best->ra[5]),unsigned(best->ra[6]),unsigned(best->ra[7]),unsigned(best->ra[8]),unsigned(best->ra[9]));
        best->bytes=0;
    }
    if(dropped)ESP_LOGI("STT","HW groups_dropped_bytes=%u",unsigned(dropped));
    portENTER_CRITICAL(&gWatchMux);
    gW.frozen=false;
    portEXIT_CRITICAL(&gWatchMux);
}
}
