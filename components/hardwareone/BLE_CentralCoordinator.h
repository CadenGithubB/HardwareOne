#pragma once
#include <cstdint>
// Serialize setup only; established links never hold this gate. Accessory work
// claims priority before waiting, so the phone cannot repeatedly win admission.
bool bleCentralSetupTake(bool phone,uint32_t timeoutMs);
void bleCentralSetupGive();
bool bleCentralAccessoryPending();
// Includes a setup currently holding the gate, for inbound credit throttling.
bool bleCentralAccessoryBusy();
class BleCentralSetupGuard {
 public:
  explicit BleCentralSetupGuard(bool phone,uint32_t timeoutMs):held_(bleCentralSetupTake(phone,timeoutMs)) {}
  ~BleCentralSetupGuard(){if(held_)bleCentralSetupGive();}
  explicit operator bool() const{return held_;}
 private: bool held_;
};
