#include "Audio_VadPolicy.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <random>
#include <tuple>
#include <vector>
using hw1::audio::AdaptiveVadConfig;
using hw1::audio::AdaptiveVadDecision;
using hw1::audio::adaptiveVadDecision;
// The recorder constants are extracted from production, not duplicated here.
// INSERT_CONSTANTS
struct RecorderState {
  uint32_t gRecSilenceStopMs=1200,gRecSampleRate=16000;
  int32_t gRecPeakAvg=0,gRecFloorAvg=-1;
  uint32_t gRecSilenceMs=0;
  bool gRecHeardSpeech=false;
  uint32_t gRecChunkIdx=0;
  int32_t gRecMinAvg=-1,gRecLatchChunk=-1,gRecLatchAvg=0;
  int32_t gRecFloorWin[kRecFloorWinChunks]={};
  size_t gRecFloorWinIdx=0,gRecFloorWinCount=0;
  bool gRecTrimEnabled=true,gRecPreLatchGaveUp=false,gRecVadAutoStopped=false;
  uint32_t gRecVadEndMs=0,recordingSamples=0,clockMs=0,stopRequests=0;
  bool trimSilent=false;
  uint32_t millis() const {return clockMs;}
  void micRecordingRequestStop(){++stopRequests;}
};
#define DEBUG_MIC_VALUESF(...) ((void)0)
#define DEBUG_MIC_LIFECYCLEF(...) ((void)0)
struct ActualRecorder : RecorderState {
  void step(int32_t avg,size_t sampleCount) {
    bool chunkScoredSilent=false;
    // INSERT_RECORDER_BLOCK
    trimSilent=chunkScoredSilent;
  }
};
// Frozen pre-extraction recorder expressions and ordering. This intentionally
// owns a separate reference implementation; it never invokes the new helper.
struct OriginalRecorder : RecorderState {
  void step(int32_t avg,size_t sampleCount) {
    bool chunkScoredSilent=false;
    if(gRecSilenceStopMs>0) {
      if(avg>gRecPeakAvg)gRecPeakAvg=avg;
      if(gRecMinAvg<0||avg<gRecMinAvg)gRecMinAvg=avg;
      const int32_t floorBefore=gRecFloorAvg;
      gRecFloorWin[gRecFloorWinIdx]=avg;
      gRecFloorWinIdx=(gRecFloorWinIdx+1)%kRecFloorWinChunks;
      if(gRecFloorWinCount<kRecFloorWinChunks)++gRecFloorWinCount;
      gRecFloorAvg=gRecFloorWin[0];
      for(size_t fi=1;fi<gRecFloorWinCount;++fi)
        if(gRecFloorWin[fi]<gRecFloorAvg)gRecFloorAvg=gRecFloorWin[fi];
      int32_t trimCut=kRecSilenceFloorAvg;
      if(2*gRecFloorAvg>trimCut)trimCut=2*gRecFloorAvg;
      int32_t cut=trimCut;
      if(gRecPeakAvg/8>cut)cut=gRecPeakAvg/8;
      const bool latchedNow=(!gRecHeardSpeech&&floorBefore>=0&&avg>=cut&&avg>=kRecSpeechFloorAvg);
      if(latchedNow){gRecHeardSpeech=true;gRecLatchChunk=static_cast<int32_t>(gRecChunkIdx);gRecLatchAvg=avg;}
      const uint32_t chunkMs=gRecSampleRate>0?static_cast<uint32_t>(uint64_t(sampleCount)*1000/gRecSampleRate):128;
      const bool scoredSilent=gRecHeardSpeech&&avg<cut;
      chunkScoredSilent=gRecHeardSpeech&&avg<trimCut;
      if(scoredSilent)gRecSilenceMs+=chunkMs;else gRecSilenceMs=0;
      const uint32_t elapsedMs=gRecSampleRate>0?static_cast<uint32_t>(uint64_t(recordingSamples)*1000/gRecSampleRate):0;
      if(gRecTrimEnabled&&!gRecHeardSpeech&&!gRecPreLatchGaveUp&&elapsedMs>=kRecPreLatchHoldMaxMs)gRecPreLatchGaveUp=true;
      ++gRecChunkIdx;
      if(gRecHeardSpeech&&gRecSilenceMs>=gRecSilenceStopMs&&elapsedMs>=kRecVadMinMs){
        gRecVadAutoStopped=true;gRecVadEndMs=millis();micRecordingRequestStop();
      }
    }
    trimSilent=chunkScoredSilent;
  }
};
static void equal(const RecorderState& a,const RecorderState& b) {
#define SAME(field) assert(a.field==b.field)
  SAME(gRecSilenceStopMs);SAME(gRecSampleRate);SAME(gRecPeakAvg);SAME(gRecFloorAvg);
  SAME(gRecSilenceMs);SAME(gRecHeardSpeech);SAME(gRecChunkIdx);SAME(gRecMinAvg);
  SAME(gRecLatchChunk);SAME(gRecLatchAvg);SAME(gRecFloorWinIdx);SAME(gRecFloorWinCount);
  SAME(gRecTrimEnabled);SAME(gRecPreLatchGaveUp);SAME(gRecVadAutoStopped);SAME(gRecVadEndMs);
  SAME(recordingSamples);SAME(clockMs);SAME(stopRequests);SAME(trimSilent);
#undef SAME
  assert(std::equal(std::begin(a.gRecFloorWin),std::end(a.gRecFloorWin),std::begin(b.gRecFloorWin)));
}
static void replay(const std::vector<int32_t>& levels,uint32_t rate,uint32_t stopMs,bool trim) {
  ActualRecorder actual;OriginalRecorder original;
  actual.gRecSampleRate=original.gRecSampleRate=rate;
  actual.gRecSilenceStopMs=original.gRecSilenceStopMs=stopMs;
  actual.gRecTrimEnabled=original.gRecTrimEnabled=trim;
  const size_t sizes[]={1,159,160,512,2048,1600};
  for(size_t i=0;i<levels.size();++i){
    const auto count=sizes[i%6];actual.step(levels[i],count);original.step(levels[i],count);equal(actual,original);
    actual.recordingSamples+=count;original.recordingSamples+=count;
    actual.clockMs+=127;original.clockMs+=127;
  }
}
static AdaptiveVadDecision originalDecision(int32_t level,int32_t before,int32_t floor,int32_t peak,bool heard,AdaptiveVadConfig config) {
  int32_t trim=config.silenceFloor;if(2*floor>trim)trim=2*floor;
  int32_t stop=trim;if(peak/8>stop)stop=peak/8;
  const bool latch=!heard&&before>=0&&level>=stop&&level>=config.speechFloor;
  heard=heard||latch;
  return {trim,stop,latch,heard,heard&&level<stop,heard&&level<trim};
}
static void decisions() {
  const int32_t boundaries[]={0,1,5,6,44,45,46,119,120,121,255,4095,16384,32768};
  for(const auto config:{AdaptiveVadConfig{120,45},AdaptiveVadConfig{12,6}})
    for(auto floor:boundaries)for(auto peak:boundaries)for(bool heard:{false,true})for(auto before:{-1,0,120}) {
      const auto reference=originalDecision(0,before,floor,peak,heard,config);
      std::vector<int32_t> levels(std::begin(boundaries),std::end(boundaries));
      for(auto cut:{reference.trimCut,reference.stopCut,config.speechFloor})
        for(int delta=-1;delta<=1;++delta)if(cut+delta>=0&&cut+delta<=32768)levels.push_back(cut+delta);
      for(auto level:levels) {
        const auto a=adaptiveVadDecision(level,before,floor,peak,heard,config);
        const auto b=originalDecision(level,before,floor,peak,heard,config);
        assert(a.trimCut==b.trimCut&&a.stopCut==b.stopCut&&a.latch==b.latch&&a.heardSpeech==b.heardSpeech&&a.stopSilent==b.stopSilent&&a.trimSilent==b.trimSilent);
      }
    }
  // The first chunk cannot latch; equality at both thresholds can latch.
  assert(!adaptiveVadDecision(120,-1,0,120,false).latch);
  assert(adaptiveVadDecision(120,0,0,120,false).latch);
  assert(!adaptiveVadDecision(119,0,0,119,false).latch);
  // Peak-based stop and noise-based trim deliberately make different decisions.
  const auto tail=adaptiveVadDecision(60,20,20,800,true);
  assert(tail.stopSilent&&!tail.trimSilent&&tail.trimCut==45&&tail.stopCut==100);
}
int main() {
  static_assert(kRecFloorWinChunks==40&&kRecSpeechFloorAvg==120&&kRecSilenceFloorAvg==45);
  static_assert(kRecVadMinMs==800&&kRecPreLatchHoldMaxMs==4000);
  decisions();
  std::vector<std::vector<int32_t>> cases;
  cases.push_back(std::vector<int32_t>(1500,0));
  cases.push_back(std::vector<int32_t>(1500,150)); // Room tone above absolute speech floor.
  cases.push_back(std::vector<int32_t>(500,1000)); // Initial speech-level floor seed.
  std::vector<int32_t> speech(60,20);speech.insert(speech.end(),160,900);speech.insert(speech.end(),80,60);speech.insert(speech.end(),80,20);cases.push_back(speech);
  std::vector<int32_t> step(100,20);step.insert(step.end(),200,200);step.insert(step.end(),100,20);step.push_back(32768);step.insert(step.end(),200,130);cases.push_back(step);
  std::vector<int32_t> exact={0,44,45,46,119,120,121,239,240,241,7,8,9,32767,32768};
  for(int repeat=0;repeat<50;++repeat)exact.insert(exact.end(),{44,45,119,120,20,240});cases.push_back(exact);
  std::mt19937 random(0x564144);std::vector<int32_t> varying(25000);
  for(auto& level:varying)level=static_cast<int32_t>(random()%32769);cases.push_back(varying);
  for(const auto& levels:cases)for(uint32_t rate:{0u,8000u,16000u,48000u})
    for(uint32_t stop:{0u,1u,600u,1200u,1800u})for(bool trim:{false,true})replay(levels,rate,stop,trim);
  puts("PASS: shared VAD threshold boundaries and actual recorder parity (window/latch/trim/timing/source rates/disabled mode)");
}
