#include "stt_segmenter.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <vector>
using namespace hw1::stt;
struct Saved { SpeechSegment info;std::vector<int16_t> pcm; };
static std::vector<int16_t> voice(size_t n,int dc=0,int level=1000) {
    std::vector<int16_t> out(n);for(size_t i=0;i<n;++i)out[i]=static_cast<int16_t>(dc+(i%2?level:-level));return out;
}
static void append(std::vector<int16_t>& out,const std::vector<int16_t>& value) { out.insert(out.end(),value.begin(),value.end()); }
static void save(Segmenter& segmenter,std::vector<Saved>& out) {
    const auto* s=segmenter.segment();assert(s&&s->pcm&&s->samples>0&&s->samples<=320000);
    assert(s->end_sample-s->start_sample==s->samples);
    assert(s->fresh_start_sample-s->start_sample==s->overlap_before_samples);
    out.push_back({*s,std::vector<int16_t>(s->pcm,s->pcm+s->samples)});out.back().info.pcm=nullptr;
    assert(segmenter.release());
}
static std::vector<Saved> run(const std::vector<int16_t>& input,const std::vector<size_t>& chunks,
                              const SegmenterConfig& config=SegmenterConfig{}) {
    std::vector<int16_t> work(config.max_segment_samples+2,12345);Segmenter segmenter;
    assert(segmenter.reset(work.data()+1,config.max_segment_samples,config));
    std::vector<Saved> out;size_t at=0,index=0;
    while(at<input.size()) {
        size_t left=std::min(chunks[index++%chunks.size()],input.size()-at);
        while(left) {
            auto result=segmenter.push(input.data()+at,left);assert(result.consumed<=left);
            at+=result.consumed;left-=result.consumed;
            if(result.status==SegmentStatus::Ready)save(segmenter,out);
            else { assert(result.status==SegmentStatus::Ok);assert(result.consumed>0); }
            assert(work.front()==12345&&work.back()==12345);
        }
    }
    if(segmenter.finish()==SegmentStatus::Ready)save(segmenter,out);
    assert(segmenter.finish()==SegmentStatus::Finished);
    assert(segmenter.push(input.data(),input.size()).status==SegmentStatus::Finished);
    assert(segmenter.samples_seen()==input.size());
    for(const auto& s:out) {
        assert(s.info.end_sample<=input.size());
        assert(std::equal(s.pcm.begin(),s.pcm.end(),input.begin()+s.info.start_sample));
    }
    return out;
}
static void same(const std::vector<Saved>& a,const std::vector<Saved>& b) {
    assert(a.size()==b.size());for(size_t i=0;i<a.size();++i) {
        assert(a[i].pcm==b[i].pcm);
        assert(a[i].info.start_sample==b[i].info.start_sample&&a[i].info.end_sample==b[i].info.end_sample);
        assert(a[i].info.fresh_start_sample==b[i].info.fresh_start_sample);
        assert(a[i].info.end==b[i].info.end&&a[i].info.new_voiced_samples==b[i].info.new_voiced_samples);
        assert(a[i].info.continuation==b[i].info.continuation);
    }
}
static void exact_delivery(const std::vector<Saved>& output,const std::vector<int16_t>& input) {
    std::vector<int16_t> fresh;uint64_t next=0;
    for(const auto& s:output) {
        assert(s.info.fresh_start_sample==next);
        fresh.insert(fresh.end(),s.pcm.begin()+s.info.overlap_before_samples,s.pcm.end());
        next=s.info.end_sample;
    }
    assert(next==input.size()&&fresh==input);
}
static void adaptive_tests() {
    // Preserve explicit low-floor coverage: the policy is configurable without
    // a per-source implementation. Production defaults are exercised below.
    SegmenterConfig config;config.adaptive=true;
    config.adaptive_speech_floor=12;config.adaptive_silence_floor=6;
    std::vector<int16_t> work(320000,12345);Segmenter segmenter;
    assert(segmenter.reset(work.data(),work.size(),config));
    assert(!segmenter.calibrated()&&segmenter.noise_rms()==0&&segmenter.peak_rms()==0);
    const auto ambient=voice(8000,20000,4);
    assert(segmenter.push(ambient.data(),7999).consumed==7999&&!segmenter.calibrated());
    assert(segmenter.push(ambient.data()+7999,1).consumed==1&&segmenter.calibrated());
    assert(segmenter.samples_seen()==8000&&segmenter.current_rms()==4&&segmenter.noise_rms()==4);
    assert(segmenter.threshold_rms()==12&&segmenter.peak_rms()==0);
    for(int16_t sample:work)assert(sample==12345); // Calibration is never even stored as pre-roll.
    const auto low=voice(3200,20000,20);
    assert(segmenter.push(low.data(),low.size()).status==SegmentStatus::Ok);
    assert(segmenter.current_rms()==20&&segmenter.noise_rms()==4&&segmenter.peak_rms()==20);
    assert(segmenter.threshold_rms()==8); // Onset12, latched silence max(6,2*ambient,peak/8).
    assert(segmenter.finish()==SegmentStatus::Ready);
    assert(segmenter.segment()->start_sample==8000&&segmenter.segment()->end_sample==11200);
    assert(std::equal(low.begin(),low.end(),segmenter.segment()->pcm));
    segmenter.reset_session();assert(!segmenter.calibrated()&&segmenter.samples_seen()==0);
    assert(segmenter.current_rms()==0&&segmenter.noise_rms()==0&&segmenter.peak_rms()==0);
    assert(segmenter.push(ambient.data(),7999).status==SegmentStatus::Ok);
    assert(segmenter.finish()==SegmentStatus::Finished&&!segmenter.calibrated());
    assert(run(voice(8000,0,24000),{17,999},config).empty()); // Loud calibration is ambient-only too.

    // A low raw signal rejected by the old400 floor is valid adaptive voice.
    auto low_session=voice(8000,0,4);append(low_session,voice(16000,0,20));
    auto low_out=run(low_session,{173},config);
    assert(low_out.size()==1&&low_out[0].info.start_sample==8000);
    assert(run(low_session,{173}).empty());
    auto too_quiet=voice(8000,0,2);append(too_quiet,voice(16000,0,10));
    assert(run(too_quiet,{601},config).empty());

    // Noise history ages after exactly500 completed frames, not per push call.
    // Modest ambient steps stay below onset; upward speech-like jumps cannot
    // be distinguished from a voice by an energy-only detector.
    assert(segmenter.reset(work.data(),work.size(),config));
    auto four=voice(8000,0,4);assert(segmenter.push(four.data(),four.size()).status==SegmentStatus::Ok);
    auto six=voice(500*160,0,6);
    assert(segmenter.push(six.data(),six.size()-160).status==SegmentStatus::Ok&&segmenter.noise_rms()==4);
    assert(segmenter.push(six.data()+six.size()-160,160).status==SegmentStatus::Ok&&segmenter.noise_rms()==6);
    auto nine=voice(500*160,0,9);assert(segmenter.push(nine.data(),nine.size()).status==SegmentStatus::Ok);
    assert(segmenter.noise_rms()==9&&segmenter.threshold_rms()==18&&segmenter.segment()==nullptr);
    auto quiet_frame=voice(160,0,0);assert(segmenter.push(quiet_frame.data(),160).status==SegmentStatus::Ok);
    assert(segmenter.noise_rms()==0);
    assert(segmenter.push(six.data(),six.size()-160).status==SegmentStatus::Ok&&segmenter.noise_rms()==0);
    assert(segmenter.push(six.data()+six.size()-160,160).status==SegmentStatus::Ok&&segmenter.noise_rms()==6);
    assert(segmenter.finish()==SegmentStatus::Finished);

    // Before any utterance, a high ambient calibration/idle frame must not
    // keep peak/8 high after the room becomes quiet. The window may fall fast.
    auto quieter_room=voice(8000,0,1600);append(quieter_room,voice(16000,0,1600));
    append(quieter_room,voice(8000,0,2));append(quieter_room,voice(3200,0,20));
    auto room_out=run(quieter_room,{683},config);
    assert(room_out.size()==1&&room_out[0].info.new_voiced_samples==3200);
    assert(room_out[0].info.start_sample==27200&&room_out[0].info.end_sample==35200);

    // Natural release forgets the previous utterance's loud peak, so a later
    // quiet voice can latch. Both raw source amplitudes use the same algorithm.
    auto two=voice(8000,0,2);append(two,voice(16000,0,1600));append(two,voice(7*16000,0,2));
    append(two,voice(16000,0,20));append(two,voice(7*16000,0,2));
    auto two_out=run(two,{781},config);
    assert(two_out.size()==2&&two_out[0].info.end==SegmentEnd::Pause&&two_out[1].info.end==SegmentEnd::Pause);
    assert(two_out[0].info.start_sample==8000&&two_out[0].info.end_sample==136000);
    assert(two_out[1].info.start_sample==136000&&two_out[1].info.end_sample==264000);
    assert(two_out[1].info.new_voiced_samples==16000);
    same(two_out,run(two,{1},config));same(two_out,run(two,{two.size()},config));

    // Peak-relative stop scoring must not discard floor-relative quiet tails.
    auto tail=voice(8000,0,2);append(tail,voice(3200,0,1600));append(tail,voice(6400,0,20));
    append(tail,voice(128000-9600,0,2));
    auto tail_out=run(tail,{8192,13},config);
    assert(tail_out.size()==1&&tail_out[0].info.new_voiced_samples==9600);
    assert(tail_out[0].info.samples==128000); // Entire admitted PCM remains unmodified.

    // Calibration contributes to absolute time but never to the next pre-roll.
    auto pause=voice(16000,0,4);append(pause,voice(16000,0,20));append(pause,voice(7*16000,0,4));
    const auto preroll=run(pause,{47,1001},config);assert(preroll.size()==1);
    assert(preroll[0].info.start_sample==11200&&preroll[0].info.end_sample==139200);
    same(preroll,run(pause,{1},config));same(preroll,run(pause,{pause.size()},config));
    auto shifted=pause;for(auto& sample:shifted)sample+=24000;
    const auto shifted_out=run(shifted,{37,641},config);assert(shifted_out.size()==preroll.size());
    assert(shifted_out[0].info.start_sample==preroll[0].info.start_sample);
    assert(shifted_out[0].info.end_sample==preroll[0].info.end_sample);
    assert(shifted_out[0].info.new_voiced_samples==preroll[0].info.new_voiced_samples);
    for(size_t i=0;i<shifted_out[0].pcm.size();++i)assert(shifted_out[0].pcm[i]==preroll[0].pcm[i]+24000);

    // Speech filling the entire5s minimum window must not become its own noise
    // floor. Preserve default no-overlap hard cuts for low AND high signals.
    for(int amplitude:{20,1600,30000}) {
        auto nonstop=voice(8000,0,2);append(nonstop,voice(45*16000+37,0,amplitude));
        const auto cuts=run(nonstop,{641,7,16000},config);
        assert(cuts.size()==3&&cuts[0].info.end==SegmentEnd::HardLimit&&cuts[1].info.end==SegmentEnd::HardLimit);
        assert(cuts[0].info.start_sample==8000&&cuts[1].info.start_sample==328000&&cuts[2].info.start_sample==648000);
        uint64_t next=8000;std::vector<int16_t> captured;
        for(const auto& chunk:cuts) {
            assert(chunk.info.start_sample==next&&chunk.info.overlap_before_samples==0);
            captured.insert(captured.end(),chunk.pcm.begin(),chunk.pcm.end());next=chunk.info.end_sample;
        }
        assert(next==nonstop.size()&&std::equal(captured.begin(),captured.end(),nonstop.begin()+8000));
        same(cuts,run(nonstop,{nonstop.size()},config));
    }
    auto tiny=voice(8000,0,2);append(tiny,voice(320000+37,0,20));
    const auto tiny_out=run(tiny,{777},config);assert(tiny_out.size()==2&&tiny_out[1].info.samples==37);
    assert(tiny_out[1].info.end_sample==328037);
    auto long_dc=std::vector<int16_t>(120*16000,24000);
    assert(run(long_dc,{997},config).empty());

    // Physical P4 room measurements fluctuated between3 and31 RMS, whereas
    // speech peaks were124..339. Default45/16 floors must not turn that room
    // noise into periodic inference, even with abrupt jumps and two-minute
    // windows. An ordinary60-RMS voice still clears the default onset gate.
    SegmenterConfig physical;physical.adaptive=true;
    assert(physical.adaptive_speech_floor==45&&physical.adaptive_silence_floor==16);
    std::vector<int16_t> room(120*16000);
    for(size_t frame=0;frame<room.size()/160;++frame) {
        const int level=3+int((frame*17+frame/7)%29); // Includes3 and31, independent of push boundaries.
        for(size_t i=0;i<160;++i)room[frame*160+i]=static_cast<int16_t>(i%2?level:-level);
    }
    assert(run(room,{13,1024,511},physical).empty());
    assert(segmenter.reset(work.data(),work.size(),physical));
    assert(segmenter.push(room.data(),7999).status==SegmentStatus::Ok&&!segmenter.calibrated());
    assert(segmenter.push(room.data()+7999,1).status==SegmentStatus::Ok&&segmenter.calibrated());
    assert(segmenter.threshold_rms()==45&&segmenter.segment()==nullptr);
    append(room,voice(16000,0,60));append(room,voice(7*16000,0,3));
    auto default_speech=run(room,{19,8192},physical);
    assert(default_speech.size()==1&&default_speech[0].info.end==SegmentEnd::Pause);
    assert(default_speech[0].info.start_sample==120*16000-4800);
    assert(default_speech[0].info.samples==128000&&default_speech[0].info.new_voiced_samples==16000);
    same(default_speech,run(room,{room.size()},physical));
    auto room_dc=room;for(auto& sample:room_dc)sample+=24000;
    auto default_dc=run(room_dc,{997},physical);assert(default_dc.size()==1);
    assert(default_dc[0].info.start_sample==default_speech[0].info.start_sample);
    assert(default_dc[0].info.end_sample==default_speech[0].info.end_sample);
    assert(default_dc[0].info.new_voiced_samples==default_speech[0].info.new_voiced_samples);
    for(size_t i=0;i<default_dc[0].pcm.size();++i)assert(default_dc[0].pcm[i]==default_speech[0].pcm[i]+24000);

    // Config bounds are independent of transport/source and cap all state.
    for(size_t window:{size_t(0),kSegmentNoiseWindowFrames+1}) {
        auto invalid=config;invalid.noise_window_frames=window;
        assert(!segmenter.reset(work.data(),work.size(),invalid));
    }
    for(size_t samples:{size_t(0),size_t(7999),size_t(80160)}) {
        auto invalid=config;invalid.calibration_samples=samples;
        assert(!segmenter.reset(work.data(),work.size(),invalid));
    }
    auto invalid=config;invalid.adaptive_speech_floor=0;assert(!segmenter.reset(work.data(),work.size(),invalid));
    invalid=config;invalid.adaptive_silence_floor=13;assert(!segmenter.reset(work.data(),work.size(),invalid));
    assert(sizeof(Segmenter)<2048); // Fixed1KB ambient history, never an unbounded ring.
    puts("PASS: adaptive calibration exclusion, configurable low/high RMS,120s measured room fluctuations, bounded noise window, loud/quiet reset, raw tails/pre-roll, DC/chunk invariance and sustained hard cuts");
}
// Opt-in latency options the continuous broker uses (HW1_STT_SHORT_CLOSE,
// HW1_STT_TRIM_SILENCE, HW1_STT_SPECULATIVE_FINAL): short phrases may close on
// a longer pause, every segment reports where its voice ended, and the
// in-progress phrase reports the input a pause would close it with.
static void latency_option_tests() {
    SegmenterConfig four;four.min_segment_samples=4*16000;
    // 2 s reply, then silence: the 4 s minimum holds it until 4 s are reached
    // (75200), a 1 s short-close pause closes it as soon as that pause is 1 s.
    std::vector<int16_t> reply(16000,0);append(reply,voice(32000));reply.resize(80000,0);
    const auto held=run(reply,{reply.size()},four);
    assert(held.size()==1&&held[0].info.end==SegmentEnd::Pause&&held[0].info.end_sample==75200);
    auto shortClose=four;shortClose.short_close_silence_samples=16000;
    const auto closed=run(reply,{reply.size()},shortClose);
    assert(closed.size()==1&&closed[0].info.end==SegmentEnd::Pause);
    assert(closed[0].info.start_sample==11200&&closed[0].info.end_sample==64000);
    same(closed,run(reply,{1}, shortClose));same(closed,run(reply,{37,160,1024},shortClose));
    // Voiced end: offset of the last voiced frame's end, reported only.
    assert(held[0].info.voiced_end_samples==48000-11200&&closed[0].info.voiced_end_samples==48000-11200);
    assert(closed[0].pcm.size()==closed[0].info.samples);
    // Below the 4 s minimum the short-close pause must be at least the
    // ordinary end pause and a whole number of 10 ms frames.
    Segmenter check;std::vector<int16_t> buffer(shortClose.max_segment_samples);
    auto invalid=shortClose;invalid.short_close_silence_samples=invalid.end_silence_samples-160;
    assert(!check.reset(buffer.data(),buffer.size(),invalid));
    invalid.short_close_silence_samples=16000+1;assert(!check.reset(buffer.data(),buffer.size(),invalid));
    assert(check.reset(buffer.data(),buffer.size(),shortClose));
    // A click up to silence_blip_samples long after the reply is no voice; a
    // longer run moves the voiced end.
    auto blips=shortClose;blips.silence_blip_samples=480;
    for(size_t run_samples:{size_t(480),size_t(640)}) {
        std::vector<int16_t> clicked(16000,0);append(clicked,voice(32000));
        clicked.resize(clicked.size()+3200,0);append(clicked,voice(run_samples));clicked.resize(96000,0);
        const auto out=run(clicked,{clicked.size()},blips);
        assert(out.size()==1&&out[0].info.end==SegmentEnd::Pause);
        assert(out[0].info.voiced_end_samples==(run_samples==480?48000:51200+run_samples)-11200);
    }
    // Speculation: a 4 s phrase in its pause reports the prefix (voiced end +
    // 200 ms) once those samples exist and while the pause can still close it
    // as an ordinary Pause; the published segment then has that voiced end.
    std::vector<int16_t> phrase(16000,0);append(phrase,voice(64000));phrase.resize(100000,0);
    Segmenter seg;std::vector<int16_t> work(four.max_segment_samples);
    assert(seg.reset(work.data(),work.size(),four));
    uint64_t start=0;size_t voicedEnd=0,samples=0;
    for(size_t at=0;at<phrase.size();at+=160) {
        const auto pushed=seg.push(phrase.data()+at,160);
        if(pushed.status==SegmentStatus::Ready) {
            const auto* s=seg.segment();
            assert(at+160==89600&&s->end==SegmentEnd::Pause&&s->voiced_end_samples==68800&&s->samples==78400);
            assert(seg.release());break;
        }
        assert(pushed.status==SegmentStatus::Ok&&pushed.consumed==160);
        const bool ready=seg.speculation(3200,&start,&voicedEnd,&samples);
        assert(ready==(at+160>=83200));
        if(ready)assert(start==11200&&voicedEnd==68800&&samples==72000);
        uint64_t liveStart=0;size_t liveSamples=0;
        const bool open=seg.live_view(&liveStart,&liveSamples);
        assert(open==(at+160>=16160));  // from the first voiced frame (pre-roll included)
        if(open)assert(liveStart==11200&&liveSamples==at+160-11200);
    }
    // Without a short close a short phrase never speculates: its ordinary
    // pause could not close it.
    Segmenter quick;assert(quick.reset(work.data(),work.size(),four));
    for(size_t at=0;at<64000;at+=160) {
        if(quick.push(reply.data()+at,160).status==SegmentStatus::Ready)break;
        assert(!quick.speculation(3200,&start,&voicedEnd,&samples));
    }
    puts("PASS: short-close pause below the minimum, voiced end with click rejection, speculation prefix and live view");
}
// Speculation with the short close: below the minimum length a continuing
// pause still closes the phrase as a Pause (at the short close or on reaching
// the minimum), so phrases of every length speculate as soon as voiced end +
// tail exist, and the prefix is the input the close then publishes. With
// short_close_silence_samples == 0 only phrases whose ordinary end pause
// reaches the minimum speculate (voiced end + end_silence >= minimum).
static void speculation_short_close_tests() {
    SegmenterConfig four;four.min_segment_samples=4*16000;
    auto shortClose=four;shortClose.short_close_silence_samples=16000;
    constexpr size_t kTail=3200,kLead=16000;
    for(const SegmenterConfig& config:{shortClose,four}) {
        for(size_t ms:{size_t(800),size_t(1500),size_t(2000),size_t(3000),size_t(3500),size_t(3900),size_t(4500),size_t(6000)}) {
            const size_t voiced=ms*16;
            std::vector<int16_t> phrase(kLead,0);append(phrase,voice(voiced));phrase.resize(phrase.size()+3*16000,0);
            // Pre-roll puts the start 300 ms before the voice: voiced end = voice + 4800.
            const size_t ve=voiced+4800;
            const bool expect=config.short_close_silence_samples || ve+config.end_silence_samples>=config.min_segment_samples;
            Segmenter seg;std::vector<int16_t> work(config.max_segment_samples);
            assert(seg.reset(work.data(),work.size(),config));
            bool speculated=false,closed=false;uint64_t specStart=0;size_t specVoicedEnd=0,specSamples=0;
            for(size_t at=0;at<phrase.size();at+=160) {
                const auto pushed=seg.push(phrase.data()+at,160);
                if(pushed.status==SegmentStatus::Ready) {
                    const auto* s=seg.segment();
                    assert(s->end==SegmentEnd::Pause&&s->voiced_end_samples==ve);
                    assert(speculated==expect);
                    if(speculated) {
                        assert(specStart==s->start_sample&&specVoicedEnd==s->voiced_end_samples);
                        assert(specSamples==s->voiced_end_samples+kTail&&specSamples<s->samples);
                    }
                    assert(seg.release());closed=true;break;
                }
                assert(pushed.status==SegmentStatus::Ok&&pushed.consumed==160);
                uint64_t start=0;size_t voicedEnd=0,samples=0;
                const bool ready=seg.speculation(kTail,&start,&voicedEnd,&samples);
                assert(ready==(expect&&at+160>=kLead+voiced+kTail));
                if(ready&&speculated)assert(start==specStart&&voicedEnd==specVoicedEnd&&samples==specSamples);
                if(ready&&!speculated) { speculated=true;specStart=start;specVoicedEnd=voicedEnd;specSamples=samples; }
            }
            assert(closed);
        }
    }
    puts("PASS: speculation below the minimum with the short close, unchanged without it");
}
int main() {
    latency_option_tests();
    speculation_short_close_tests();
    // Natural pause waits for the8s span; the first300ms is exact pre-roll.
    std::vector<int16_t> pause(16000,0);append(pause,voice(16000));pause.resize(9*16000,0);
    const auto baseline=run(pause,{pause.size()});assert(baseline.size()==1);
    assert(baseline[0].info.start_sample==11200&&baseline[0].info.end_sample==139200);
    assert(baseline[0].info.end==SegmentEnd::Pause&&baseline[0].info.new_voiced_samples==16000);
    same(baseline,run(pause,{1}));same(baseline,run(pause,{37,159,160,161,1001,8192}));

    // Continuous45s voice: exact nonoverlapping hard cuts, then a5s final flush.
    const auto nonstop=voice(45*16000);const auto cuts=run(nonstop,{777,32768,3});
    assert(cuts.size()==3&&cuts[0].info.end==SegmentEnd::HardLimit&&cuts[1].info.end==SegmentEnd::HardLimit);
    assert(cuts[2].info.end==SegmentEnd::SessionEnd&&cuts[2].info.samples==5*16000);
    exact_delivery(cuts,nonstop);same(cuts,run(nonstop,{nonstop.size()}));same(cuts,run(nonstop,{1}));

    // Explicit optional overlap changes offsets, never silently deduplicates text/PCM.
    SegmenterConfig overlap;overlap.forced_overlap_samples=16000;
    const auto repeated=run(nonstop,{517},overlap);assert(repeated.size()==3);
    assert(repeated[0].info.next_overlap_samples==16000&&repeated[0].info.overlap_before_samples==0);
    assert(repeated[1].info.start_sample==304000&&repeated[1].info.fresh_start_sample==320000);
    assert(repeated[1].info.overlap_before_samples==16000&&repeated[2].info.next_overlap_samples==0);
    exact_delivery(repeated,nonstop);
    SegmenterConfig large_overlap;large_overlap.forced_overlap_samples=304000;large_overlap.end_silence_samples=160;
    auto discarded_context=voice(320000);discarded_context.resize(320160,0);append(discarded_context,voice(3200));
    auto context_out=run(discarded_context,{901},large_overlap);
    assert(context_out.size()==2&&context_out[1].info.overlap_before_samples==0);
    exact_delivery(context_out,discarded_context);

    // finish bypasses minimum wall span, and preserves sub-minimum voiced tails
    // after a forced boundary, including a final partial analysis frame.
    const auto short_tail=voice(320000+37);const auto tail=run(short_tail,{481});
    assert(tail.size()==2&&tail[1].info.samples==37&&tail[1].info.new_voiced_samples==37);
    exact_delivery(tail,short_tail);
    assert(run(voice(1600),{53}).empty()); //100ms click/noise is below200ms minimum.
    auto minimum=run(voice(3200),{319});assert(minimum.size()==1&&minimum[0].info.end==SegmentEnd::SessionEnd);

    // A pause landing exactly on20s is a natural endpoint, not a forced cut.
    auto boundary=voice(320000-9600);boundary.resize(320000,0);auto boundary_out=run(boundary,{311});
    assert(boundary_out.size()==1&&boundary_out[0].info.end==SegmentEnd::Pause);

    // Discarded idle silence causes no periodic model calls, including after
    // a hard cut and after rejecting a brief unqualified candidate.
    auto silent_tail=voice(320000);silent_tail.resize(50*16000,0);
    auto silent_out=run(silent_tail,{1023});assert(silent_out.size()==1);
    auto click=voice(1600);click.resize(10*16000,0);assert(run(click,{777}).empty());
    std::vector<int16_t> work(320000),silence(997,24000);Segmenter idle;assert(idle.reset(work.data(),work.size()));
    for(size_t i=0;i<10000;++i) { auto r=idle.push(silence.data(),silence.size());assert(r.consumed==silence.size()&&r.status==SegmentStatus::Ok); }
    assert(idle.samples_seen()==9970000&&idle.finish()==SegmentStatus::Finished); //623s constant DC.
    assert(run(voice(8*16000,24000,200),{643}).empty()); //Sub-threshold energy despite large DC.
    auto dc=voice(3200,24000);auto dc_out=run(dc,{57});assert(dc_out.size()==1&&dc_out[0].pcm==dc);

    // Delivered silence cannot be reused as pre-roll for the next segment.
    auto adjacent=voice(16000);adjacent.resize(128000,0);append(adjacent,voice(16000));adjacent.resize(256000,0);
    auto adjacent_out=run(adjacent,{999});assert(adjacent_out.size()==2);
    assert(adjacent_out[0].info.end_sample==128000&&adjacent_out[1].info.start_sample==128000);
    exact_delivery(adjacent_out,adjacent);

    // Ready blocks further input without consuming or modifying the view.
    Segmenter held;assert(held.reset(work.data(),work.size()));auto full=voice(320007);
    auto first=held.push(full.data(),full.size());assert(first.status==SegmentStatus::Ready&&first.consumed==320000);
    const auto view=*held.segment();auto held_pcm=std::vector<int16_t>(view.pcm,view.pcm+view.samples);
    auto blocked=held.push(full.data()+first.consumed,7);assert(blocked.consumed==0&&blocked.status==SegmentStatus::Ready);
    assert(held.samples_seen()==320000&&std::equal(held_pcm.begin(),held_pcm.end(),view.pcm));
    assert(held.finish()==SegmentStatus::Ready&&held.release());assert(held.push(full.data(),7).status==SegmentStatus::Finished);
    held.reset_session();assert(held.samples_seen()==0&&held.segment()==nullptr);
    assert(held.push(full.data(),91).status==SegmentStatus::Ok);held.reset_session();
    assert(held.push(dc.data(),dc.size()).status==SegmentStatus::Ok);
    assert(held.finish()==SegmentStatus::Ready&&held.segment()->start_sample==0&&held.segment()->samples==3200);

    Segmenter invalid;assert(invalid.push(nullptr,0).status==SegmentStatus::NotInitialized);
    SegmenterConfig bad;bad.rms_threshold=0;assert(!invalid.reset(work.data(),work.size(),bad));
    bad={};bad.max_segment_samples=320001;assert(!invalid.reset(work.data(),work.size(),bad));
    bad={};bad.end_silence_samples=1;assert(!invalid.reset(work.data(),work.size(),bad));
    bad={};bad.forced_overlap_samples=320000;assert(!invalid.reset(work.data(),work.size(),bad));
    assert(!invalid.reset(nullptr,320000)&&!invalid.reset(work.data(),319999));
    assert(invalid.reset(work.data(),work.size()));assert(invalid.push(nullptr,1).status==SegmentStatus::InvalidArgument);
    assert(invalid.push(dc.data(),1).consumed==1);
    if(sizeof(size_t)==sizeof(uint64_t)) {
        const auto overflow=invalid.push(dc.data(),std::numeric_limits<size_t>::max());
        assert(overflow.consumed==0&&overflow.status==SegmentStatus::CounterOverflow&&invalid.samples_seen()==1);
    }
    SegmenterConfig extreme;extreme.pre_roll_samples=0;extreme.rms_threshold=32767;
    std::vector<int16_t> full_scale(3200);for(size_t i=0;i<full_scale.size();++i)full_scale[i]=i%2?32767:-32768;
    assert(run(full_scale,{61},extreme).size()==1);
    adaptive_tests();
    puts("PASS: deterministic chunking, pre-roll,8s pause floor,20s cuts, exact raw delivery, overlap, silence/DC, short-tail finish, reset and ready ownership");
}
