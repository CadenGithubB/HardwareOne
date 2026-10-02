#include "stt_lm.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#endif
namespace hw1::stt::lm {
namespace {
constexpr size_t kHeaderBytes=64, kShaBytes=32, kLocal=128, kYieldFrames=32;
constexpr uint32_t kEmpty=0xffffffffu;
constexpr double kNegInf=-std::numeric_limits<double>::infinity();
constexpr double kLn10=2.302585092994045684017991454684364208;
constexpr char kVocab[]=" abcdefghijklmnopqrstuvwxyz'";
static_assert(sizeof(kVocab)==kClasses && kBlank==kClasses-1 && 2*kMaxBeam<=kLocal);
uint16_t u16(const uint8_t* p) { return uint16_t(p[0]|p[1]<<8); }
int32_t i16(const uint8_t* p) { return int16_t(u16(p)); }
uint32_t u32(const uint8_t* p) { return uint32_t(p[0])|uint32_t(p[1])<<8|uint32_t(p[2])<<16|uint32_t(p[3])<<24; }
float f32(const uint8_t* p) { const uint32_t v=u32(p); float f; std::memcpy(&f,&v,4); return f; }
uint64_t align4(uint64_t v) { return (v+3)&~uint64_t(3); }
// Byte order of a stored NUL-terminated word against a length-bounded key.
int compareWord(const char* stored, const char* key, size_t length) {
    for(size_t i=0;i<length;++i) {
        const uint8_t a=uint8_t(stored[i]), b=uint8_t(key[i]);
        if(a!=b)return a<b?-1:1;
        if(!a)return -1;
    }
    return stored[length]?1:0;
}
#if !defined(ESP_PLATFORM)
constexpr uint32_t kRound[64]={
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
uint32_t ror(uint32_t x, int n) { return x>>n|x<<(32-n); }
void shaBlock(uint32_t* h, const uint8_t* p) {
    uint32_t w[64];
    for(int i=0;i<16;++i)w[i]=uint32_t(p[4*i])<<24|uint32_t(p[4*i+1])<<16|uint32_t(p[4*i+2])<<8|p[4*i+3];
    for(int i=16;i<64;++i)w[i]=w[i-16]+(ror(w[i-15],7)^ror(w[i-15],18)^(w[i-15]>>3))+w[i-7]
                                   +(ror(w[i-2],17)^ror(w[i-2],19)^(w[i-2]>>10));
    uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],k=h[7];
    for(int i=0;i<64;++i) {
        const uint32_t t1=k+(ror(e,6)^ror(e,11)^ror(e,25))+((e&f)^(~e&g))+kRound[i]+w[i];
        const uint32_t t2=(ror(a,2)^ror(a,13)^ror(a,22))+((a&b)^(a&c)^(b&c));
        k=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
    }
    h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=k;
}
#endif
void* allocate(size_t bytes) {
#if defined(ESP_PLATFORM)
    return heap_caps_aligned_alloc(16,bytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
#else
    return ::operator new(bytes,std::nothrow);
#endif
}
void release(void* p) {
#if defined(ESP_PLATFORM)
    heap_caps_free(p);
#else
    ::operator delete(p);
#endif
}
void yieldTask() {
#if defined(ESP_PLATFORM)
    vTaskDelay(1);
#endif
}
// common.py::normalize_text over one line as a streaming transducer: lower,
// '-' and non-alphabet to space, then the leftmost non-overlapping regex
// "\s+'|'\s+" -> ' ' on the space-padded line, then collapse/strip. finish()
// accepts exactly one resulting word of 1..31 bytes.
class LineWord {
 public:
    void feed(char c) { // c is a-z, apostrophe or space
        switch(state_) {
            case Normal: if(c==' ')state_=Run; else if(c=='\'')state_=Apos; else emit(c); break;
            case Run: if(c==' ')break; emit(' '); if(c!='\'')emit(c); state_=Normal; break; // run+' matched
            case Apos:
                if(c==' ') { emit(' '); state_=Skip; }            // '+run matched
                else if(c=='\'')emit('\'');
                else { emit('\''); emit(c); state_=Normal; }
                break;
            case Skip: if(c==' ')break; if(c=='\'')state_=Apos; else { emit(c); state_=Normal; } break;
        }
    }
    bool finish(char* out, size_t& length) {
        feed(' ');
        std::memcpy(out,word_,length_); out[length_]=0; length=length_;
        return words_==1 && !overflow_ && length_;
    }
 private:
    void emit(char c) {
        if(c==' ') { inWord_=false; return; }
        if(!inWord_) { inWord_=true; ++words_; }
        if(words_!=1)return;
        if(length_<kMaxCustomWordBytes)word_[length_++]=c; else overflow_=true;
    }
    enum State : uint8_t { Normal, Run, Apos, Skip } state_=Run; // leading pad space
    char word_[kMaxCustomWordBytes];
    size_t length_=0, words_=0;
    bool inWord_=false, overflow_=false;
};
// One UTF-8 code point (invalid bytes decode one U+FFFD each, as Python's
// 'replace' does up to repetition, which normalisation makes irrelevant).
size_t utf8(const uint8_t* p, size_t n, uint32_t& cp) {
    const uint8_t b=p[0];
    const size_t length=b<0x80 ? 1 : b>=0xc2 && b<=0xdf ? 2 : b>=0xe0 && b<=0xef ? 3 : b>=0xf0 && b<=0xf4 ? 4 : 0;
    cp=0xfffd;
    if(length==1) { cp=b; return 1; }
    if(!length || length>n)return 1;
    uint32_t v=b&(0x7f>>length);
    for(size_t i=1;i<length;++i) { if((p[i]&0xc0)!=0x80)return 1; v=v<<6|(p[i]&0x3f); }
    constexpr uint32_t kMinimum[5]={0,0,0x80,0x800,0x10000};
    if(v<kMinimum[length] || v>0x10ffff || (v>=0xd800 && v<=0xdfff))return 1;
    cp=v; return length;
}
// Python str.splitlines() boundaries.
bool lineBreak(uint32_t cp) {
    return (cp>=0x0a && cp<=0x0d) || (cp>=0x1c && cp<=0x1e) || cp==0x85 || cp==0x2028 || cp==0x2029;
}
// str.lower() then [^a-z'] -> space. U+0130 and U+212A are the only code
// points whose lowercase contains an ASCII letter.
void feedLower(LineWord& line, uint32_t cp) {
    if(cp>='A' && cp<='Z')cp+=32;
    if((cp>='a' && cp<='z') || cp=='\'')line.feed(char(cp));
    else if(cp==0x130) { line.feed('i'); line.feed(' '); }   // 'i' + U+0307
    else if(cp==0x212a)line.feed('k');                        // KELVIN SIGN
    else line.feed(' ');
}
// Scores accumulate in double so tiny log-sum corrections (e.g. an exp(-21)
// alternative alignment) still order prefixes as a float64 reference does;
// only the correction term uses single-precision exp/log1p (P4 has no double
// FPU). Exact ties therefore arise from identical arithmetic, as in the spec.
double lse(double a, double b) {
    if(a<b)std::swap(a,b);
    return b==kNegInf ? a : a+double(std::log1p(std::exp(float(b-a))));
}
size_t pow2(size_t n) { size_t p=1; while(p<n)p<<=1; return p; }
struct Node {
    double lm, spaceLm;                    // spaceLm: after completing this node's partial word
    uint32_t parent, jump;                 // jump: skew-binary ancestor, O(log depth) LCA
    uint16_t ctx2, ctx1, spaceWord, depth; // context (w-2, w-1); id pushed by that completion
    uint8_t ch, wordLen, spaceReady, pad;  // wordLen saturates at 255 (then OOV, not listed)
};
struct Beam { double pb, pnb; uint32_t node, pad; };
// key = float(score): rounding is monotonic, so unequal keys order exactly
// like the doubles and most heap comparisons avoid soft-float on the P4.
struct Cand { double pb, pnb, score; uint32_t node, parent; float key; uint8_t ch, fresh, pad[2]; };
static_assert(sizeof(Node)==40 && sizeof(Beam)==24 && sizeof(Cand)==40);
struct Layout { size_t nodes, table, tableSize, beams, cands, heap, local, slots, exps, bytes, maxNodes; };
bool layout(size_t frames, size_t beam, Layout& l) {
    if(frames>kMaxFrames || !beam || beam>kMaxBeam)return false;
    l.maxNodes=frames*beam+1; // <= beam new prefixes per frame, plus the root
    l.tableSize=pow2(2*l.maxNodes);
    size_t at=0;
    auto take=[&](size_t bytes) { const size_t start=(at+15)&~size_t(15); at=start+bytes; return start; };
    l.nodes=take(sizeof(Node)*l.maxNodes); l.table=take(4*l.tableSize);
    l.beams=take(sizeof(Beam)*beam); l.cands=take(sizeof(Cand)*beam*kClasses);
    l.heap=take(2*beam); l.local=take(4*kLocal); l.slots=take(kLocal); l.exps=take(sizeof(double)*256);
    l.bytes=at; return true;
}
struct Cursor { uint32_t node; int extra; }; // extra>=0: one virtual child char
class Search {
 public:
    Search(const Lm& lm, const CustomWords* words, uint8_t* w, const Layout& l, size_t beam)
        : lm_(lm), words_(words && words->size() ? words : nullptr), alphaLn_(double(lm.alpha())*kLn10),
          nodes_(reinterpret_cast<Node*>(w+l.nodes)), table_(reinterpret_cast<uint32_t*>(w+l.table)),
          beams_(reinterpret_cast<Beam*>(w+l.beams)), cands_(reinterpret_cast<Cand*>(w+l.cands)),
          heap_(reinterpret_cast<uint16_t*>(w+l.heap)), local_(reinterpret_cast<uint32_t*>(w+l.local)),
          slots_(w+l.slots), exps_(reinterpret_cast<double*>(w+l.exps)),
          mask_(l.tableSize-1), width_(beam) {
        std::fill(table_,table_+l.tableSize,kEmpty);
        // Root: no character (ch is never compared), context (none, <s>).
        nodes_[0]=Node{0,0,kEmpty,0,kNoWord,lm.bos(),0,0,uint8_t(kBlank),0,0,0};
        count_=1; beams_[0]=Beam{0,kNegInf,0,0}; live_=1;
    }
    DecodeStatus run(const int8_t* logits, size_t frames, int exponent, const DecodeControl& control,
                     char* text, size_t capacity, DecodeStats* stats);
 private:
    uint8_t byte(const Cursor& c) const { return uint8_t(kVocab[c.extra>=0 ? c.extra : nodes_[c.node].ch]); }
    size_t depth(const Cursor& c) const { return nodes_[c.node].depth+(c.extra>=0); }
    // Ancestor of c (inclusive) with the given length, via jump pointers.
    Cursor ancestor(Cursor c, size_t length) const {
        if(c.extra>=0) { if(depth(c)==length)return c; c.extra=-1; }
        uint32_t x=c.node;
        while(nodes_[x].depth>length)x=nodes_[nodes_[x].jump].depth>=length ? nodes_[x].jump : nodes_[x].parent;
        return {x,-1};
    }
    // Prefix byte order without materialising either string: cut both to
    // the shorter length, then climb to the children of their lowest common
    // ancestor. The trie is canonical, so distinct nodes are distinct strings.
    // Exact score ties persist across frames (tied branches that only differ
    // in an early character), so this must not walk whole prefixes.
    int compare(Cursor a, Cursor b) const {
        const size_t la=depth(a), lb=depth(b), length=std::min(la,lb);
        const int shorter=la<lb ? -1 : la>lb ? 1 : 0;
        a=ancestor(a,length); b=ancestor(b,length);
        if(a.node==b.node && a.extra==b.extra)return shorter;
        const uint32_t pa=a.extra>=0 ? a.node : nodes_[a.node].parent, pb=b.extra>=0 ? b.node : nodes_[b.node].parent;
        if(pa==pb)return byte(a)==byte(b) ? shorter : byte(a)<byte(b) ? -1 : 1;
        uint32_t x=pa, y=pb; // distinct real nodes of equal depth
        while(nodes_[x].parent!=nodes_[y].parent) {
            if(nodes_[x].jump!=nodes_[y].jump) { x=nodes_[x].jump; y=nodes_[y].jump; }
            else { x=nodes_[x].parent; y=nodes_[y].parent; }
        }
        return nodes_[x].ch==nodes_[y].ch ? shorter : uint8_t(kVocab[nodes_[x].ch])<uint8_t(kVocab[nodes_[y].ch]) ? -1 : 1;
    }
    Cursor cursor(uint16_t j) const { const Cand& c=cands_[j]; return c.fresh ? Cursor{c.parent,c.ch} : Cursor{c.node,-1}; }
    bool better(uint16_t a, uint16_t b) const {
        if(cands_[a].key!=cands_[b].key)return cands_[a].key>cands_[b].key;
        if(cands_[a].score!=cands_[b].score)return cands_[a].score>cands_[b].score;
        return compare(cursor(a),cursor(b))<0; // ties: prefix bytes ascending
    }
    // Word completion after node id (ends in a letter/apostrophe), computed
    // once per node: alpha*ln10*log10P(word|ctx) + beta (+ hotword bonus).
    void complete(uint32_t id) {
        Node& n=nodes_[id];
        if(n.spaceReady)return;
        char word[256]; const size_t length=n.wordLen;
        int found=-1; bool listed=false;
        if(length<255) {
            uint32_t at=id;
            for(size_t i=length;i>0;--i) { word[i-1]=kVocab[nodes_[at].ch]; at=nodes_[at].parent; }
            found=lm_.find(word,length);
            listed=words_ && words_->contains(word,length);
        }
        // Same operation order as the reference: delta first, then accumulate.
        const double log10p=found<0 ? double(lm_.unkLog10()) : lm_.log10(n.ctx2,n.ctx1,uint16_t(found));
        const double delta=alphaLn_*log10p+double(lm_.beta())+(listed ? double(lm_.hotwordBonus()) : 0.0);
        n.spaceLm=n.lm+delta;
        n.spaceWord=found<0 ? lm_.unk() : uint16_t(found); n.spaceReady=1; ++completions_;
    }
    uint32_t child(uint32_t parent, uint8_t c) {
        const uint32_t key=parent<<5|c;
        uint32_t h=key*0x9e3779b1u; h^=h>>15;
        for(h&=mask_;table_[h]!=kEmpty;h=(h+1)&mask_) {
            const Node& n=nodes_[table_[h]];
            if(n.parent==parent && n.ch==c)return table_[h];
        }
        if(c==kSpace)complete(parent);
        const Node& p=nodes_[parent];
        const uint32_t id=count_++;
        Node& n=nodes_[id];
        n.parent=parent;n.ch=c;n.depth=uint16_t(p.depth+1);n.spaceLm=0;n.spaceWord=0;n.spaceReady=0;n.pad=0;
        const Node& j=nodes_[p.jump]; // Myers' skew-binary jump: depends only on depth
        n.jump=p.depth-j.depth==j.depth-nodes_[j.jump].depth ? j.jump : parent;
        if(c==kSpace) { n.lm=p.spaceLm;n.ctx2=p.ctx1;n.ctx1=p.spaceWord;n.wordLen=0; }
        else { n.lm=p.lm;n.ctx2=p.ctx2;n.ctx1=p.ctx1;n.wordLen=uint8_t(std::min(255,p.wordLen+1)); }
        table_[h]=id; return id;
    }
    uint16_t localFind(uint32_t key) const {
        for(uint32_t h=(key*0x9e3779b1u)>>25;local_[h]!=kEmpty;h=(h+1)&(kLocal-1))
            if(local_[h]==key)return slots_[h];
        return 0xffff;
    }
    void localInsert(uint32_t key, uint8_t slot) {
        uint32_t h=(key*0x9e3779b1u)>>25;
        while(local_[h]!=kEmpty)h=(h+1)&(kLocal-1);
        local_[h]=key; slots_[h]=slot;
    }
    // prefix+c gains value in p_nb: merge into a live beam with that exact
    // prefix, else append a new candidate ((parent, c) pairs are unique).
    void extend(uint32_t parent, uint8_t c, double value) {
        if(value==kNegInf)return;
        const uint16_t j=localFind(parent<<5|c);
        if(j!=0xffff) { cands_[j].pnb=lse(cands_[j].pnb,value); return; }
        cands_[candidates_++]=Cand{kNegInf,value,0,0,parent,0,c,1,{}};
    }
    void step(const double* lp, const uint8_t* active, size_t count);
    void select();
    const Lm& lm_;
    const CustomWords* words_;
    const double alphaLn_;
    Node* nodes_; uint32_t* table_; Beam* beams_; Cand* cands_; uint16_t* heap_;
    uint32_t* local_; uint8_t* slots_; double* exps_;
    size_t mask_, width_, count_=0, live_=0, candidates_=0, considered_=0, completions_=0;
};
// active: the non-blank classes with lp >= char_prune this frame, ascending.
// The repeat of a prefix's last symbol is never pruned (reference reading of
// FORMAT.md, hw1lm.py): after a space it is blank-like (p_b), after a letter
// it stays in p_nb and doubles the letter from p_b. A leading space is an
// ordinary (pruned) token that is blank-like.
void Search::step(const double* lp, const uint8_t* active, size_t count) {
    std::fill(local_,local_+kLocal,kEmpty);
    for(size_t i=0;i<live_;++i) {
        cands_[i]=Cand{kNegInf,kNegInf,0,beams_[i].node,0,0,0,0,{}};
        if(beams_[i].node) { const Node& n=nodes_[beams_[i].node]; localInsert(n.parent<<5|n.ch,uint8_t(i)); }
    }
    candidates_=live_;
    for(size_t i=0;i<live_;++i) {
        const Beam b=beams_[i];
        const double total=lse(b.pb,b.pnb);
        const int last=b.node ? nodes_[b.node].ch : -1;
        cands_[i].pb=lse(cands_[i].pb,total+lp[kBlank]);
        if(last==int(kSpace))cands_[i].pb=lse(cands_[i].pb,total+lp[kSpace]);
        else if(last>0) { cands_[i].pnb=lse(cands_[i].pnb,b.pnb+lp[last]); extend(b.node,uint8_t(last),b.pb+lp[last]); }
        for(size_t k=0;k<count;++k) {
            const uint8_t c=active[k];
            if(int(c)==last)continue;
            if(c==kSpace && last<0) { cands_[i].pb=lse(cands_[i].pb,total+lp[c]); continue; }
            extend(b.node,c,total+lp[c]);
        }
    }
    for(size_t j=0;j<candidates_;++j) {
        Cand& c=cands_[j];
        double lm;
        if(!c.fresh)lm=nodes_[c.node].lm;
        else if(c.ch==kSpace) { complete(c.parent); lm=nodes_[c.parent].spaceLm; }
        else lm=nodes_[c.parent].lm;
        c.score=lse(c.pb,c.pnb)+lm; c.key=float(c.score);
    }
    considered_+=candidates_;
    select();
}
void Search::select() {
    // Bounded heap, worst kept candidate on top; the order is total (distinct
    // prefixes), so the kept set is exactly the top width_.
    size_t kept=0;
    auto sink=[&](size_t i) {
        for(;;) {
            size_t l=2*i+1, r=l+1, m=i;
            if(l<kept && better(heap_[m],heap_[l]))m=l;
            if(r<kept && better(heap_[m],heap_[r]))m=r;
            if(m==i)return;
            std::swap(heap_[i],heap_[m]); i=m;
        }
    };
    for(size_t j=0;j<candidates_;++j) {
        if(kept<width_) {
            size_t i=kept++; heap_[i]=uint16_t(j);
            while(i && better(heap_[(i-1)/2],heap_[i])) { std::swap(heap_[i],heap_[(i-1)/2]); i=(i-1)/2; }
        } else if(better(uint16_t(j),heap_[0])) { heap_[0]=uint16_t(j); sink(0); }
    }
    for(size_t i=0;i<kept;++i) {
        const Cand& c=cands_[heap_[i]];
        beams_[i]=Beam{c.pb,c.pnb,c.fresh ? child(c.parent,c.ch) : c.node,0};
    }
    live_=kept;
}
DecodeStatus Search::run(const int8_t* logits, size_t frames, int exponent, const DecodeControl& control,
                         char* text, size_t capacity, DecodeStats* stats) {
    // log_softmax of q*2^exponent: exp(x-max) takes only 256 distinct values.
    const double scale=std::ldexp(1.0,exponent);
    for(int d=0;d<256;++d)exps_[d]=std::exp(-double(d)*scale);
    const double prune=lm_.charPrune();
    double lp[kClasses]; uint8_t active[kBlank];
    for(size_t t=0;t<frames;++t) {
        if(!(t%kYieldFrames)) {
            if(control.cancelled && control.cancelled(control.context))return DecodeStatus::Cancelled;
            if(t)yieldTask();
        }
        const int8_t* q=logits+t*kClasses;
        int top=q[0];
        for(size_t c=1;c<kClasses;++c)top=std::max(top,int(q[c]));
        double sum=0;
        for(size_t c=0;c<kClasses;++c)sum+=exps_[top-q[c]];
        const double lz=std::log(sum);
        size_t count=0;
        for(size_t c=0;c<kClasses;++c) {
            lp[c]=double(q[c]-top)*scale-lz;
            if(c<kBlank && lp[c]>=prune)active[count++]=uint8_t(c);
        }
        step(lp,active,count);
    }
    if(control.cancelled && control.cancelled(control.context))return DecodeStatus::Cancelled;
    // Finish: score any partial word as a completion, then </s>.
    size_t best=0; double bestScore=kNegInf;
    for(size_t i=0;i<live_;++i) {
        const uint32_t id=beams_[i].node;
        double lm; uint16_t w2, w1;
        if(nodes_[id].wordLen) { complete(id); lm=nodes_[id].spaceLm; w2=nodes_[id].ctx1; w1=nodes_[id].spaceWord; }
        else { lm=nodes_[id].lm; w2=nodes_[id].ctx2; w1=nodes_[id].ctx1; }
        lm+=alphaLn_*lm_.log10(w2,w1,lm_.eos());
        const double score=lse(beams_[i].pb,beams_[i].pnb)+lm;
        if(!i || score>bestScore || (score==bestScore && compare({id,-1},{beams_[best].node,-1})<0)) { best=i; bestScore=score; }
    }
    if(stats) {
        stats->frames=uint32_t(frames); stats->beam=uint32_t(width_); stats->nodes=uint32_t(count_);
        stats->candidates=uint32_t(std::min<size_t>(considered_,UINT32_MAX)); stats->completions=uint32_t(completions_);
    }
    uint32_t at=beams_[best].node;
    size_t length=nodes_[at].depth;
    if(length && nodes_[at].ch==kSpace) { at=nodes_[at].parent; --length; } // no trailing space
    const bool fits=length<capacity;
    for(size_t skip=fits ? 0 : length-(capacity-1);skip;--skip) { at=nodes_[at].parent; --length; }
    text[length]=0;
    for(size_t i=length;i>0;--i) { text[i-1]=kVocab[nodes_[at].ch]; at=nodes_[at].parent; }
    return fits ? DecodeStatus::Ok : DecodeStatus::OutputTooSmall;
}
}

const char* describe(LoadStatus s) {
    switch(s) {
        case LoadStatus::Ok: return "ok";
        case LoadStatus::InvalidArgument: return "invalid argument";
        case LoadStatus::BadSize: return "file size does not match header";
        case LoadStatus::BadMagic: return "not an HW1LM1 file";
        case LoadStatus::BadVersion: return "unsupported version";
        case LoadStatus::BadHeader: return "invalid header field";
        case LoadStatus::BadLayout: return "sections exceed file";
        case LoadStatus::BadPadding: return "non-zero padding";
        case LoadStatus::BadChecksum: return "SHA-256 mismatch";
        case LoadStatus::BadStrings: return "invalid string table";
        case LoadStatus::Unsorted: return "unsorted words or n-grams";
        case LoadStatus::BadWordId: return "n-gram word id out of range";
        case LoadStatus::MissingSpecial: return "missing <s>, </s> or <unk>";
    }
    return "unknown";
}

bool sha256(const void* data, size_t bytes, uint8_t hash[32]) {
    if(!hash || (!data && bytes))return false;
#if defined(ESP_PLATFORM)
    return mbedtls_sha256(static_cast<const unsigned char*>(data),bytes,hash,0)==0;
#else
    uint32_t h[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    auto p=static_cast<const uint8_t*>(data);
    size_t left=bytes;
    for(;left>=64;left-=64,p+=64)shaBlock(h,p);
    uint8_t tail[128]={};
    if(left)std::memcpy(tail,p,left);
    tail[left]=0x80;
    const size_t end=left<56 ? 64 : 128;
    const uint64_t bits=uint64_t(bytes)*8;
    for(int i=0;i<8;++i)tail[end-1-i]=uint8_t(bits>>(8*i));
    shaBlock(h,tail);
    if(end==128)shaBlock(h,tail+64);
    for(int i=0;i<8;++i)for(int k=0;k<4;++k)hash[4*i+k]=uint8_t(h[i]>>(24-8*k));
    return true;
#endif
}

LoadStatus Lm::open(const uint8_t* d, size_t n) {
    *this=Lm{};
    if(!d)return LoadStatus::InvalidArgument;
    if(n<kHeaderBytes+kShaBytes)return LoadStatus::BadSize;
    if(std::memcmp(d,"HW1LM1\0\0",8))return LoadStatus::BadMagic;
    if(u32(d+8)!=1)return LoadStatus::BadVersion;
    if(u32(d+48)!=n)return LoadStatus::BadSize;
    const uint32_t V=u32(d+12), nb=u32(d+16), nt=u32(d+20), sb=u32(d+24), beam=u32(d+40);
    const float alpha=f32(d+28), beta=f32(d+32), unk=f32(d+36), prune=f32(d+44), bonus=f32(d+52);
    bool reserved=false;
    for(size_t i=56;i<kHeaderBytes;++i)reserved|=d[i]!=0;
    if(V<3 || V>65535 || !sb || !beam || beam>kMaxBeam || reserved || !std::isfinite(alpha) || !std::isfinite(beta)
       || !std::isfinite(unk) || !std::isfinite(prune) || !std::isfinite(bonus))return LoadStatus::BadHeader;
    // 64-bit section arithmetic cannot overflow from 32-bit counts.
    const uint64_t sizes[5]={4ull*V,4ull*V,sb,8ull*nb,8ull*nt};
    uint64_t starts[5], end=kHeaderBytes;
    for(int i=0;i<5;++i) { starts[i]=align4(end); end=starts[i]+sizes[i]; }
    const uint64_t digest=align4(end);
    if(digest+kShaBytes!=n)return LoadStatus::BadLayout;
    uint64_t previous=kHeaderBytes;
    for(int i=0;i<=5;++i) {
        const uint64_t start=i<5 ? starts[i] : digest;
        for(uint64_t p=previous;p<start;++p)if(d[p])return LoadStatus::BadPadding;
        if(i<5)previous=start+sizes[i];
    }
    uint8_t hash[32];
    if(!sha256(d,size_t(digest),hash) || std::memcmp(hash,d+digest,32))return LoadStatus::BadChecksum;
    const uint8_t* offsets=d+starts[1]; const char* strings=reinterpret_cast<const char*>(d+starts[2]);
    if(strings[sb-1])return LoadStatus::BadStrings;
    const char* previousWord=nullptr;
    for(uint32_t i=0;i<V;++i) {
        const uint32_t o=u32(offsets+4*i);
        if(o>=sb)return LoadStatus::BadStrings;
        if(previousWord && std::strcmp(previousWord,strings+o)>=0)return LoadStatus::Unsorted;
        previousWord=strings+o;
    }
    for(uint32_t i=0, prior=0;i<nb;++i) {
        const uint8_t* p=d+starts[3]+8ull*i;
        if(u16(p)>=V || u16(p+2)>=V)return LoadStatus::BadWordId;
        const uint32_t key=uint32_t(u16(p))<<16|u16(p+2);
        if(i && key<=prior)return LoadStatus::Unsorted;
        prior=key;
    }
    for(uint64_t i=0, prior=0;i<nt;++i) {
        const uint8_t* p=d+starts[4]+8*i;
        if(u16(p)>=V || u16(p+2)>=V || u16(p+4)>=V)return LoadStatus::BadWordId;
        const uint64_t key=uint64_t(u16(p))<<32|uint64_t(u16(p+2))<<16|u16(p+4);
        if(i && key<=prior)return LoadStatus::Unsorted;
        prior=key;
    }
    Lm view;
    view.data_=d;view.unigram_=d+starts[0];view.offsets_=offsets;view.strings_=d+starts[2];
    view.bigram_=d+starts[3];view.trigram_=d+starts[4];
    view.vocab_=V;view.bigrams_=nb;view.trigrams_=nt;view.strings_bytes_=sb;view.beam_=beam;
    view.alpha_=alpha;view.beta_=beta;view.unk_=unk;view.prune_=prune;view.bonus_=bonus;
    const int bos=view.find("<s>",3), eos=view.find("</s>",4), unkId=view.find("<unk>",5);
    if(bos<0 || eos<0 || unkId<0)return LoadStatus::MissingSpecial;
    view.bos_=uint16_t(bos);view.eos_=uint16_t(eos);view.unkId_=uint16_t(unkId);
    *this=view;
    return LoadStatus::Ok;
}
const char* Lm::word(uint16_t id) const {
    return data_ && id<vocab_ ? reinterpret_cast<const char*>(strings_+u32(offsets_+4*id)) : nullptr;
}
int Lm::find(const char* key, size_t length) const {
    if(!strings_ || (!key && length))return -1;
    size_t lo=0, hi=vocab_;
    while(lo<hi) {
        const size_t mid=lo+(hi-lo)/2;
        const int c=compareWord(reinterpret_cast<const char*>(strings_+u32(offsets_+4*mid)),key,length);
        if(!c)return int(mid);
        if(c<0)lo=mid+1; else hi=mid;
    }
    return -1;
}
long Lm::findBigram(uint16_t a, uint16_t b) const {
    const uint32_t key=uint32_t(a)<<16|b;
    size_t lo=0, hi=bigrams_;
    while(lo<hi) {
        const size_t mid=lo+(hi-lo)/2; const uint8_t* p=bigram_+8*mid;
        const uint32_t k=uint32_t(u16(p))<<16|u16(p+2);
        if(k==key)return long(mid);
        if(k<key)lo=mid+1; else hi=mid;
    }
    return -1;
}
long Lm::findTrigram(uint16_t a, uint16_t b, uint16_t c) const {
    const uint64_t key=uint64_t(a)<<32|uint64_t(b)<<16|c;
    size_t lo=0, hi=trigrams_;
    while(lo<hi) {
        const size_t mid=lo+(hi-lo)/2; const uint8_t* p=trigram_+8*mid;
        const uint64_t k=uint64_t(u16(p))<<32|uint64_t(u16(p+2))<<16|u16(p+4);
        if(k==key)return long(mid);
        if(k<key)lo=mid+1; else hi=mid;
    }
    return -1;
}
int32_t Lm::p2(uint16_t b, uint16_t c) const {
    const long i=findBigram(b,c);
    return i>=0 ? i16(bigram_+8*i+4) : i16(unigram_+4*b+2)+i16(unigram_+4*c);
}
// Doubles summed in hw1lm.py's order, each term q/1000.0.
double Lm::p2d(uint16_t b, uint16_t c) const {
    const long i=findBigram(b,c);
    return i>=0 ? i16(bigram_+8*i+4)/1000.0 : i16(unigram_+4*b+2)/1000.0+i16(unigram_+4*c)/1000.0;
}
double Lm::log10(uint16_t w2, uint16_t w1, uint16_t w) const {
    if(!data_ || w1>=vocab_ || w>=vocab_ || (w2!=kNoWord && w2>=vocab_))return std::numeric_limits<double>::quiet_NaN();
    if(w2==kNoWord)return p2d(w1,w);
    const long t=findTrigram(w2,w1,w);
    if(t>=0)return i16(trigram_+8*t+6)/1000.0;
    const long b=findBigram(w2,w1);
    return (b>=0 ? i16(bigram_+8*b+6)/1000.0 : 0.0)+p2d(w1,w);
}
int32_t Lm::score(uint16_t w2, uint16_t w1, uint16_t w) const {
    if(!data_ || w1>=vocab_ || w>=vocab_ || (w2!=kNoWord && w2>=vocab_))return kInvalidScore;
    if(w2==kNoWord)return p2(w1,w);
    const long t=findTrigram(w2,w1,w);
    if(t>=0)return i16(trigram_+8*t+6);
    const long b=findBigram(w2,w1);
    return (b>=0 ? i16(bigram_+8*b+6) : 0)+p2(w1,w);
}

size_t CustomWords::parse(const char* data, size_t bytes) {
    count_=0;
    if(!data)return 0;
    const auto p=reinterpret_cast<const uint8_t*>(data);
    LineWord line;
    for(size_t i=0;i<=bytes && count_<kMaxCustomWords;) {
        uint32_t cp='\n'; // the end of data ends the last line
        i+=i<bytes ? utf8(p+i,bytes-i,cp) : 1;
        if(!lineBreak(cp)) { feedLower(line,cp); continue; }
        char word[kMaxCustomWordBytes+1]; size_t length=0;
        if(line.finish(word,length)) { // sorted insert, first occurrence wins
            size_t lo=0, hi=count_; int c=1;
            while(lo<hi && c) {
                const size_t mid=lo+(hi-lo)/2;
                c=std::strcmp(words_[mid],word);
                if(c<0)lo=mid+1; else if(c>0)hi=mid;
            }
            if(c) {
                std::memmove(words_[lo+1],words_[lo],(count_-lo)*sizeof(words_[0]));
                std::memcpy(words_[lo],word,length+1); ++count_;
            }
        }
        line=LineWord{};
    }
    return count_;
}
bool CustomWords::contains(const char* word, size_t length) const {
    if(!word || !length || length>kMaxCustomWordBytes)return false;
    size_t lo=0, hi=count_;
    while(lo<hi) {
        const size_t mid=lo+(hi-lo)/2;
        const int c=compareWord(words_[mid],word,length);
        if(!c)return true;
        if(c<0)lo=mid+1; else hi=mid;
    }
    return false;
}

size_t workspace_bytes(size_t frames, size_t beam) {
    Layout l; return layout(frames,beam,l) ? l.bytes : 0;
}
DecodeStatus decode(const int8_t* logits, size_t frames, int exponent, const Lm& lm,
                    const CustomWords* words, char* text, size_t capacity,
                    const DecodeControl& control, size_t beam, DecodeStats* stats) {
    if(text && capacity)text[0]=0;
    if(stats)*stats=DecodeStats{};
    if(!beam)beam=lm.beamWidth();
    Layout l;
    if(!text || !capacity || (!logits && frames) || !lm.valid() || exponent<-30 || exponent>30
       || !layout(frames,beam,l))return DecodeStatus::InvalidArgument;
    if(control.cancelled && control.cancelled(control.context))return DecodeStatus::Cancelled;
    auto workspace=static_cast<uint8_t*>(allocate(l.bytes));
    if(!workspace)return DecodeStatus::OutOfMemory;
    DecodeStatus status;
    {
        Search search(lm,words,workspace,l,beam);
        status=search.run(logits,frames,exponent,control,text,capacity,stats);
    }
    release(workspace);
    if(stats)stats->workspaceBytes=l.bytes;
    if(status==DecodeStatus::Cancelled)text[0]=0;
    return status;
}
}
