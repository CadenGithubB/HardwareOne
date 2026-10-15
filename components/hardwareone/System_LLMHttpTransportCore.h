#pragma once
#include <cstdint>
namespace hw1httptransport {
enum class Path : uint8_t { None, Wifi, Phone };
enum class Failure : uint8_t { Network, Trust, Cancelled, Protocol, Resources };
// One instance belongs to one logical HTTP request. Header writes count even if
// the transport reports failure: remote receipt cannot be inferred from errno.
class Attempt {
 public:
  Path path=Path::None;
  bool mayHaveSent=false,finished=false;
  bool select(Path p) {
    if(finished || mayHaveSent || p==Path::None || (tried_ & pathBit(p))) return false;
    path=p; tried_|=pathBit(p); return true;
  }
  bool fallback(Failure why,bool phoneReady) const {
    return !finished && !mayHaveSent && path==Path::Wifi && why==Failure::Network &&
           phoneReady && !(tried_ & pathBit(Path::Phone));
  }
  void beforeHttpWrite() { mayHaveSent=true; }
  void finish() { finished=true; }
 private:
  uint8_t tried_=0;
  static constexpr uint8_t pathBit(Path p) {return uint8_t(1u<<unsigned(p));}
};
// Call under the shared path-health lock; association/config epochs invalidate
// only reachability hints. No token, address or SSID is retained here.
class Health {
 public:
  void context(uint32_t wifiEpoch,uint32_t configRevision) {
    if(wifiEpoch!=wifiEpoch_ || configRevision!=revision_) {
      wifiEpoch_=wifiEpoch; revision_=configRevision; negative_=probing_=false;
    }
  }
  void networkFailed(uint32_t now) {failedAt_=now;negative_=true;probing_=false;}
  void succeeded() {negative_=probing_=false;}
  bool directEligible() const {return !negative_;}
  bool claimProbe(uint32_t now) {
    if(!negative_ || probing_ || uint32_t(now-failedAt_)<30000) return false;
    probing_=true; return true;
  }
  void endProbe(bool success,uint32_t now) {if(success) succeeded();else networkFailed(now);}
 private:
  uint32_t wifiEpoch_=0,revision_=0,failedAt_=0;
  bool negative_=false,probing_=false;
};
}
