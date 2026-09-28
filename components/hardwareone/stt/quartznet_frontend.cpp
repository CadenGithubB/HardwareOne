#include "quartznet_frontend.h"
#include "quartznet_constants.h"
#include <cmath>
#include <cstring>
namespace hw1::stt {
namespace {
constexpr float kPreemphasis=0.97f, kDither=1e-5f, kLogGuard=0x1p-24f;
struct Gaussian {
    uint32_t state;
    float spare=0;
    bool has_spare=false;
    uint32_t next() { state^=state<<13; state^=state>>17; state^=state<<5; return state; }
    float uniform() { return static_cast<float>((static_cast<double>((next()>>8)+1))/16777217.0); }
    float sample() {
        if (has_spare) { has_spare=false; return spare; }
        const float u1=uniform(), u2=uniform();
        const float radius=std::sqrt(-2.0f*std::log(u1));
        const float angle=0x1.921fb6p+2f*u2;
        spare=radius*std::sin(angle); has_spare=true;
        return radius*std::cos(angle);
    }
};
void fft(FrontendWorkspace& w) {
    for (size_t i=0;i<512;++i) {
        const size_t j=constants::bit_reverse[i];
        if (j>i) { const float temp=w.real[i]; w.real[i]=w.real[j]; w.real[j]=temp; }
    }
    for (size_t width=2;width<=512;width*=2) {
        const size_t half=width/2, step=512/width;
        for (size_t base=0;base<512;base+=width) for (size_t j=0;j<half;++j) {
            const size_t a=base+j,b=a+half,t=j*step;
            const float tr=constants::twiddle_real[t]*w.real[b]-constants::twiddle_imag[t]*w.imag[b];
            const float ti=constants::twiddle_real[t]*w.imag[b]+constants::twiddle_imag[t]*w.real[b];
            const float ar=w.real[a],ai=w.imag[a];
            w.real[a]=ar+tr; w.imag[a]=ai+ti;
            w.real[b]=ar-tr; w.imag[b]=ai-ti;
        }
    }
}
size_t reflect(int index, size_t samples) {
    if (index<0) return static_cast<size_t>(-index);
    if (static_cast<size_t>(index)>=samples) return 2*samples-2-static_cast<size_t>(index);
    return static_cast<size_t>(index);
}
bool valid_score_extent(size_t frames,size_t stride) {
    // The caller still owns the score allocation; reject arithmetic overflow before reads.
    return frames<=kMaxSamples/160 && stride>=kCtcClasses &&
        (frames==0 || stride<=(SIZE_MAX-kCtcClasses)/(frames>1?frames-1:1));
}
constexpr char kVocabulary[]=" abcdefghijklmnopqrstuvwxyz'";
template<typename Score>
Status feed(CtcState* state,const Score* scores,size_t frames,size_t stride,char* text,size_t capacity) {
    if (!state||!text||!capacity||(!scores&&frames)||!valid_score_extent(frames,stride)||
        state->frames>kMaxSamples/160||frames>kMaxSamples/160-state->frames||
        state->written>=capacity||text[state->written]!='\0') return Status::InvalidArgument;
    if (state->finished) return Status::AlreadyFinished;
    for (size_t frame=0;frame<frames;++frame) {
        const Score* row=scores+frame*stride;
        size_t best=0;
        for (size_t i=1;i<kCtcClasses;++i) if (row[i]>row[best]) best=i;
        if (best!=kCtcBlank&&static_cast<int>(best)!=state->previous) {
            const char value=kVocabulary[best];
            if (!(state->written==0&&value==' ')) {
                if (state->written+1<capacity&&!state->truncated) {
                    text[state->written++]=value; text[state->written]='\0';
                } else state->truncated=true;
            }
        }
        state->previous=static_cast<int>(best); ++state->frames;
    }
    return state->truncated?Status::OutputTooSmall:Status::Ok;
}
}
size_t feature_frames(size_t samples) { return samples>=kMinSamples&&samples<=kMaxSamples?(samples+159)/160:0; }
Status compute_features(const int16_t* pcm,size_t samples,float* output,size_t capacity,
                        FrontendWorkspace* workspace,uint32_t seed,bool dither) {
    const size_t frames=feature_frames(samples);
    if (!pcm||!output||!workspace||!frames||(dither&&!seed)) return Status::InvalidArgument;
    if (capacity<frames*kMelBins) return Status::OutputTooSmall;
    auto& w=*workspace;
    Gaussian rng{seed};
    size_t generated=0;
    float previous=0;
    for (size_t frame=0;frame<frames;++frame) {
        const size_t center=frame*160;
        size_t required=center+159;
        if (required<160) required=160;
        if (required>=samples) required=samples-1;
        while (generated<=required) {
            float sample=static_cast<float>(pcm[generated])*(1.0f/32768.0f);
            if (dither) sample+=kDither*rng.sample();
            w.conditioned[generated%512]=generated?sample-kPreemphasis*previous:sample;
            previous=sample; ++generated;
        }
        std::memset(w.real,0,sizeof(w.real)); std::memset(w.imag,0,sizeof(w.imag));
        for (size_t k=0;k<320;++k) {
            const size_t input=reflect(static_cast<int>(center+k)-160,samples);
            w.real[96+k]=w.conditioned[input%512]*constants::window[k];
        }
        fft(w);
        for (size_t k=0;k<257;++k) w.power[k]=w.real[k]*w.real[k]+w.imag[k]*w.imag[k];
        for (size_t m=0;m<kMelBins;++m) {
            float value=0;
            for (size_t j=0;j<constants::mel_count[m];++j)
                value+=constants::mel_weight[constants::mel_offset[m]+j]*w.power[constants::mel_start[m]+j];
            output[frame*kMelBins+m]=std::log(value+kLogGuard);
        }
    }
    for (size_t m=0;m<kMelBins;++m) {
        double sum=0;
        for (size_t t=0;t<frames;++t) sum+=output[t*kMelBins+m];
        const double exact_mean=sum/frames;
        const float mean=static_cast<float>(exact_mean);
        double variance=0;
        for (size_t t=0;t<frames;++t) {
            const double d=static_cast<double>(output[t*kMelBins+m])-exact_mean;
            variance+=d*d;
        }
        const float stddev=std::sqrt(static_cast<float>(variance/(frames-1)))+1e-5f;
        w.means[m]=mean; w.variances[m]=stddev;
        for (size_t t=0;t<frames;++t) output[t*kMelBins+m]=(output[t*kMelBins+m]-mean)/stddev;
    }
    return Status::Ok;
}
Status ctc_reset(CtcState* state,char* text,size_t capacity) {
    if (!state||!text||!capacity) return Status::InvalidArgument;
    *state=CtcState{}; text[0]='\0'; return Status::Ok;
}
Status ctc_feed(CtcState* state,const float* scores,size_t frames,size_t stride,char* text,size_t capacity) {
    if (!state||!text||!capacity||(!scores&&frames)||!valid_score_extent(frames,stride)||
        state->frames>kMaxSamples/160||frames>kMaxSamples/160-state->frames) return Status::InvalidArgument;
    for (size_t t=0;t<frames;++t) for(size_t c=0;c<kCtcClasses;++c)
        if (!std::isfinite(scores[t*stride+c])) return Status::NonFinite;
    return feed(state,scores,frames,stride,text,capacity);
}
Status ctc_feed(CtcState* state,const int8_t* scores,size_t frames,size_t stride,char* text,size_t capacity) {
    return feed(state,scores,frames,stride,text,capacity);
}
Status ctc_finish(CtcState* state,char* text,size_t capacity) {
    if (!state||!text||!capacity||state->written>=capacity||text[state->written]!='\0') return Status::InvalidArgument;
    if (state->finished) return Status::AlreadyFinished;
    while(state->written&&text[state->written-1]==' ') --state->written;
    text[state->written]='\0'; state->finished=true;
    return state->truncated?Status::OutputTooSmall:Status::Ok;
}
}
