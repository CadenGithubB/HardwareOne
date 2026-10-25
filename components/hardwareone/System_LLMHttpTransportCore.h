#pragma once
#include <cstdint>
#include "BLE_PhonePipeCore.h"  // hw1pipe::knownError: the pipe's error codes
namespace hw1httptransport {
enum class Path : uint8_t { None, Wifi, Phone };
enum class Failure : uint8_t { Network, Trust, Cancelled, Protocol, Resources };
// One instance belongs to one logical HTTP request. Header writes count even if
// the transport reports failure: remote receipt cannot be inferred from errno.
class Attempt {
 public:
  Path path=Path::None,first=Path::None;
  bool mayHaveSent=false,finished=false;
  bool select(Path p) {
    if(finished || mayHaveSent || p==Path::None || (tried_ & pathBit(p))) return false;
    if(first==Path::None) first=p;
    path=p; tried_|=pathBit(p); return true;
  }
  // F4/F5: after a Network failure before any HTTP write, the request may use
  // the other path once. Each path is tried at most once per request, so
  // Wi-Fi -> phone -> Wi-Fi (or the reverse) cannot happen.
  bool fallback(Failure why,Path next,bool nextAvailable) const {
    return !finished && !mayHaveSent && path!=Path::None && next!=Path::None && next!=path &&
           why==Failure::Network && nextAvailable && !(tried_ & pathBit(next));
  }
  bool tried(Path p) const {return (tried_ & pathBit(p))!=0;}
  void beforeHttpWrite() { mayHaveSent=true; }
  void finish() { finished=true; }
 private:
  uint8_t tried_=0;
  static constexpr uint8_t pathBit(Path p) {return uint8_t(1u<<unsigned(p));}
};
// Reachability hint for one path. Call under the shared path-health lock. The
// context is (Wi-Fi association epoch or phone session generation, config
// revision); a new context drops the hint. No token, address or SSID is kept.
// A negative path is a routing preference, never a ban: it is still used when
// it is the only path, or once per request after the other path failed.
class Health {
 public:
  void context(uint32_t epoch,uint32_t configRevision) {
    if(epoch!=epoch_ || configRevision!=revision_) {
      epoch_=epoch; revision_=configRevision; negative_=probing_=false;
    }
  }
  void networkFailed(uint32_t now) {failedAt_=now;negative_=true;probing_=false;}
  void succeeded() {negative_=probing_=false;}
  bool eligible() const {return !negative_;}
  bool claimProbe(uint32_t now) {
    if(!negative_ || probing_ || uint32_t(now-failedAt_)<30000) return false;
    probing_=true; return true;
  }
  void endProbe(bool success,uint32_t now) {if(success) succeeded();else networkFailed(now);}
 private:
  uint32_t epoch_=0,revision_=0,failedAt_=0;
  bool negative_=false,probing_=false;
};
// F5: how a phone OPEN_ERR (or a RESET before OPEN_OK) routes the request. An
// OPEN_ERR always precedes OPEN_OK, so no HTTP byte can have gone out.
//  5 VPN/route, 6 DNS, 7 connect, 8 connect timeout, 11 TCP lost before OPEN_OK:
//    the phone's network path failed -> Network (phone marked negative for
//    30 s; the request may use Wi-Fi once).
//  9 both stream slots busy, 10 phone resources: transient, no penalty, no
//    other path for this request -> Resources.
//  16 service off (the phone's relay is off or has no destination): its relay
//    is not usable now -> Network (Wi-Fi once; phone marked negative 30 s).
//  14 cancelled -> Cancelled. 17 unsupported, 4 target not allowed and anything
//    else -> Protocol (a policy or protocol fault that another path must not bypass).
inline Failure openRefusal(uint16_t code) {
  switch(code) {
    case 5: case 6: case 7: case 8: case 11: case 16: return Failure::Network;
    case 9: case 10: return Failure::Resources;
    case 14: return Failure::Cancelled;
    case 17: return Failure::Protocol;
    default: return Failure::Protocol;
  }
}
// `relay status` counts refusals per pipe error code; a code the pipe does not
// define shares bucket 0 and never folds onto a known one.
constexpr unsigned kRefusalCodes = unsigned(hw1pipe::Error::Paused) + 1;
inline unsigned refusalSlot(uint16_t code) { return hw1pipe::knownError(code) ? code : 0; }
}
