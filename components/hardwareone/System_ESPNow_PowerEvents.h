// System_ESPNow_PowerEvents.h - battery state and system events over the mesh
//
// A device that is not the mesh master sends its battery state (POWER_STATUS,
// opcode 152) and its system events (EVENT_PUSH, opcode 132) to the masters
// named on it (espnowmasterfingerprint, espnowbackupfingerprint). A master or
// backup master keeps the latest battery state of each paired sender and a
// list of recent events per mesh. Nothing here needs a bond: the frames are
// session unicasts between paired devices. The rules are in
// System_ESPNow_PowerEventCore.h; this part reads the battery, the event ring
// and the settings, sends the frames and holds what was received.
#ifndef SYSTEM_ESPNOW_POWEREVENTS_H
#define SYSTEM_ESPNOW_POWEREVENTS_H

#include "System_BuildConfig.h"

#if ENABLE_ESPNOW

#include <Arduino.h>
#include <stdint.h>

#include "System_ESPNow_PowerEventCore.h"

struct V4RxCtx;

// Receive handlers (espnow_task). The receive table has already required a
// paired sender and a session-encrypted frame; a device whose role does not
// receive (a worker) refuses the frame and counts it.
void v4hPowerStatus(const V4RxCtx& ctx);
void v4hEventPush(const V4RxCtx& ctx);

// espnow_task loop, after sensorLinkTick: sends this device's battery state
// when it is due and its new events (at most four a second), and starts a
// session with a named master that has none.
void espnowPowerEventsTick(uint32_t nowMs);

// What this device sends, for status displays. Any task.
struct PowerEventsSendView {
  espnowsensor::PushWhyNot batteryWhy;
  uint16_t batteryIntervalS;
  uint32_t batterySent;
  bool     batteryEverSent;
  uint32_t batteryLastSentMs;
  espnowsensor::PushWhyNot eventsWhy;
  uint8_t  eventFloor;      // the setting eventPush
  uint32_t eventsSent;
  uint32_t eventsLeftOut;   // rate limited, or overwritten in the ring before they could be sent
};
void espnowPowerEventsSendView(PowerEventsSendView* out);

// Whether the user the current command runs for may see the event counts of
// other devices: an admin only, as espnowevents (events carry logins and
// failures). The two functions below leave the counts out for anyone else.
bool espnowEventCountsVisible();
// What this device keeps of one sender, for 'espnowsensorstatus json <device>':
// its latest battery state as one JSON object (the row of 'espnowbattery
// json'), and its event counts as one JSON object (admin only). 0 when
// nothing is kept (or the counts are not for this user).
size_t espnowBatteryJsonFor(const uint8_t mac[6], char* out, size_t cap);
size_t espnowEventCountsJsonFor(const uint8_t mac[6], char* out, size_t cap);
// The same in a few words ("battery 71.5 % 12 s ago, 3 events"; the events
// for an admin only); 0 when nothing is kept.
size_t espnowPowerEventsPeerText(const uint8_t mac[6], char* out, size_t cap);

const char* cmd_espnow_battery(const String& argsInput);
const char* cmd_espnow_events(const String& argsInput);

#endif  // ENABLE_ESPNOW
#endif  // SYSTEM_ESPNOW_POWEREVENTS_H
