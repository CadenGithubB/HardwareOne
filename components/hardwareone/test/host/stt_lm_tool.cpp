// CLI over the production decoder (stt/stt_lm.cpp) and greedy CTC
// (stt/quartznet_frontend.cpp) for fixture parity and host benchmarks.
//   stt_lm_tool decode --lm F [--words F] --logits F.i8 --exponent E [--beam B]
//       prints one JSON object: status, greedy text, LM text, stats
//   stt_lm_tool bench [--frames N] [--beams 16,32] [--repeat R] [--lm F]
//       synthetic ~20k-word trigram LM (unless --lm) and synthetic logits
#include "stt_lm_testlib.h"
#include "stt/quartznet_frontend.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace lm=hw1::stt::lm;
namespace {
bool readFile(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream in(path,std::ios::binary);
    if(!in)return false;
    out.assign(std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>());
    return true;
}
std::string json(const std::string& s) {
    std::string o="\"";
    for(char c:s) { if(c=='"' || c=='\\')o.push_back('\\'); o.push_back(c); }
    return o+"\"";
}
const char* name(lm::DecodeStatus s) {
    switch(s) {
        case lm::DecodeStatus::Ok: return "ok";
        case lm::DecodeStatus::InvalidArgument: return "invalid_argument";
        case lm::DecodeStatus::OutOfMemory: return "out_of_memory";
        case lm::DecodeStatus::Cancelled: return "cancelled";
        case lm::DecodeStatus::OutputTooSmall: return "output_too_small";
    }
    return "unknown";
}
std::string arg(int argc, char** argv, const char* key, const char* fallback=nullptr) {
    for(int i=2;i+1<argc;++i)if(!std::strcmp(argv[i],key))return argv[i+1];
    if(!fallback) { std::fprintf(stderr,"missing %s\n",key); std::exit(2); }
    return fallback;
}
std::string greedy(const int8_t* q, size_t frames) {
    std::vector<char> text(8192);
    hw1::stt::CtcState state;
    if(hw1::stt::ctc_reset(&state,text.data(),text.size())!=hw1::stt::Status::Ok
       || hw1::stt::ctc_feed(&state,q,frames,29,text.data(),text.size())!=hw1::stt::Status::Ok
       || hw1::stt::ctc_finish(&state,text.data(),text.size())!=hw1::stt::Status::Ok)return "<greedy failed>";
    return text.data();
}
int decodeCommand(int argc, char** argv) {
    std::vector<uint8_t> blob, logits, wordsFile;
    if(!readFile(arg(argc,argv,"--lm"),blob) || !readFile(arg(argc,argv,"--logits"),logits)) { std::fprintf(stderr,"cannot read inputs\n"); return 2; }
    if(logits.size()%29) { std::fprintf(stderr,"logits size %zu is not a multiple of 29\n",logits.size()); return 2; }
    const int exponent=std::atoi(arg(argc,argv,"--exponent").c_str());
    const size_t beam=size_t(std::atoi(arg(argc,argv,"--beam","0").c_str()));
    lm::Lm model;
    const auto loaded=model.open(blob.data(),blob.size());
    const std::string wordsPath=arg(argc,argv,"--words","");
    auto words=std::make_unique<lm::CustomWords>();
    size_t listed=0;
    if(!wordsPath.empty()) {
        if(!readFile(wordsPath,wordsFile)) { std::fprintf(stderr,"cannot read %s\n",wordsPath.c_str()); return 2; }
        listed=words->parse(reinterpret_cast<const char*>(wordsFile.data()),wordsFile.size());
    }
    const auto q=reinterpret_cast<const int8_t*>(logits.data());
    const size_t frames=logits.size()/29;
    std::vector<char> text(8192);
    lm::DecodeStats stats;
    const auto start=std::chrono::steady_clock::now();
    const auto status=loaded==lm::LoadStatus::Ok
        ? lm::decode(q,frames,exponent,model,listed ? words.get() : nullptr,text.data(),text.size(),{},beam,&stats)
        : lm::DecodeStatus::InvalidArgument;
    const double us=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-start).count();
    std::printf("{\"load\":%s,\"status\":%s,\"frames\":%zu,\"beam\":%u,\"custom_words\":%zu,\"greedy\":%s,\"lm\":%s,"
                "\"nodes\":%u,\"candidates\":%u,\"completions\":%u,\"workspace_bytes\":%zu,\"decode_us\":%.1f}\n",
                json(lm::describe(loaded)).c_str(),json(name(status)).c_str(),frames,stats.beam,listed,
                json(greedy(q,frames)).c_str(),json(text.data()).c_str(),stats.nodes,stats.candidates,
                stats.completions,stats.workspaceBytes,us);
    return loaded==lm::LoadStatus::Ok && status==lm::DecodeStatus::Ok ? 0 : 1;
}

// ~20k words, 400k bigrams, 400k trigrams: binary-search depths of a
// meeting-sized pruned LM. Values are random; only cost is representative.
std::vector<uint8_t> syntheticLm(std::mt19937& rng, std::vector<std::string>& vocab) {
    lmtest::Spec s; s.alpha=0.8f; s.beta=1.5f; s.unk=-5; s.prune=-10; s.bonus=2; s.beam=16;
    std::uniform_int_distribution<int> len(2,9), letter(0,25), q(-7000,-1000), bo(-1000,0);
    s.uni("</s>",-1500).uni("<s>",-32768,-300).uni("<unk>",-6000,-500);
    while(s.unigrams.size()<20000) {
        std::string w; for(int i=len(rng);i>0;--i)w.push_back(char('a'+letter(rng)));
        s.uni(w,q(rng),bo(rng));
    }
    for(const auto& u:s.unigrams)vocab.push_back(u.first);
    std::uniform_int_distribution<size_t> pick(0,vocab.size()-1);
    while(s.bigrams.size()<400000)s.bi(vocab[pick(rng)],vocab[pick(rng)],q(rng)/2,bo(rng));
    while(s.trigrams.size()<400000)s.tri(vocab[pick(rng)],vocab[pick(rng)],vocab[pick(rng)],q(rng)/3);
    return lmtest::build(s);
}
// ~20 ms per output frame: a Zipf-ish word stream (15% OOV), 1-2 frames per
// symbol, blanks between, peaky frames with a 25% confusable second choice.
std::vector<int8_t> syntheticLogits(std::mt19937& rng, const std::vector<std::string>& vocab, size_t frames) {
    std::vector<int8_t> q;
    std::uniform_int_distribution<int> noise(-40,0), peak(28,44), gap(0,12), coin(0,99), sym(0,27), extra(1,2);
    auto frame=[&](int c) {
        const size_t at=q.size(); q.resize(at+29);
        for(int k=0;k<29;++k)q[at+k]=int8_t(noise(rng));
        const int top=peak(rng); q[at+c]=int8_t(top);
        if(coin(rng)<25) { const int other=sym(rng); if(other!=c)q[at+other]=int8_t(top-gap(rng)); }
    };
    std::uniform_real_distribution<double> u(0,1);
    while(q.size()<frames*29) {
        std::string w;
        if(coin(rng)<15) { for(int i=2+coin(rng)%6;i>0;--i)w.push_back(char('a'+coin(rng)%26)); }
        else w=vocab[size_t(std::pow(u(rng),3.0)*double(vocab.size()-4))+3];
        for(char ch:w) { for(int i=extra(rng);i>0;--i)frame(lmtest::cls(ch)); for(int i=extra(rng);i>0;--i)frame(28); }
        for(int i=extra(rng);i>0;--i)frame(0);
        for(int i=extra(rng);i>0;--i)frame(28);
    }
    q.resize(frames*29);
    return q;
}
int benchCommand(int argc, char** argv) {
    const size_t frames=size_t(std::atoi(arg(argc,argv,"--frames","1000").c_str()));
    const int repeat=std::max(1,std::atoi(arg(argc,argv,"--repeat","5").c_str()));
    std::vector<size_t> beams;
    { std::stringstream in(arg(argc,argv,"--beams","16,32")); std::string item; while(std::getline(in,item,','))beams.push_back(size_t(std::atoi(item.c_str()))); }
    std::mt19937 rng(20260929);
    std::vector<std::string> vocab; std::vector<uint8_t> blob;
    const std::string path=arg(argc,argv,"--lm","");
    if(path.empty())blob=syntheticLm(rng,vocab);
    else if(!readFile(path,blob)) { std::fprintf(stderr,"cannot read %s\n",path.c_str()); return 2; }
    lm::Lm model;
    auto start=std::chrono::steady_clock::now();
    const auto loaded=model.open(blob.data(),blob.size());
    const double openMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    if(loaded!=lm::LoadStatus::Ok) { std::fprintf(stderr,"LM rejected: %s\n",lm::describe(loaded)); return 1; }
    if(vocab.empty())for(uint32_t i=0;i<model.vocab();++i)vocab.push_back(model.word(uint16_t(i)));
    const auto q=syntheticLogits(rng,vocab,frames);
    std::printf("LM %zu bytes: V=%u bigrams=%u trigrams=%u prune=%.1f; open+validate+SHA %.1f ms\n",
                blob.size(),model.vocab(),model.bigrams(),model.trigrams(),double(model.charPrune()),openMs);
    std::printf("input: %zu frames (%.1f s of audio); greedy: \"%.60s...\"\n",frames,double(frames)*0.02,greedy(q.data(),frames).c_str());
    for(size_t beam:beams) {
        double best=1e30; lm::DecodeStats stats; std::vector<char> text(8192);
        for(int r=0;r<repeat;++r) {
            start=std::chrono::steady_clock::now();
            const auto status=lm::decode(q.data(),frames,-2,model,nullptr,text.data(),text.size(),{},beam,&stats);
            best=std::min(best,std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-start).count());
            if(status!=lm::DecodeStatus::Ok) { std::fprintf(stderr,"decode failed: %s\n",name(status)); return 1; }
        }
        std::printf("beam %2zu: %8.1f us total, %6.2f us/frame | %5.1f candidates/frame, %u LM completions, %u nodes,"
                    " workspace %zu KiB (1500 frames: %zu KiB) | \"%.40s...\"\n",
                    beam,best,best/double(frames),double(stats.candidates)/double(frames),stats.completions,stats.nodes,
                    stats.workspaceBytes/1024,lm::workspace_bytes(1500,beam)/1024,text.data());
    }
    return 0;
}
}

int main(int argc, char** argv) {
    if(argc>=2 && !std::strcmp(argv[1],"decode"))return decodeCommand(argc,argv);
    if(argc>=2 && !std::strcmp(argv[1],"bench"))return benchCommand(argc,argv);
    std::fprintf(stderr,"usage: %s decode --lm F [--words F] --logits F --exponent E [--beam B]\n"
                        "       %s bench [--frames N] [--beams 16,32] [--repeat R] [--lm F]\n",argv[0],argv[0]);
    return 2;
}
