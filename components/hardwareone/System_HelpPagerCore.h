// System_HelpPagerCore.h - dependency-free, allocation-free help pagination.
//
// The firmware adapter owns text formatting and output. It reports the capture
// byte cost and logical-frame cost of each indivisible item to Pager, which
// returns that item's one-based page. No item text, page table, or callback is
// retained here, so the same input walk can first count pages and then emit one
// selected page without heap allocation.
#ifndef SYSTEM_HELPPAGERCORE_H
#define SYSTEM_HELPPAGERCORE_H

#include <stddef.h>

namespace hw1_help_pager {

enum class PageTokenStatus {
  NotPageToken,
  Invalid,
  Valid,
};

struct PageToken {
  PageTokenStatus status = PageTokenStatus::NotPageToken;
  size_t page = 0;
};

// Parse the canonical, one-based page token [pP][1-9][0-9]*.
//
// A token whose first byte is not p/P is NotPageToken, allowing a command
// parser to distinguish an unrelated argument from a malformed page request.
// Once p/P is present, every malformed spelling (including p, p0, p01,
// whitespace, a sign, a suffix, or size_t overflow) is Invalid.
inline PageToken parsePageToken(const char* token, size_t length) {
  PageToken result;
  if (!token || length == 0 || (token[0] != 'p' && token[0] != 'P')) {
    return result;
  }

  result.status = PageTokenStatus::Invalid;
  if (length < 2 || token[1] < '1' || token[1] > '9') return result;

  size_t value = 0;
  const size_t maxValue = static_cast<size_t>(-1);
  for (size_t i = 1; i < length; ++i) {
    const unsigned char byte = static_cast<unsigned char>(token[i]);
    if (byte < static_cast<unsigned char>('0') ||
        byte > static_cast<unsigned char>('9')) {
      return result;
    }
    const size_t digit = static_cast<size_t>(byte - '0');
    if (value > (maxValue - digit) / 10) return result;
    value = value * 10 + digit;
  }

  result.status = PageTokenStatus::Valid;
  result.page = value;
  return result;
}

// captureBytes includes every byte the command-capture buffer will consume for
// an item, including its terminating newline when the adapter emits one.
// logicalFrames is the number of independently queued display frames.
struct Cost {
  size_t captureBytes = 0;
  size_t logicalFrames = 0;
};

struct Limits {
  size_t maxCaptureBytes = 0;
  size_t maxLogicalFrames = 0;
};

struct Placement {
  size_t itemOrdinal = 0;
  size_t page = 0;
  Cost cost;
  bool oversizedItem = false;
};

inline bool costIsZero(Cost cost) {
  return cost.captureBytes == 0 && cost.logicalFrames == 0;
}

inline bool costFitsEmptyPage(Cost cost, Limits limits) {
  return cost.captureBytes <= limits.maxCaptureBytes &&
         cost.logicalFrames <= limits.maxLogicalFrames;
}

inline bool costFitsAfter(Cost used, Cost added, Limits limits) {
  return used.captureBytes <= limits.maxCaptureBytes &&
         used.logicalFrames <= limits.maxLogicalFrames &&
         added.captureBytes <= limits.maxCaptureBytes - used.captureBytes &&
         added.logicalFrames <= limits.maxLogicalFrames - used.logicalFrames;
}

// Stateful streaming page packer.
//
// Before adding a contiguous group, call beginGroup() with its total cost.
// When that group fits an empty page, Pager moves it wholesale to the next
// page if necessary. A group larger than an empty page starts on a fresh page
// and is then split at addItem() boundaries. An individually oversized item
// is placed alone, reported via oversizedItem, and seals its page; this is the
// only possible lossless behavior for an indivisible item and guarantees the
// following item makes forward progress.
class Pager {
 public:
  explicit Pager(Limits limits) : limits_(limits) {
    valid_ = limits_.maxCaptureBytes != 0 &&
             limits_.maxLogicalFrames != 0;
  }

  bool valid() const { return valid_; }

  void beginGroup(Cost groupCost) {
    if (!valid_ || pageItemCount_ == 0) return;

    // A fitting group is atomic. An oversized group also starts cleanly, then
    // addItem() splits it deterministically at item boundaries.
    if (!costFitsEmptyPage(groupCost, limits_) ||
        !costFitsAfter(used_, groupCost, limits_)) {
      advancePage();
    }
  }

  Placement addItem(Cost itemCost) {
    Placement placement;
    placement.itemOrdinal = itemCount_;
    placement.cost = itemCost;
    if (!valid_) return placement;

    if (pageSealed_ ||
        (pageItemCount_ != 0 &&
         !costFitsAfter(used_, itemCost, limits_))) {
      advancePage();
    }

    placement.page = currentPage_;
    placement.oversizedItem = !costFitsEmptyPage(itemCost, limits_);

    // Saturating accounting avoids wrap even for an impossible indivisible
    // item. A sealed oversized page is advanced before the next placement.
    used_.captureBytes = saturatingAdd(used_.captureBytes,
                                       itemCost.captureBytes);
    used_.logicalFrames = saturatingAdd(used_.logicalFrames,
                                        itemCost.logicalFrames);
    ++pageItemCount_;
    ++itemCount_;
    if (placement.oversizedItem) {
      ++oversizedItemCount_;
      pageSealed_ = true;
    }
    return placement;
  }

  size_t pageCount() const { return itemCount_ == 0 ? 0 : currentPage_; }
  size_t currentPage() const { return currentPage_; }
  Cost currentCost() const { return used_; }
  size_t itemCount() const { return itemCount_; }
  size_t oversizedItemCount() const { return oversizedItemCount_; }

 private:
  static size_t saturatingAdd(size_t left, size_t right) {
    const size_t maxValue = static_cast<size_t>(-1);
    return right > maxValue - left ? maxValue : left + right;
  }

  void advancePage() {
    ++currentPage_;
    used_ = Cost{};
    pageItemCount_ = 0;
    pageSealed_ = false;
  }

  Limits limits_;
  Cost used_;
  size_t currentPage_ = 1;
  size_t pageItemCount_ = 0;
  size_t itemCount_ = 0;
  size_t oversizedItemCount_ = 0;
  bool valid_ = false;
  bool pageSealed_ = false;
};

}  // namespace hw1_help_pager

#endif  // SYSTEM_HELPPAGERCORE_H
