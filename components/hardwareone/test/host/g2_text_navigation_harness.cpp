// Production gesture-route test: fake platform/queue, real route and slot owner.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>
enum G2TapKind { G2_TAP_PAGE_NEXT, G2_TAP_PAGE_PREV };
using G2TapFn = void (*)(G2TapKind);
struct G2Temple { bool connected=true; uint32_t connectionGeneration=4; char side='R'; };
static G2Temple temple;
static uint32_t gPresentationEpoch=2, gWidgetLifecycleEpoch=1;
static uint32_t gTextViewLifecycleEpoch, gTextViewPresentationEpoch, gTextViewSlotSerial;
static uint32_t gTextNavPendingSerial, gTextExitPendingSerial;
static uint8_t gPageSwapAdmission, gInteractiveLensLease;
static bool gWidgetTerminalLatched, gTextViewActive, gTextViewIsTextPage;
static bool overlay, swapping, hijacked=true;
static void (*gTextViewExitFn)();
static G2TapFn gTextViewTapFn;
static int gSyncLifecycleMux, gTopologyMux;
static uint32_t gHijackStartedMs;
static uint32_t millis() { return 100; }
static bool g2FsmPageSwapping() { return swapping; }
static bool g2FsmHijackActive() { return hijacked; }
static bool firmwareOverlayActiveLocked(uint32_t) { assert(gSyncLifecycleMux==1); return overlay; }
static void portENTER_CRITICAL(int* mux) { assert(!*mux); if(mux==&gTopologyMux)assert(gSyncLifecycleMux==1); *mux=1; }
static void portEXIT_CRITICAL(int* mux) { assert(*mux==1); *mux=0; }
template<class... T> static void debug(const char*, T...) {}
#define DEBUG_G2F debug
static const char* osEventTypeName(uint32_t) { return "test"; }
enum G2HijackPage { G2_HIJACK_PAGE_MAIN, G2_HIJACK_PAGE_OTHER };
struct G2PageModule { const char* name; void (*handleTap)(int); };
static G2HijackPage currentPage=G2_HIJACK_PAGE_MAIN;
static G2PageModule module={"list",nullptr};
static G2HijackPage g2GetHijackPage() { return currentPage; }
static const G2PageModule* g2FindPageByHijackPage(G2HijackPage) { return &module; }
enum class G2HoldVia { Text };
static unsigned holdCopies;
static bool g2HoldIsEvent(uint32_t type) { return type==8 || type==9; }
static void g2HoldIngest(G2Temple&,uint32_t,uint32_t,G2HoldVia,uint32_t,uint32_t,uint32_t) { ++holdCopies; }
// INSERT_PRODUCTION
struct Queued { G2TapFn nav; void(*exit)(); G2TapKind kind; uint32_t presentation, lifecycle, serial; };
static std::vector<Queued> queued;
static bool tapDispatcherEnqueueNav(G2TapFn nav,G2TapKind kind,uint32_t presentation,uint32_t lifecycle,uint32_t serial) {
  queued.push_back({nav,nullptr,kind,presentation,lifecycle,serial}); return true;
}
static bool tapDispatcherEnqueueTextExit(void(*exit)(),uint32_t presentation,uint32_t lifecycle,uint32_t serial) {
  queued.push_back({nullptr,exit,G2_TAP_PAGE_NEXT,presentation,lifecycle,serial}); return true;
}
static std::vector<G2TapKind> ordinaryMoves, multiMoves;
static unsigned exits;
static void ordinaryNav(G2TapKind kind) { ordinaryMoves.push_back(kind); }
static void multiNav(G2TapKind kind) { multiMoves.push_back(kind); }
static void onExit() { ++exits; }
static void listTap(int) {}
static void routeText(G2Temple& t,uint32_t etype,uint32_t rxGeneration,uint32_t rxLifecycleEpoch,uint32_t rxPresentationEpoch) {
  do {
// INSERT_TEXT_ROUTE
  } while(false);
}
static void routeSys(G2Temple& t,uint32_t etype,uint32_t rxGeneration,uint32_t rxLifecycleEpoch,uint32_t rxPresentationEpoch) {
  const uint32_t src=2;
// INSERT_SYS_ROUTE
}
using Route = void(*)(G2Temple&,uint32_t,uint32_t,uint32_t,uint32_t);
static void reset(G2TapFn nav) {
  temple={}; gPresentationEpoch=2; gWidgetLifecycleEpoch=1;
  gTextViewLifecycleEpoch=gTextViewPresentationEpoch=gTextViewSlotSerial=0;
  gTextNavPendingSerial=gTextExitPendingSerial=0;
  gPageSwapAdmission=gInteractiveLensLease=0;
  gWidgetTerminalLatched=gTextViewActive=gTextViewIsTextPage=overlay=swapping=false;
  gTextViewExitFn=nullptr; gTextViewTapFn=nullptr;
  gHijackStartedMs=holdCopies=exits=0; currentPage=G2_HIJACK_PAGE_MAIN; module.handleTap=nullptr;
  queued.clear(); ordinaryMoves.clear(); multiMoves.clear();
  assert(g2TextViewPublishForLifecycle(onExit,nav,1,2,true));
}
static void emit(Route route,uint32_t type,uint32_t generation=4,uint32_t lifecycle=1,uint32_t presentation=2) {
  route(temple,type,generation,lifecycle,presentation);
  assert(!gSyncLifecycleMux&&!gTopologyMux);
}
static void deliver() {
  for(const auto& job:queued) {
    assert(job.presentation==2&&job.lifecycle==1&&job.serial==gTextViewSlotSerial);
    if(job.nav)job.nav(job.kind); else job.exit();
  }
  queued.clear();
}
int main() {
  for(Route route:{routeText,routeSys}) {
    for(G2TapFn nav:{ordinaryNav,multiNav}) {
      reset(nav);
      emit(route,1);emit(route,2);emit(route,0);
      assert(ordinaryMoves.empty()&&multiMoves.empty()&&queued.size()==3); // callbacks are deferred
      deliver();
      const auto& moves=nav==ordinaryNav ? ordinaryMoves : multiMoves;
      assert((moves==std::vector<G2TapKind>{G2_TAP_PAGE_PREV,G2_TAP_PAGE_NEXT,G2_TAP_PAGE_NEXT}));
      assert(!exits&&gHijackStartedMs==100);
      for(int guard=0;guard<9;++guard) {
        reset(nav);
        switch(guard) {
          case 0: gPageSwapAdmission=1; break;
          case 1: swapping=true; break;
          case 2: overlay=true; break;
          case 3: gWidgetTerminalLatched=true; break;
          case 4: temple.connected=false; break;
          case 5: gTextViewActive=false; break;
          case 6: ++gWidgetLifecycleEpoch; break;
          case 7: ++gPresentationEpoch; break;
          case 8: ++temple.connectionGeneration; break;
        }
        emit(route,1);emit(route,2);assert(queued.empty());
      }
      reset(nav);emit(route,1,0);emit(route,1,4,0);emit(route,1,4,1,0);assert(queued.empty());
      reset(nav);for(uint32_t type:{4u,5u,6u,7u,8u,9u,10u})emit(route,type);assert(queued.empty());
      // Ordinary list pages have no text callback slot; row/list handlers own them.
      reset(nav);portENTER_CRITICAL(&gSyncLifecycleMux);g2TextViewDisarmLocked(true);portEXIT_CRITICAL(&gSyncLifecycleMux);
      currentPage=G2_HIJACK_PAGE_OTHER;module.handleTap=listTap;
      emit(route,1);emit(route,2);emit(route,0);assert(queued.empty());
    }
  }
  reset(multiNav);emit(routeSys,3);assert(queued.size()==1&&!exits);deliver();assert(exits==1&&multiMoves.empty());
  reset(multiNav);emit(routeText,3);assert(queued.empty()); // double-click exits only on SysEvent
  reset(multiNav);emit(routeText,8);emit(routeText,9);assert(holdCopies==2&&queued.empty());
  reset(nullptr);emit(routeText,0);assert(queued.size()==1);deliver();assert(exits==1);
  std::puts("G2 text navigation: production Text/Sys routes, ordinary/multi-text callbacks, list exclusion and ownership guards passed");
}
