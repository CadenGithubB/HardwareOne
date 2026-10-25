#pragma once
#include <cstdint>
enum class BleCentralLink : uint8_t { Left, Right, Ring, Phone };
// Link capacity and the recovery block. Who sets up next is the setup worker's
// call (BLE_SetupWorker.h): one central setup at a time, glasses first.
// Reservations count from before client creation until confirmed retirement.
// Token matching prevents a late release from retiring a newer reservation.
uint8_t bleCentralLinkLimit();
// Published by the pipe runtime, not merely its compile-time availability.
void bleCentralPhoneEnabled(bool enabled);
bool bleCentralRingShouldYield();
uint32_t bleCentralLinkReserve(BleCentralLink link, bool reserveArms=false);
void bleCentralLinkBind(uint32_t token,const void* client);
void bleCentralLinkRelease(uint32_t token);
void bleCentralLinkReleaseClient(const void* client);
void bleCentralLinksHostStopped(); // verified terminal host/controller teardown only
void bleCentralRecoveryBlock(bool blocked);
bool bleCentralRecoveryBlocked();
// A reservation (pending or live) is held for this link.
bool bleCentralLinkHeld(BleCentralLink link);
class BleCentralLinkGuard {
 public:
  explicit BleCentralLinkGuard(BleCentralLink link,bool reserveArms=false)
      : token_(bleCentralLinkReserve(link,reserveArms)) {}
  ~BleCentralLinkGuard(){if(!retained_)bleCentralLinkRelease(token_);}
  explicit operator bool() const{return token_!=0;}
  void retain(const void* client){bleCentralLinkBind(token_,client);retained_=true;}
  BleCentralLinkGuard(const BleCentralLinkGuard&)=delete;
  BleCentralLinkGuard& operator=(const BleCentralLinkGuard&)=delete;
 private: uint32_t token_; bool retained_=false;
};
