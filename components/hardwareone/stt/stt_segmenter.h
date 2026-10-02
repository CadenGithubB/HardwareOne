#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include "../Audio_VadPolicy.h"

namespace hw1::stt {
inline constexpr size_t kSegmentSampleRate=16000;
inline constexpr size_t kSegmentAnalysisSamples=160; // 10 ms, independent of push chunk size.
inline constexpr size_t kSegmentMaximumSamples=20*kSegmentSampleRate;
inline constexpr size_t kSegmentNoiseWindowFrames=500; // At most five seconds of 10 ms levels.

// Simple fixed/adaptive energy endpointing, not a neural voice-activity detector. All
// durations are samples and must be multiples of the 10 ms analysis frame.
struct SegmenterConfig {
    size_t max_segment_samples=kSegmentMaximumSamples;
    size_t min_segment_samples=8*kSegmentSampleRate;
    size_t pre_roll_samples=4800;
    size_t end_silence_samples=9600;
    size_t min_voice_samples=3200;
    size_t forced_overlap_samples=0;
    uint16_t rms_threshold=400; // Fixed mode, DC-removed RMS in native int16 PCM units.
    bool adaptive=false; // Opt in per consumer; existing fixed-mode behavior is unchanged.
    size_t calibration_samples=8000; // Initial 500 ms is ambient only, never admitted as speech.
    size_t noise_window_frames=kSegmentNoiseWindowFrames;
    // Raw-RMS defaults separate measured P4 room noise (3..31) from speech.
    // Callers may tune these for other microphones or acoustic conditions.
    uint16_t adaptive_speech_floor=45;
    uint16_t adaptive_silence_floor=16;
};
enum class SegmentEnd : uint8_t { Pause, HardLimit, SessionEnd };
enum class SegmentStatus : uint8_t { Ok, Ready, Finished, InvalidArgument, NotInitialized, CounterOverflow };
struct SegmentPushResult { size_t consumed; SegmentStatus status; };
struct SpeechSegment {
    const int16_t* pcm=nullptr;
    size_t samples=0;
    uint64_t start_sample=0, end_sample=0; // Absolute session offsets, end exclusive.
    uint64_t fresh_start_sample=0; // Samples before this offset are explicit overlap.
    size_t overlap_before_samples=0;
    size_t next_overlap_samples=0; // Planned context if a hard-cut continuation is emitted.
    size_t new_voiced_samples=0; // Classified voiced samples, excluding retained overlap.
    SegmentEnd end=SegmentEnd::Pause;
    bool continuation=false;
};

// Single-owner, no heap or platform dependencies. Buffer must remain valid and
// have at least config.max_segment_samples elements. Input must not alias it.
// push stops at one Ready result; only `consumed` samples belong to this call.
// Copy/queue segment()->pcm before release(), then push the unconsumed suffix.
// A ready segment is immutable until release/reset. Idle/noise gaps are skipped
// deliberately; absolute offsets identify them. Raw PCM is never filtered.
class Segmenter {
public:
    bool reset(int16_t* buffer,size_t capacity,const SegmenterConfig& config=SegmenterConfig{}) {
        if(!buffer || !valid(config) || capacity<config.max_segment_samples)return false;
        buffer_=buffer;config_=config;initialized_=true;reset_session();return true;
    }
    void reset_session() {
        seen_=start_=fresh_start_=0;used_=head_=history_=voice_=silence_=0;
        frame_count_=0;frame_sum_=0;frame_square_=0;
        active_=continuation_=ready_=finishing_=finished_=false;segment_={};
        calibrated_=!config_.adaptive;heard_speech_=false;calibration_seen_=0;
        noise_head_=noise_count_=0;current_rms_=noise_rms_=peak_rms_=0;
        threshold_rms_=config_.adaptive?config_.adaptive_speech_floor:config_.rms_threshold;
        std::memset(noise_window_,0,sizeof(noise_window_));
    }
    uint64_t samples_seen() const { return seen_; }
    bool calibrated() const { return initialized_ && calibrated_; }
    uint16_t current_rms() const { return current_rms_; }
    uint16_t noise_rms() const { return noise_rms_; }
    uint16_t threshold_rms() const { return threshold_rms_; }
    uint16_t peak_rms() const { return peak_rms_; }
    const SpeechSegment* segment() const { return ready_?&segment_:nullptr; }
    // Live view of the utterance still being captured (pre-roll included),
    // for draft transcription. Valid only on the pushing thread, until the
    // next push/release/reset. False while idle or before enough voice.
    bool in_progress(const int16_t** pcm,size_t* samples) const {
        if(!initialized_||!active_||ready_||!eligible()||!used_)return false;
        *pcm=buffer_;*samples=used_;return true;
    }
    SegmentPushResult push(const int16_t* pcm,size_t count) {
        if(!initialized_)return {0,SegmentStatus::NotInitialized};
        if(ready_)return {0,SegmentStatus::Ready};
        if(finished_ || finishing_)return {0,SegmentStatus::Finished};
        if(!pcm && count)return {0,SegmentStatus::InvalidArgument};
        if(count>std::numeric_limits<uint64_t>::max()-seen_)return {0,SegmentStatus::CounterOverflow};
        size_t consumed=0;
        while(consumed<count) {
            const int16_t sample=pcm[consumed++];
            // Calibration advances absolute positions but cannot become pre-roll
            // or model input, even if it contains loud audio or ends mid-push.
            if(calibrated_) {
                if(active_)buffer_[used_++]=sample;
                else append_history(sample);
            }
            ++seen_;++frame_count_;frame_sum_+=sample;
            frame_square_+=static_cast<uint64_t>(static_cast<int64_t>(sample)*sample);
            if(frame_count_==kSegmentAnalysisSamples) {
                evaluate_frame(false);clear_frame();
                if(ready_)return {consumed,SegmentStatus::Ready};
            }
        }
        return {consumed,SegmentStatus::Ok};
    }
    // End the session. Flush qualified voiced tails even below the minimum
    // wall span; no empty/silence-only continuation is emitted. Calling finish
    // while a segment is ready keeps that view and seals the session on release.
    SegmentStatus finish() {
        if(!initialized_)return SegmentStatus::NotInitialized;
        finishing_=true;
        if(ready_)return SegmentStatus::Ready;
        if(finished_)return SegmentStatus::Finished;
        if(frame_count_) { evaluate_frame(true);clear_frame(); }
        if(active_ && eligible()) { publish(SegmentEnd::SessionEnd);return SegmentStatus::Ready; }
        finished_=true;active_=false;return SegmentStatus::Finished;
    }
    bool release() {
        if(!ready_)return false;
        ready_=false;
        if(finishing_) { active_=false;finished_=true;return true; }
        if(segment_.end==SegmentEnd::HardLimit) {
            const size_t keep=config_.forced_overlap_samples;
            if(keep)std::memmove(buffer_,buffer_+used_-keep,keep*sizeof(int16_t));
            used_=keep;start_=seen_-keep;fresh_start_=seen_;
            voice_=0;active_=true;continuation_=true;
        } else {
            // Already-delivered trailing silence must not become a new segment's
            // pre-roll: the default zero-overlap mode is exactly nonoverlapping.
            used_=head_=history_=voice_=silence_=0;active_=continuation_=false;
            reset_utterance_levels();
        }
        segment_={};return true;
    }
private:
    static bool aligned(size_t n) { return n%kSegmentAnalysisSamples==0; }
    static bool valid(const SegmenterConfig& c) {
        return c.max_segment_samples>=2*kSegmentAnalysisSamples && c.max_segment_samples<=kSegmentMaximumSamples
            && c.min_segment_samples>0 && c.min_segment_samples<=c.max_segment_samples
            && c.pre_roll_samples<=c.max_segment_samples-kSegmentAnalysisSamples
            && c.end_silence_samples>0 && c.end_silence_samples<=c.max_segment_samples
            && c.min_voice_samples>0 && c.min_voice_samples<=c.max_segment_samples-c.pre_roll_samples
            && c.forced_overlap_samples<c.max_segment_samples && c.rms_threshold>0 && c.rms_threshold<=32767
            && aligned(c.max_segment_samples) && aligned(c.min_segment_samples) && aligned(c.pre_roll_samples)
            && aligned(c.end_silence_samples) && aligned(c.min_voice_samples) && aligned(c.forced_overlap_samples)
            && (!c.adaptive || (c.noise_window_frames>0 && c.noise_window_frames<=kSegmentNoiseWindowFrames
                && c.calibration_samples>=kSegmentAnalysisSamples
                && c.calibration_samples<=kSegmentNoiseWindowFrames*kSegmentAnalysisSamples && aligned(c.calibration_samples)
                && c.adaptive_speech_floor>0 && c.adaptive_speech_floor<=32767
                && c.adaptive_silence_floor>0 && c.adaptive_silence_floor<=c.adaptive_speech_floor));
    }
    size_t history_capacity() const { return config_.pre_roll_samples+kSegmentAnalysisSamples; }
    void append_history(int16_t sample) {
        const size_t capacity=history_capacity();
        if(history_==capacity) { buffer_[head_]=sample;head_=(head_+1)%capacity; }
        else { buffer_[(head_+history_)%capacity]=sample;++history_; }
    }
    void reverse(size_t begin,size_t end) {
        while(begin<end) { --end;if(begin>=end)break;int16_t v=buffer_[begin];buffer_[begin++]=buffer_[end];buffer_[end]=v; }
    }
    void begin_segment() {
        // A full ring contains pre-roll plus the triggering analysis frame.
        if(head_) { reverse(0,head_);reverse(head_,history_);reverse(0,history_); }
        used_=history_;start_=seen_-used_;fresh_start_=start_;
        head_=history_=voice_=silence_=0;active_=true;continuation_=false;
    }
    bool eligible() const { return voice_>0 && (continuation_ || voice_>=config_.min_voice_samples); }
    void clear_frame() { frame_count_=0;frame_sum_=0;frame_square_=0; }
    void evaluate_frame(bool finalizing) {
        const uint64_t n=frame_count_;
        const uint64_t centered=frame_square_*n-static_cast<uint64_t>(frame_sum_*frame_sum_);
        // Integer floor(sqrt(variance)) is reproducible across hosts and chips;
        // the fixed-mode comparison below retains its original exact arithmetic.
        current_rms_=integer_sqrt(static_cast<uint32_t>(centered/(n*n)));
        bool voiced=false, silent=false;
        if(config_.adaptive) {
            const int32_t floor_before=noise_count_?noise_rms_:-1;
            const uint16_t window_min=track_noise(current_rms_);
            if(!calibrated_) {
                noise_rms_=window_min;calibration_seen_+=frame_count_;
                if(calibration_seen_>=config_.calibration_samples)calibrated_=true;
                threshold_rms_=onset_threshold();
                return;
            }
            // A sustained voice can fill the entire window. Do not learn it as
            // its own ambient floor after five seconds; retain the utterance's
            // seed while permitting quieter evidence to lower it. Natural
            // endpoints resume upward adaptation. A noise jump that resembles
            // speech is inherently ambiguous to this energy-only detector.
            if(!active_ || window_min<noise_rms_)noise_rms_=window_min;
            // No utterance exists while idle: an old ambient peak must not
            // prevent a quiet voice after the room/source becomes quieter.
            if(!active_ || current_rms_>peak_rms_)peak_rms_=current_rms_;
            const auto decision=hw1::audio::adaptiveVadDecision(current_rms_,floor_before,noise_rms_,peak_rms_,
                heard_speech_,{config_.adaptive_speech_floor,config_.adaptive_silence_floor});
            threshold_rms_=static_cast<uint16_t>(heard_speech_?decision.stopCut:
                (decision.stopCut>config_.adaptive_speech_floor?decision.stopCut:config_.adaptive_speech_floor));
            heard_speech_=decision.heardSpeech;
            // Peak-relative stop timing must never remove quiet word tails.
            // All admitted raw samples stay in the segment; floor-relative
            // evidence counts toward minimum voice duration independently.
            voiced=heard_speech_ && !decision.trimSilent;
            silent=decision.stopSilent;
        } else {
            if(current_rms_>peak_rms_)peak_rms_=current_rms_;
            const uint64_t threshold=uint64_t(config_.rms_threshold)*config_.rms_threshold*n*n;
            voiced=centered>=threshold;silent=!voiced;
        }
        if(!active_ && voiced)begin_segment();
        if(!active_)return;
        if(voiced)voice_+=frame_count_;
        if(silent)silence_+=frame_count_;else silence_=0;
        if(finalizing)return;
        const bool pause=used_>=config_.min_segment_samples && silence_>=config_.end_silence_samples;
        if(pause || used_>=config_.max_segment_samples) {
            if(eligible())publish(pause?SegmentEnd::Pause:SegmentEnd::HardLimit);
            else discard_candidate();
        }
    }
    static uint16_t integer_sqrt(uint32_t value) {
        uint32_t result=0,bit=uint32_t(1)<<30;
        while(bit>value)bit>>=2;
        while(bit) {
            if(value>=result+bit) { value-=result+bit;result=(result>>1)+bit; }
            else result>>=1;
            bit>>=2;
        }
        return static_cast<uint16_t>(result);
    }
    uint16_t track_noise(uint16_t level) {
        noise_window_[noise_head_]=level;
        noise_head_=(noise_head_+1)%config_.noise_window_frames;
        if(noise_count_<config_.noise_window_frames)++noise_count_;
        uint16_t minimum=noise_window_[0];
        for(size_t i=1;i<noise_count_;++i)if(noise_window_[i]<minimum)minimum=noise_window_[i];
        return minimum;
    }
    uint16_t onset_threshold() const {
        uint16_t threshold=static_cast<uint16_t>(2*noise_rms_);
        if(threshold<config_.adaptive_speech_floor)threshold=config_.adaptive_speech_floor;
        return threshold;
    }
    void reset_utterance_levels() {
        heard_speech_=false;peak_rms_=0;
        if(config_.adaptive) {
            if(noise_count_) {
                noise_rms_=noise_window_[0];
                for(size_t i=1;i<noise_count_;++i)if(noise_window_[i]<noise_rms_)noise_rms_=noise_window_[i];
            }
            threshold_rms_=onset_threshold();
        }
    }
    void publish(SegmentEnd end) {
        segment_={buffer_,used_,start_,seen_,fresh_start_,static_cast<size_t>(fresh_start_-start_),
                  end==SegmentEnd::HardLimit?config_.forced_overlap_samples:0,voice_,end,continuation_};
        ready_=true;
    }
    void discard_candidate() {
        const size_t fresh=static_cast<size_t>(seen_-fresh_start_);
        size_t keep=used_<history_capacity()?used_:history_capacity();
        // A rejected forced continuation may contain retained, already-delivered
        // context. Only fresh samples may seed a subsequent ordinary pre-roll.
        if(keep>fresh)keep=fresh;
        if(keep)std::memmove(buffer_,buffer_+used_-keep,keep*sizeof(int16_t));
        history_=keep;head_=used_=voice_=silence_=0;active_=continuation_=false;
        reset_utterance_levels();
    }
    int16_t* buffer_=nullptr;
    SegmenterConfig config_{};
    SpeechSegment segment_{};
    uint64_t seen_=0,start_=0,fresh_start_=0;
    size_t used_=0,head_=0,history_=0,voice_=0,silence_=0;
    int64_t frame_sum_=0;
    uint64_t frame_square_=0;
    uint16_t frame_count_=0;
    uint16_t noise_window_[kSegmentNoiseWindowFrames]{};
    size_t noise_head_=0,noise_count_=0,calibration_seen_=0;
    uint16_t current_rms_=0,noise_rms_=0,threshold_rms_=0,peak_rms_=0;
    bool calibrated_=false,heard_speech_=false;
    bool initialized_=false,active_=false,continuation_=false,ready_=false,finishing_=false,finished_=false;
};
} // namespace hw1::stt
