// Host-side adversarial tests for the exact production notification-list core.

#include <cstdio>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include "../../System_NotificationKindListCore.h"

namespace nkl = hw1_notification_kind_list;

static int failures = 0;

#define CHECK(condition, message)                                            \
  do {                                                                       \
    if (!(condition)) {                                                      \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, message); \
      ++failures;                                                            \
    }                                                                        \
  } while (0)

static const std::string kLongCanonical =
    "kind_whose_canonical_name_is_intentionally_far_longer_than_sixty_three_"
    "bytes_to_prove_known_names_are_not_treated_as_preserved_unknowns";

static nkl::TokenView view(const std::string& value) {
  return {value.data(), value.size()};
}

static bool asciiEqualIgnoreCase(nkl::TokenView input,
                                 const std::string& expected) {
  if (!input.data || input.length != expected.size()) return false;
  for (size_t i = 0; i < input.length; ++i) {
    char a = input.data[i];
    char b = expected[i];
    if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
    if (a != b) return false;
  }
  return true;
}

struct CatalogEntry {
  uint16_t kind;
  std::string canonical;
};

struct CatalogFixture {
  std::vector<CatalogEntry> entries = {
      {1, "alpha"},
      {2, "beta"},
      {3, "boot_finished"},
      {4, "gamma"},
      {5, kLongCanonical},
  };
  bool failEnumeration = false;
  size_t failEnumerationAt = static_cast<size_t>(-1);
  bool invalidResolve = false;

  static bool resolve(void* raw, nkl::TokenView input,
                      nkl::CanonicalToken& out) {
    CatalogFixture& self = *static_cast<CatalogFixture*>(raw);
    if (self.invalidResolve && asciiEqualIgnoreCase(input, "alpha")) {
      out = {{nullptr, 9}, 1};
      return true;
    }
    for (const CatalogEntry& entry : self.entries) {
      if (asciiEqualIgnoreCase(input, entry.canonical)) {
        out = {view(entry.canonical), entry.kind};
        return true;
      }
    }
    // Production compatibility alias: readable, never enumerated.
    if (asciiEqualIgnoreCase(input, "boot")) {
      for (const CatalogEntry& entry : self.entries) {
        if (entry.kind == 3) {
          out = {view(entry.canonical), entry.kind};
          return true;
        }
      }
    }
    return false;
  }

  static bool at(void* raw, size_t ordinal, nkl::CanonicalToken& out) {
    CatalogFixture& self = *static_cast<CatalogFixture*>(raw);
    if (self.failEnumeration && ordinal == self.failEnumerationAt) return false;
    if (ordinal >= self.entries.size()) return false;
    const CatalogEntry& entry = self.entries[ordinal];
    out = {view(entry.canonical), entry.kind};
    return true;
  }

  nkl::CatalogView catalog() {
    return {this, &CatalogFixture::resolve, entries.size(), &CatalogFixture::at};
  }
};

struct SequenceFixture {
  std::vector<std::string> values;
  size_t failAt = static_cast<size_t>(-1);
  size_t calls = 0;

  static bool at(void* raw, size_t index, nkl::TokenView& out) {
    SequenceFixture& self = *static_cast<SequenceFixture*>(raw);
    ++self.calls;
    if (index == self.failAt || index >= self.values.size()) return false;
    out = view(self.values[index]);
    return true;
  }

  nkl::TokenSequence sequence() {
    return {this, values.size(), &SequenceFixture::at};
  }
};

struct SinkFixture {
  std::vector<std::string> values;
  size_t failAt = static_cast<size_t>(-1);
  size_t calls = 0;

  static bool emit(void* raw, nkl::TokenView token) {
    SinkFixture& self = *static_cast<SinkFixture*>(raw);
    const size_t call = self.calls++;
    if (call == self.failAt) return false;
    self.values.emplace_back(token.data, token.length);
    return true;
  }

  nkl::OutputSink sink() { return {this, &SinkFixture::emit}; }
};

struct RunResult {
  nkl::Result result;
  std::vector<std::string> output;
  size_t sinkCalls;
};

static RunResult run(const std::string& arguments,
                     std::vector<std::string> current,
                     CatalogFixture& catalogFixture,
                     size_t sinkFailAt = static_cast<size_t>(-1)) {
  SequenceFixture sequence{std::move(current)};
  SinkFixture sink;
  sink.failAt = sinkFailAt;
  const nkl::Result result = nkl::mutate(
      view(arguments), sequence.sequence(), catalogFixture.catalog(), sink.sink());
  return {result, std::move(sink.values), sink.calls};
}

static void checkOutput(const std::vector<std::string>& actual,
                        std::initializer_list<const char*> expected,
                        const char* message) {
  if (actual.size() != expected.size()) {
    CHECK(false, message);
    return;
  }
  size_t index = 0;
  for (const char* value : expected) {
    if (actual[index] != value) {
      CHECK(false, message);
      return;
    }
    ++index;
  }
}

static void test_scratch_and_two_stage_api() {
  static_assert(nkl::kDeclaredCoreScratchBytes < 512,
                "core must retain its sub-512-byte scratch contract");
  static_assert(nkl::kMaxEntries == 256, "persisted-list bound drifted");

  CatalogFixture catalog;
  const std::string command = "patch +alpha,-beta";
  nkl::PreparedMutation prepared;
  nkl::Result result = nkl::prepare(view(command), catalog.catalog(), prepared);
  CHECK(result.ok(), "command-only prepare must succeed");
  CHECK(result.operation == nkl::Operation::Patch,
        "prepare must classify patch operation");

  SequenceFixture current{{"beta", "future_kind"}};
  SinkFixture sink;
  result = nkl::apply(prepared, current.sequence(), catalog.catalog(), sink.sink());
  CHECK(result.ok(), "prepared mutation must apply");
  CHECK(result.outputCount == 2 && result.emittedCount == 2,
        "apply must report planned and accepted output counts");
  checkOutput(sink.values, {"future_kind", "alpha"},
              "prepared apply must preserve current order then append additions");

  nkl::PreparedMutation unprepared;
  SinkFixture rejectedSink;
  result = nkl::apply(unprepared, current.sequence(), catalog.catalog(),
                      rejectedSink.sink());
  CHECK(result.status == nkl::Status::InvalidPreparedMutation,
        "apply must reject an unprepared plan");
  CHECK(rejectedSink.calls == 0, "invalid plans must not reach the sink");
}

static void test_legacy_replacement() {
  CatalogFixture catalog;
  RunResult result = run(
      " alpha, boot, beta, alpha, boot_finished, " + kLongCanonical + ",,",
      {"malformed stored value ignored by replacement"}, catalog);
  CHECK(result.result.ok(), "legacy replacement must succeed");
  CHECK(result.result.operation == nkl::Operation::Replace,
        "unreserved comma list must select replacement grammar");
  checkOutput(result.output,
              {"alpha", "boot_finished", "beta", kLongCanonical.c_str()},
              "replacement must canonicalize aliases, dedupe, and keep long known names");

  result = run("alpha,future_kind", {}, catalog);
  CHECK(result.result.status == nkl::Status::UnknownKind,
        "legacy replacement must reject unknown command tokens");
  CHECK(result.sinkCalls == 0,
        "unknown replacement input must fail before output begins");

  result = run(",,  ,", {}, catalog);
  CHECK(result.result.status == nkl::Status::InvalidGrammar,
        "empty legacy fields alone must not form a replacement");

  std::string tooMany;
  for (size_t i = 0; i < 257; ++i) {
    if (i) tooMany += ',';
    tooMany += "alpha";
  }
  result = run(tooMany, {}, catalog);
  CHECK(result.result.status == nkl::Status::TooManyEntries,
        "raw replacement input above 256 entries must fail even when duplicate");
  CHECK(result.sinkCalls == 0, "over-count replacement must not emit");
}

static void test_all_and_none_repair_bad_current_state() {
  CatalogFixture catalog;
  RunResult result = run("ALL", {"Bad Token", "set"}, catalog);
  CHECK(result.result.ok(), "all must ignore malformed current storage");
  checkOutput(result.output,
              {"alpha", "beta", "boot_finished", "gamma",
               kLongCanonical.c_str()},
              "all must use canonical catalog enumeration order");

  result = run("none", {"Bad Token", "set"}, catalog);
  CHECK(result.result.ok() && result.result.outputCount == 0,
        "none must repair malformed current storage to an empty list");
  CHECK(result.sinkCalls == 0, "none must not invoke the output sink");

  result = run("all extra", {}, catalog);
  CHECK(result.result.status == nkl::Status::InvalidGrammar,
        "reserved all grammar must reject trailing input");
  result = run("none extra", {}, catalog);
  CHECK(result.result.status == nkl::Status::InvalidGrammar,
        "reserved none grammar must reject trailing input");
}

static void test_set_preserves_and_canonicalizes_current() {
  CatalogFixture catalog;
  RunResult result = run(
      "set alpha ON",
      {"boot", "future_kind", "future_kind", "beta", "boot_finished"},
      catalog);
  CHECK(result.result.ok(), "set on must accept valid current storage");
  checkOutput(result.output,
              {"boot_finished", "future_kind", "beta", "alpha"},
              "set on must canonicalize/dedupe current and append a new kind");

  result = run("set beta off",
               {"boot", "future_kind", "beta", "gamma"}, catalog);
  CHECK(result.result.ok(), "set off must succeed");
  checkOutput(result.output, {"boot_finished", "future_kind", "gamma"},
              "set off must remove only the requested known kind");

  result = run("set boot on", {"boot_finished", "boot"}, catalog);
  CHECK(result.result.ok(), "set must resolve the legacy boot alias");
  checkOutput(result.output, {"boot_finished"},
              "known aliases in current storage must collapse canonically");

  result = run("set " + kLongCanonical + " on", {}, catalog);
  CHECK(result.result.ok(), "known canonical name over 63 bytes must remain legal");
  CHECK(result.output.size() == 1 && result.output[0] == kLongCanonical,
        "long known token must be emitted intact");

  result = run("set alpha maybe", {}, catalog);
  CHECK(result.result.status == nkl::Status::InvalidGrammar,
        "set state must be on or off");
  result = run("set unknown_kind on", {}, catalog);
  CHECK(result.result.status == nkl::Status::UnknownKind,
        "set must reject unknown delta kinds");
}

static void test_patch_is_strict_after_alias_canonicalization() {
  CatalogFixture catalog;
  RunResult result = run(
      "patch +alpha, -beta, +" + kLongCanonical,
      {"beta", "future_kind", "gamma"}, catalog);
  CHECK(result.result.ok(), "valid multi-kind patch must succeed");
  checkOutput(result.output,
              {"future_kind", "gamma", "alpha", kLongCanonical.c_str()},
              "patch must preserve retained current order and append additions in delta order");

  result = run("patch +boot,+boot_finished", {}, catalog);
  CHECK(result.result.status == nkl::Status::DuplicatePatchOperation,
        "alias-equivalent duplicate patch operations must fail");
  CHECK(result.sinkCalls == 0, "duplicate patch must not emit");

  result = run("patch +boot,-boot_finished", {}, catalog);
  CHECK(result.result.status == nkl::Status::ContradictoryPatchOperation,
        "alias-equivalent contradictory patch operations must fail");

  result = run("patch -alpha,-alpha", {}, catalog);
  CHECK(result.result.status == nkl::Status::DuplicatePatchOperation,
        "same-sign duplicate patch operations must fail");

  result = run("patch +future_kind", {}, catalog);
  CHECK(result.result.status == nkl::Status::UnknownKind,
        "patch must reject unknown additions");
  result = run("patch alpha", {}, catalog);
  CHECK(result.result.status == nkl::Status::InvalidGrammar,
        "patch operations require an explicit sign");
  result = run("patch + alpha", {}, catalog);
  CHECK(result.result.status == nkl::Status::InvalidGrammar,
        "patch must reject whitespace between sign and kind");
  result = run("patch +alpha,", {}, catalog);
  CHECK(result.result.status == nkl::Status::InvalidGrammar,
        "patch must reject an empty trailing operation");
}

static void test_preserved_unknown_validation_and_bounds() {
  CatalogFixture catalog;
  for (const std::string& reserved :
       {"boot", "none", "set", "patch", "all", "list"}) {
    CHECK(nkl::detail::reservedUnknown(view(reserved)),
          "every catalog-reserved token must be reserved in stored lists");
  }
  for (const std::string& neighbor :
       {"boot_finished", "nonevent", "setting_changed", "patch_applied",
        "all_clear", "listing"}) {
    CHECK(!nkl::detail::reservedUnknown(view(neighbor)),
          "stored-list reserved checks must match exact tokens only");
  }
  const std::vector<std::string> invalidStored = {
      "Future_kind", "future-kind", "future kind", "set", "patch", "all",
      "none", "list", std::string(64, 'x'), "",
  };
  for (const std::string& invalid : invalidStored) {
    RunResult result = run("set alpha on", {invalid}, catalog);
    CHECK(result.result.status == nkl::Status::InvalidStoredToken,
          "set must reject malformed/reserved preserved unknown tokens");
    CHECK(result.sinkCalls == 0,
          "stored-token validation must finish before the first sink call");
  }

  // `boot` is reserved as an unknown, but the production-style resolver sees
  // it first and canonicalizes the compatibility alias.
  RunResult result = run("set alpha off", {"boot"}, catalog);
  CHECK(result.result.ok(), "recognized boot alias must not be rejected as unknown");
  checkOutput(result.output, {"boot_finished"},
              "recognized boot alias must persist canonically");

  std::vector<std::string> overCount(257, "future_kind");
  result = run("set alpha off", overCount, catalog);
  CHECK(result.result.status == nkl::Status::TooManyEntries,
        "set/patch must reject raw current arrays above 256 entries");
  CHECK(result.sinkCalls == 0, "over-limit current data must not emit");

  std::vector<std::string> full;
  full.reserve(256);
  for (size_t i = 0; i < 256; ++i) full.push_back("u" + std::to_string(i));
  result = run("set alpha on", full, catalog);
  CHECK(result.result.status == nkl::Status::TooManyEntries,
        "adding a known kind to 256 unique unknowns must exceed output bound");
  CHECK(result.sinkCalls == 0,
        "computed output overflow must be caught before output begins");

  result = run("set alpha off", full, catalog);
  CHECK(result.result.ok() && result.result.outputCount == 256,
        "exact 256-entry preserved output must remain legal");
}

static void test_no_output_before_complete_current_validation() {
  CatalogFixture catalog;
  RunResult result = run("patch +alpha", {"future_kind", "Bad-Last"}, catalog);
  CHECK(result.result.status == nkl::Status::InvalidStoredToken,
        "late malformed current token must fail the whole patch");
  CHECK(result.sinkCalls == 0,
        "valid early entries must not emit before late validation finishes");

  SequenceFixture sequence{{"future_kind", "beta"}};
  sequence.failAt = 1;
  SinkFixture sink;
  const std::string command = "set alpha on";
  result.result = nkl::mutate(view(command), sequence.sequence(),
                              catalog.catalog(), sink.sink());
  CHECK(result.result.status == nkl::Status::CurrentVisitError,
        "current visitor failure must be explicit");
  CHECK(sink.calls == 0,
        "visitor failure during validation must happen before output");

  // Replacement does not depend on or inspect the current list.
  SequenceFixture ignored{{"bad"}};
  ignored.failAt = 0;
  SinkFixture replacementSink;
  const std::string replacement = "alpha";
  const nkl::Result replacementResult = nkl::mutate(
      view(replacement), ignored.sequence(), catalog.catalog(),
      replacementSink.sink());
  CHECK(replacementResult.ok() && ignored.calls == 0,
        "replacement must ignore malformed/unvisitable current storage");
}

static void test_sink_failure_and_exact_accepted_count() {
  CatalogFixture catalog;
  RunResult result = run("patch +alpha,+gamma", {"future_kind", "beta"},
                         catalog, 2);
  CHECK(result.result.status == nkl::Status::SinkFailed,
        "fallible output sink failure must propagate");
  CHECK(result.result.outputCount == 4,
        "sink failure must retain the fully validated planned count");
  CHECK(result.result.emittedCount == 2,
        "emitted count must exclude the rejected sink callback");
  CHECK(result.sinkCalls == 3,
        "sink must stop immediately after its first rejection");
  checkOutput(result.output, {"future_kind", "beta"},
              "only fully accepted sink entries may be retained");
}

static void test_catalog_contract_failures_preflight() {
  CatalogFixture invalidResolve;
  invalidResolve.invalidResolve = true;
  RunResult result = run("alpha", {}, invalidResolve);
  CHECK(result.result.status == nkl::Status::CatalogError,
        "resolver success with invalid canonical storage must be rejected");
  CHECK(result.sinkCalls == 0, "invalid resolver result must not emit");

  CatalogFixture duplicateIndex;
  duplicateIndex.entries.push_back({1, "delta"});
  result = run("all", {}, duplicateIndex);
  CHECK(result.result.status == nkl::Status::CatalogError,
        "all preflight must reject duplicate catalog kind indices");
  CHECK(result.sinkCalls == 0, "bad all enumeration must not partially emit");

  CatalogFixture failedEnumeration;
  failedEnumeration.failEnumeration = true;
  failedEnumeration.failEnumerationAt = 3;
  result = run("all", {}, failedEnumeration);
  CHECK(result.result.status == nkl::Status::CatalogError,
        "all preflight must reject an enumeration callback failure");
  CHECK(result.sinkCalls == 0,
        "late enumeration failure must occur before the first output callback");
}

static void test_status_names_are_total() {
  for (int value = static_cast<int>(nkl::Status::Ok);
       value <= static_cast<int>(nkl::Status::InconsistentCallbacks); ++value) {
    const char* name = nkl::statusName(static_cast<nkl::Status>(value));
    CHECK(name && std::string(name) != "invalid_status",
          "every public status must have a stable diagnostic token");
  }
}

int main() {
  test_scratch_and_two_stage_api();
  test_legacy_replacement();
  test_all_and_none_repair_bad_current_state();
  test_set_preserves_and_canonicalizes_current();
  test_patch_is_strict_after_alias_canonicalization();
  test_preserved_unknown_validation_and_bounds();
  test_no_output_before_complete_current_validation();
  test_sink_failure_and_exact_accepted_count();
  test_catalog_contract_failures_preflight();
  test_status_names_are_total();

  if (failures != 0) {
    std::fprintf(stderr, "%d notification-kind-list test(s) failed\n", failures);
    return 1;
  }
  std::puts("notification-kind-list tests passed");
  return 0;
}
