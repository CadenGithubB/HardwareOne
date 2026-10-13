// Host test: ESP-DL's metadata allocator hook end to end, with the real code.
//
// Compiled unchanged from the tree: dl_tool_metadata.{hpp,cpp} (the hook API, the
// metadata containers and DL_METADATA_NEW_DELETE), dl_model_context.{hpp,cpp} (the
// name map, tensor lists, clear/minimize), dl_tensor_base.hpp (TensorBase, ExponentInfo)
// with TensorBase's destructor, constructors and set_shape extracted from
// dl_tensor_base.cpp (tensor_extract.inc, made by test_stt_dl_metadata.py), and
// HardwareOne's stt_dl_metadata.{h,cpp} (ps_alloc / aligned PSRAM requests, counters).
// The ESP-IDF heap and System_MemUtil's ps_alloc/ps_free are fakes (fake_heap.cpp) that
// register every block with where it lives and which allocator made it.
//
// The core property: every block the hook hands out is returned through the hook's free
// (ps_free) and nothing else, through every destructor path (delete of a context, of a
// tensor, minimize, clear, container growth/shrink, failure paths), and the hook's free
// also takes the prebuilt-style blocks (TensorBase objects libfbs_model allocates with
// malloc_aligned) that ESP-DL's operator delete returns to it.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <new>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "dl_model_context.hpp"
#include "dl_tensor_base.hpp"
#include "dl_tool_metadata.hpp"
#include "System_MemUtil.h"
#include "fake_heap.h"
#include "stt_dl_metadata.h"

namespace dl {
#include "tensor_extract.inc"
void TensorBase::print(bool) {}  // debug output, not under test (its only other virtual)
}  // namespace dl

using dl::TensorBase;
using dl::tool::MetadataKind;
using hw1::stt::DlMetadataStats;

namespace {
DlMetadataStats delta(const DlMetadataStats& a) { return hw1::stt::dlMetadataDelta(a, hw1::stt::dlMetadataStats()); }

std::map<std::string, size_t> gTags;  // tag -> allocations seen by memAllocDebug
std::mutex gTagMutex;
void recordTag(const char*, void* p, size_t, bool requested, bool, bool, const char* tag) {
  assert(requested && tag);
  if (!p) return;
  std::lock_guard<std::mutex> lock(gTagMutex);
  ++gTags[tag];
}

std::string longName(int i) { return "/blocks." + std::to_string(i % 15) + "/main/main." + std::to_string(i) + "/Conv_output_0"; }
std::string paramName(int i) { return "blocks." + std::to_string(i % 15) + ".main." + std::to_string(i) + ".weight"; }

// Upstream placement while no allocator is installed: module/tensor/exponent blocks from
// malloc_aligned(MALLOC_CAP_DEFAULT), the rest from malloc; the fake heap sees none of them.
void defaultPath() {
  assert(!dl::tool::get_metadata_allocator() && !hw1::stt::dlMetadataAllocatorInstalled());
  fake::reset();
  const size_t aligned0 = fake::gMallocAlignedCalls.load();
  void* t = dl::tool::metadata_alloc(64, 16, MetadataKind::Tensor);
  void* e = dl::tool::metadata_alloc(400, 16, MetadataKind::Exponent);
  void* n = dl::tool::metadata_alloc(37, alignof(int), MetadataKind::Names);
  assert(t && e && n && fake::gMallocAlignedCalls.load() == aligned0 + 2);
  assert(reinterpret_cast<uintptr_t>(t) % 16 == 0 && reinterpret_cast<uintptr_t>(e) % 16 == 0);
  dl::tool::metadata_free(t);
  dl::tool::metadata_free(e);
  dl::tool::metadata_free(n);
  dl::tool::metadata_free(nullptr);
  {
    auto* ctx = new dl::ModelContext();
    std::vector<int> exps(32, -7);
    int8_t weights[64] = {};
    ctx->add_tensor("w", true, new TensorBase({2, 32}, weights, exps, dl::DATA_TYPE_INT8, false));
    ctx->add_tensor(longName(1), false, nullptr);
    assert(ctx->get_tensor("w")->exponent.is_per_channel());
    delete ctx;
  }
  assert(fake::liveAll() == 0 && fake::counts().psAllocBlocks == 0 && fake::counts().alignedBlocks == 0);
  assert(hw1::stt::dlMetadataStats().allocs == 0);
}

void install() {
  assert(hw1::stt::installDlMetadataAllocator());
  const auto* a = dl::tool::get_metadata_allocator();
  assert(a && a->alloc && a->free && hw1::stt::dlMetadataAllocatorInstalled());
  assert(hw1::stt::installDlMetadataAllocator() && dl::tool::get_metadata_allocator() == a);  // idempotent
}

// A model-sized context: 202 activations, 268 parameters (some allocated the way prebuilt
// libfbs_model does), per-channel exponents, long and short names; looked up, re-shaped,
// minimized and destroyed.
void contextLifecycle() {
  fake::reset();
  gTags.clear();
  const DlMetadataStats s0 = hw1::stt::dlMetadataStats();
  const auto foreign0 = dl::tool::metadata_foreign_stats();
  std::vector<int8_t> arena(1 << 16), weights(1 << 16);
  auto* ctx = new dl::ModelContext();
  assert(fake::owns(ctx) && fake_heap_is_psram(ctx));
  std::vector<TensorBase*> prebuilt;
  for (int i = 0; i < 268; ++i) {
    const int channels = 8 + i % 5;
    std::vector<int> exps(channels);
    for (int c = 0; c < channels; ++c) exps[c] = -(c % 7);
    const std::vector<int> shape = i % 3 == 2 ? std::vector<int>{channels} : std::vector<int>{1, 1, 4, channels};
    TensorBase* t;
    if (i % 4 == 0) {  // as fbs::create_tensor: malloc_aligned for the object, then the constructor
      void* raw = fake::prebuiltAlloc(sizeof(TensorBase));
      t = new (raw) TensorBase(shape, weights.data() + 16 * i, exps, dl::DATA_TYPE_INT8, false);
      prebuilt.push_back(t);
      dl::tool::metadata_note_foreign(t);
    } else {
      t = new TensorBase(shape, weights.data() + 16 * i, exps, dl::DATA_TYPE_INT8, false);
      assert(fake::origin(t) == fake::Origin::HookAligned && reinterpret_cast<uintptr_t>(t) % 16 == 0);
    }
    assert(t->exponent.is_valid() && t->exponent.is_per_channel());
    assert(fake::owns(t->exponent.data()) && reinterpret_cast<uintptr_t>(t->exponent.data()) % 16 == 0);
    assert(fake::owns(t->shape.data()) && fake::owns(t->axis_offset.data()));
    const int index = ctx->add_tensor(i % 2 ? paramName(i) : "p" + std::to_string(i), true, t);
    assert(index >= CONTEXT_PARAMETER_OFFSET);
  }
  const auto foreign1 = dl::tool::metadata_foreign_stats();
  assert(foreign1.objects - foreign0.objects == prebuilt.size() && foreign1.internal == foreign0.internal);
  for (int i = 0; i < 202; ++i) {
    const int index = ctx->add_tensor(i % 3 ? longName(i) : "v" + std::to_string(i), false, nullptr);
    assert(index == i);
    ctx->update_tensor(index, new TensorBase({1, 2000, 1, 64}, arena.data(), -4, dl::DATA_TYPE_INT8, false));
  }
  assert(ctx->add_tensor(longName(1), false, ctx->get_tensor(1)) == 1);  // existing name: same slot
  assert(ctx->get_variable_count() == 202 && ctx->get_parameter_count() == 268);
  for (int i = 0; i < 202; ++i) {
    const std::string name = i % 3 ? longName(i) : "v" + std::to_string(i);
    assert(ctx->has_tensor(name) && ctx->get_tensor_index(name) == i && ctx->get_variable_index(name) == i);
    assert(ctx->get_tensor(name) == ctx->m_variables[i]);
  }
  assert(ctx->get_tensor_index(paramName(1)) == CONTEXT_PARAMETER_OFFSET + 1 && ctx->get_variable_index(paramName(1)) == -1);
  assert(!ctx->has_tensor("missing") && ctx->get_tensor_index("missing") == -1);
  // Everything the hook placed is in PSRAM; no internal RAM used at all.
  assert(fake::usedInternal() == 0 && fake::counts().psAllocBlocks > 900 && fake::counts().alignedBlocks > 200);
  // Re-shaping (every phrase of a resident model) allocates nothing.
  const DlMetadataStats before = hw1::stt::dlMetadataStats();
  for (int frames : {7, 1999, 333, 2000}) {
    for (int i = 0; i < 202; ++i) {
      TensorBase* t = ctx->m_variables[i];
      const int* storage = t->shape.data();
      t->set_shape({1, frames, 1, 64});
      assert(t->shape.data() == storage && t->shape == std::vector<int>({1, frames, 1, 64}));
      assert(t->axis_offset == std::vector<int>({frames * 64, 64, 64, 1}) && t->get_size() == frames * 64);
    }
  }
  assert(delta(before).allocs == 0 && delta(before).frees == 0);
  // Name strings longer than the small-string buffer and the map nodes are Names blocks.
  assert(gTags["stt.dl.names"] > 400 && gTags["stt.dl.shape"] >= 2 * (268 + 202) && gTags["stt.dl.exp"] == 268);
  assert(gTags["stt.dl.tensor"] == 202 + 268 - prebuilt.size() && gTags["stt.dl.ctx"] == 1 && gTags["stt.dl.list"] > 0);
  // minimize() drops the map: its nodes and keys go back through the hook.
  const size_t names = fake::counts().psFrees;
  ctx->minimize();
  assert(fake::counts().psFrees > names + 400 && !ctx->has_tensor(longName(1)));
  delete ctx;  // clear(): every tensor (hook-made and prebuilt-style), lists, the object
  assert(fake::liveAll() == 0);
  const fake::Counts c = fake::counts();
  assert(c.otherFrees == 0 && c.foreignThroughPsFree == prebuilt.size());
  assert(c.psFrees == c.psAllocBlocks + c.alignedBlocks + prebuilt.size());
  const DlMetadataStats d = delta(s0);
  assert(d.allocs == c.psAllocBlocks + c.alignedBlocks && d.frees == c.psFrees && d.internal == 0 && d.failed == 0);
  std::printf("PASS context: %u hook blocks (%u bytes), %zu prebuilt-style tensors, all returned through ps_free\n",
              unsigned(d.allocs), unsigned(d.bytes), prebuilt.size());
}

// Containers and objects on their own: growth, copies, moves, swaps, the conversions
// ESP-DL relies on, class operator new/delete forms.
struct Polymorphic {
  DL_METADATA_NEW_DELETE(Module, 16)
  virtual ~Polymorphic() = default;
  int a = 1;
};
struct Derived : Polymorphic {
  dl::tool::meta_vector<int, MetadataKind::Index> idx{1, 2, 3};
  ~Derived() override = default;
};
void containersAndObjects() {
  fake::reset();
  const DlMetadataStats s0 = hw1::stt::dlMetadataStats();
  {
    using V = dl::tool::meta_vector<int, MetadataKind::Index>;
    V v;
    for (int i = 0; i < 1000; ++i) v.push_back(i);
    V copy = v, moved = std::move(copy);
    assert(copy.empty() && moved.size() == 1000 && moved == std::vector<int>(v.begin(), v.end()));
    std::vector<int> plain = moved;  // conversion: a copy on the default heap
    assert(plain.size() == 1000 && plain == moved && moved == plain && !(plain != moved));
    V from(plain);
    from = std::vector<int>{4, 5};
    assert(from == std::vector<int>({4, 5}) && from.size() == 2);
    from = {7, 8, 9};
    assert(from.size() == 3 && from[2] == 9);
    from.swap(moved);
    assert(from.size() == 1000 && moved.size() == 3);
    from.shrink_to_fit();
    from.clear();
    from.shrink_to_fit();
    assert(dl::vector_to_string(moved) == "[7, 8, 9]");
    dl::meta_shape shape{1, 2, 3};
    int8_t data[6] = {};
    TensorBase t({1, 2, 3}, data, 0, dl::DATA_TYPE_INT8, false);  // a stack object
    assert(&t.get_shape() == &t.shape && t.get_shape() == std::vector<int>({1, 2, 3}) && t.shape == shape);
    dl::tool::meta_name_map<int> names;
    names.emplace(dl::tool::meta_string(longName(3).data(), longName(3).size()), 3);
    assert(names.find(longName(3)) != names.end() && names.find(std::string("x")) == names.end());
    assert(names.count(longName(3)) == 1);
    auto* p = new Polymorphic;
    Polymorphic* d = new Derived;
    assert(fake::origin(p) == fake::Origin::HookAligned && reinterpret_cast<uintptr_t>(d) % 16 == 0);
    delete p;
    delete d;  // virtual destructor: the class operator delete of the base, the hook's free
    Polymorphic* q = new (std::nothrow) Polymorphic;
    assert(q);
    delete q;
    alignas(16) unsigned char buffer[sizeof(Polymorphic)];
    Polymorphic* placed = new (buffer) Polymorphic;  // placement new stays available
    placed->~Polymorphic();
    assert(!fake::owns(buffer));
    // The runtime's own blocks (stt.resident).
    std::vector<int, hw1::stt::PsramStlAllocator<int>> resident;
    for (int i = 0; i < 300; ++i) resident.push_back(i);
    void* r = hw1::stt::sttPsramAlloc(40, "stt.resident");
    assert(r && fake_heap_is_psram(r) && fake::origin(r) == fake::Origin::PsAlloc);
    hw1::stt::sttPsramFree(r);
  }
  assert(fake::liveAll() == 0);
  const fake::Counts c = fake::counts();
  assert(c.otherFrees == 0 && c.psFrees == c.psAllocBlocks + c.alignedBlocks);
  const DlMetadataStats d = delta(s0);
  assert(d.allocs == d.frees && d.internal == 0);
  std::printf("PASS containers/objects: %u hook blocks, growth/copy/move/swap/convert, operator new/delete forms\n",
              unsigned(d.allocs));
}

// PSRAM refused: internal RAM, counted (hook counters and ps_alloc's fallback counter).
// Both refused: nullptr, counted; containers report std::bad_alloc (abort on the device);
// per-channel exponents mark the tensor invalid, as upstream does on allocation failure.
void fallbacks() {
  fake::reset({0, 8u << 20});
  const DlMetadataStats s0 = hw1::stt::dlMetadataStats();
  const uint32_t ps0 = gPsAllocFallbacks.load();
  {
    auto* ctx = new dl::ModelContext();
    std::vector<int> exps(16, -3);
    int8_t w[64] = {};
    ctx->add_tensor(paramName(5), true, new TensorBase({1, 1, 4, 16}, w, exps, dl::DATA_TYPE_INT8, false));
    assert(!fake_heap_is_psram(ctx) && fake::usedPsram() == 0 && fake::usedInternal() > 0);
    delete ctx;
  }
  const DlMetadataStats d = delta(s0);
  assert(d.allocs > 5 && d.internal == d.allocs && d.internalBytes == d.bytes && d.failed == 0);
  assert(gPsAllocFallbacks.load() - ps0 == d.allocs);  // both paths report to the shared counter
  assert(fake::liveAll() == 0);
  // Nothing anywhere.
  fake::setBudget({0, 0});
  const DlMetadataStats s1 = hw1::stt::dlMetadataStats();
  assert(!dl::tool::metadata_alloc(10, 4, MetadataKind::Index) && !dl::tool::metadata_alloc(64, 16, MetadataKind::Tensor));
  assert(!(new (std::nothrow) dl::ModelContext()));
  bool threw = false;
  try {
    dl::tool::meta_vector<int, MetadataKind::Index> v;
    v.push_back(1);
  } catch (const std::bad_alloc&) {
    threw = true;
  }
  assert(threw);
  fake::setBudget({1u << 20, 1u << 20});
  int8_t data[4] = {};
  auto* t = new TensorBase({1, 4}, data, 0, dl::DATA_TYPE_INT8, false);
  fake::setBudget({0, 0});
  {
    dl::ExponentInfo e(std::vector<int>{1, 2, 3});
    assert(!e.is_valid() && !e.is_per_channel());
  }
  fake::setBudget({1u << 20, 1u << 20});
  delete t;
  assert(delta(s1).failed >= 5 && fake::liveAll() == 0);
  // The PSRAM bypass (psramBypassGlobal) keeps every block internal; not a fallback.
  fake::setBudget({}); fakeSetBypass(true);
  const DlMetadataStats s2 = hw1::stt::dlMetadataStats();
  const uint32_t ps2 = gPsAllocFallbacks.load();
  void* a = dl::tool::metadata_alloc(10, 4, MetadataKind::Index);
  void* b = dl::tool::metadata_alloc(64, 16, MetadataKind::Module);
  assert(a && b && !fake_heap_is_psram(a) && !fake_heap_is_psram(b) && gPsAllocFallbacks.load() == ps2);
  assert(delta(s2).internal == 2);
  dl::tool::metadata_free(a);
  dl::tool::metadata_free(b);
  fakeSetBypass(false);
  assert(fake::liveAll() == 0 && fake::counts().otherFrees == 0);
  std::printf("PASS fallbacks: PSRAM refused -> internal, counted; nothing -> nullptr / bad_alloc; bypass internal\n");
}

// Concurrent models (another task building one) and repeated installs: atomics only.
void threads() {
  fake::reset();
  const DlMetadataStats s0 = hw1::stt::dlMetadataStats();
  std::vector<std::thread> workers;
  for (int w = 0; w < 4; ++w) {
    workers.emplace_back([w] {
      for (int i = 0; i < 400; ++i) {
        hw1::stt::installDlMetadataAllocator();
        auto* ctx = new dl::ModelContext();
        for (int k = 0; k < 8; ++k) ctx->add_tensor(longName(w * 1000 + i * 8 + k), false, nullptr);
        dl::tool::meta_vector<int, MetadataKind::Shape> s(17, w);
        delete ctx;
      }
    });
  }
  for (auto& t : workers) t.join();
  const DlMetadataStats d = delta(s0);
  assert(fake::liveAll() == 0 && d.allocs == d.frees && d.allocs > 4 * 400 * 8 && fake::counts().otherFrees == 0);
  std::printf("PASS threads: %u blocks from 4 tasks, all returned\n", unsigned(d.allocs));
}

void kindNames() {
  static const char* const expected[] = {"model", "ctx", "loader", "fbs", "module", "tensor",
                                         "exp", "shape", "index", "list", "names"};
  static_assert(sizeof(expected) / sizeof(expected[0]) == dl::tool::kMetadataKinds);
  for (size_t k = 0; k < dl::tool::kMetadataKinds; ++k) {
    assert(!std::strcmp(dl::tool::metadata_kind_name(MetadataKind(k)), expected[k]));
    // Per-kind counters exist for every kind and the tags match ("stt.dl.<name>").
    (void)hw1::stt::dlMetadataStats(k);
  }
  assert(!std::strcmp(dl::tool::metadata_kind_name(MetadataKind::Count), "?"));
  assert(hw1::stt::dlMetadataStats(dl::tool::kMetadataKinds).allocs == 0);
  for (const auto& tag : gTags) {
    assert(tag.first.rfind("stt.dl.", 0) == 0 || tag.first == "stt.resident");
    bool known = tag.first == "stt.resident";
    for (const char* e : expected) known |= tag.first == std::string("stt.dl.") + e;
    assert(known);
  }
}
}  // namespace

int main() {
  fakeSetDebugHook(recordTag);
  defaultPath();
  install();
  contextLifecycle();
  containersAndObjects();
  fallbacks();
  threads();
  kindNames();
  std::printf("dl metadata hook host test passed (C++%ld)\n", long(__cplusplus / 100 % 100));
}
