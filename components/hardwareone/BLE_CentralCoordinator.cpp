#include "BLE_CentralCoordinator.h"
#include "HAL_Radio.h"
#include <sdkconfig.h>
#include <freertos/FreeRTOS.h>
static portMUX_TYPE sLinksMux=portMUX_INITIALIZER_UNLOCKED;
struct LinkSlot {uint32_t token=0;const void* client=nullptr;};
static LinkSlot sLinks[4];
static uint32_t sNextToken=0;
static bool recoveryBlocked=false,phoneEnabled=false;
uint8_t bleCentralLinkLimit() {
#ifdef CONFIG_BT_ACL_CONNECTIONS
  const uint8_t host=CONFIG_BT_ACL_CONNECTIONS<4?CONFIG_BT_ACL_CONNECTIONS:4;
#else
  const uint8_t host=0;
#endif
  const uint8_t radio=radioHalCompanionBleLinks();
  return radio<host?radio:host;
}
void bleCentralPhoneEnabled(bool enabled) {
  portENTER_CRITICAL(&sLinksMux);phoneEnabled=enabled;portEXIT_CRITICAL(&sLinksMux);
}
bool bleCentralRingShouldYield() {
  const uint8_t limit=bleCentralLinkLimit();
  portENTER_CRITICAL(&sLinksMux);const bool yield=phoneEnabled && limit<4;portEXIT_CRITICAL(&sLinksMux);
  return yield;
}
uint32_t bleCentralLinkReserve(BleCentralLink link,bool reserveArms) {
  const unsigned index=static_cast<unsigned>(link);
  const uint8_t limit=bleCentralLinkLimit();
  if(index>=4)return 0;
  portENTER_CRITICAL(&sLinksMux);
  unsigned used=0;
  for(unsigned i=0;i<4;++i)used+=(sLinks[i].token!=0 || (reserveArms && i<2));
  uint32_t token=0;
  if(!recoveryBlocked && !(link==BleCentralLink::Ring && phoneEnabled && limit<4) &&
     !sLinks[index].token && used<limit) {
    if(++sNextToken==0)++sNextToken;
    token=sNextToken;sLinks[index]={token,nullptr};
  }
  portEXIT_CRITICAL(&sLinksMux);
  return token;
}
void bleCentralLinkBind(uint32_t token,const void* client) {
  if(!token)return;
  portENTER_CRITICAL(&sLinksMux);
  for(auto& slot:sLinks)if(slot.token==token)slot.client=client;
  portEXIT_CRITICAL(&sLinksMux);
}
void bleCentralLinkRelease(uint32_t token) {
  if(!token)return;
  portENTER_CRITICAL(&sLinksMux);
  for(auto& slot:sLinks)if(slot.token==token)slot={};
  portEXIT_CRITICAL(&sLinksMux);
}
void bleCentralLinkReleaseClient(const void* client) {
  if(!client)return;
  portENTER_CRITICAL(&sLinksMux);
  for(auto& slot:sLinks)if(slot.client==client)slot={};
  portEXIT_CRITICAL(&sLinksMux);
}
void bleCentralLinksHostStopped() {
  portENTER_CRITICAL(&sLinksMux);
  for(auto& slot:sLinks)slot={};
  portEXIT_CRITICAL(&sLinksMux);
}
void bleCentralRecoveryBlock(bool blocked) {
  portENTER_CRITICAL(&sLinksMux);recoveryBlocked=blocked;portEXIT_CRITICAL(&sLinksMux);
}
bool bleCentralLinkHeld(BleCentralLink link) {
  const unsigned index=static_cast<unsigned>(link);
  if(index>=4)return false;
  portENTER_CRITICAL(&sLinksMux);const bool held=sLinks[index].token!=0;portEXIT_CRITICAL(&sLinksMux);
  return held;
}
bool bleCentralRecoveryBlocked() {
  portENTER_CRITICAL(&sLinksMux);const bool blocked=recoveryBlocked;portEXIT_CRITICAL(&sLinksMux);
  return blocked;
}
