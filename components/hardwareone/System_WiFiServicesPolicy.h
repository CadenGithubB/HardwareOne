#ifndef HARDWAREONE_SYSTEM_WIFI_SERVICES_POLICY_H
#define HARDWAREONE_SYSTEM_WIFI_SERVICES_POLICY_H

// Whether a Wi-Fi join that happened after boot starts the web server. Boot
// used to be the only place HTTP autostart ran, so a device that joined later
// (the rehunt after a failed boot connect, the driver's own rejoin, the radio
// companion's restore, `radiopower on`, `openwifi`) stayed without its web
// server until `openhttp`.
//
// Dependency-free (host test: test_wifi_rehunt_policy.cpp). System_WiFi.cpp
// reads it from the network-services job that every join submits to cmd_exec
// (wifiReconcileNetworkServicesJob), so it runs in order with every
// `openhttp` and `closehttp`.
//
// Rules:
//  - Only with autostart wanted (httpautostart, as boot resolves it, RAM-flush
//    overlay included).
//  - Never a second start while one runs.
//  - `closehttp` is the user's word until `openhttp`: a join does not undo it.
//    `closewifi` stopping the server is not a user close, so the next join
//    brings it back.
//  - A join the boot connect made belongs to boot: boot starts the services
//    for it (in setup, or on its network task after a hand-off).
struct WifiServicesInputs {
  bool autostart = false;          // autostart wanted, as boot resolves it
  bool running = false;            // the server is up
  bool userClosed = false;         // closehttp since the last openhttp
  bool bootConnectActive = false;  // the boot connect owns the radio and its services
};

inline bool wifiServicesShouldStartHttp(const WifiServicesInputs& in) {
  return in.autostart && !in.running && !in.userClosed &&
         !in.bootConnectActive;
}

#endif  // HARDWAREONE_SYSTEM_WIFI_SERVICES_POLICY_H
